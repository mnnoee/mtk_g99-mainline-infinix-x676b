// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015 MediaTek Inc.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/interrupt.h>
#include <linux/mailbox_controller.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/soc/mediatek/mtk-cmdq.h>
#include <linux/soc/mediatek/mtk-mmsys.h>
#include <linux/soc/mediatek/mtk-mutex.h>

#include <asm/barrier.h>
#include <asm/fpsimd.h>
#include <asm/neon.h>
#include <asm/simd.h>

#include <drm/drm_atomic.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_modes.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "mtk_crtc.h"
#include "mtk_ddp_comp.h"
#include "mtk_drm_drv.h"
#include "mtk_gem.h"
#include "mtk_plane.h"

/*
 * struct mtk_crtc - MediaTek specific crtc structure.
 * @base: crtc object.
 * @enabled: records whether crtc_enable succeeded
 * @planes: array of 4 drm_plane structures, one for each overlay plane
 * @pending_planes: whether any plane has pending changes to be applied
 * @mmsys_dev: pointer to the mmsys device for configuration registers
 * @mutex: handle to one of the ten disp_mutex streams
 * @ddp_comp_nr: number of components in ddp_comp
 * @ddp_comp: array of pointers the mtk_ddp_comp structures used by this crtc
 *
 * TODO: Needs update: this header is missing a bunch of member descriptions.
 */
struct mtk_crtc {
	struct drm_crtc			base;
	bool				enabled;

	bool				pending_needs_vblank;
	struct drm_pending_vblank_event	*event;

	struct drm_plane		*planes;
	unsigned int			layer_nr;
	bool				pending_planes;
	bool				pending_async_planes;

#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	struct cmdq_client		cmdq_client;
	struct cmdq_pkt			cmdq_handle;
	u32				cmdq_event;
	u32				cmdq_vblank_cnt;
	wait_queue_head_t		cb_blocking_queue;
#endif

	struct device			*mmsys_dev;
	struct device			*dma_dev;
	struct mtk_mutex		*mutex;
	unsigned int			ddp_comp_nr;
	struct mtk_ddp_comp		**ddp_comp;
	unsigned int			num_conn_routes;
	const struct mtk_drm_route	*conn_routes;

	/* lock for display hardware access */
	struct mutex			hw_lock;
	bool				config_updating;
	/* lock for config_updating to cmd buffer */
	spinlock_t			config_lock;

	/* command-mode panels need a frame trigger + fake vblank */
	struct delayed_work		cmd_mode_work;
	dma_addr_t			cmd_fb_addr;
	unsigned int			cmd_fb_pitch;
	/*
	 * Two packed RGB888 frames, used alternately: the DSI reads frame N
	 * from one buffer while the CPU converts frame N+1 into the other,
	 * so the conversion no longer adds to the frame period.
	 */
	void				*cmd_conv_buf[2];
	dma_addr_t			cmd_conv_dma[2];
	unsigned int			cmd_conv_idx;
	/*
	 * Snapshot of the plane-0 framebuffer, taken in atomic_begin() where
	 * the modeset locks are held.  The command-mode worker must not take
	 * those locks: the atomic commit that waits for the flip completion
	 * holds them, so locking there deadlocks the commit against itself.
	 */
	struct drm_framebuffer		*cmd_fb_cur;
	dma_addr_t			cmd_fb_cur_addr;
	spinlock_t			cmd_fb_lock;
	/* panel TE pin: paces command-mode frames instead of a timer */
	struct gpio_desc		*te_gpio;
	int				te_irq;
	struct device			*te_dev;
	bool				te_mode;
	u64				te_count;
	u64				te_seen;
	u64				work_count;
	u64				trig_count;
	u64				w_busy;
	u64				w_nofb;
	u64				w_upd;
	u64				w_maxms;
	u64				w_sum_ms;
	u64				w_sync_ms;
	u64				w_cfg_ms;
	u64				w_tail_ms;
};

struct mtk_crtc_state {
	struct drm_crtc_state		base;

	bool				pending_config;
	unsigned int			pending_width;
	unsigned int			pending_height;
	unsigned int			pending_vrefresh;
};

static inline struct mtk_crtc *to_mtk_crtc(struct drm_crtc *c)
{
	return container_of(c, struct mtk_crtc, base);
}

static inline struct mtk_crtc_state *to_mtk_crtc_state(struct drm_crtc_state *s)
{
	return container_of(s, struct mtk_crtc_state, base);
}

static void mtk_crtc_finish_page_flip(struct mtk_crtc *mtk_crtc)
{
	struct drm_crtc *crtc = &mtk_crtc->base;
	unsigned long flags;

	if (mtk_crtc->event) {
		spin_lock_irqsave(&crtc->dev->event_lock, flags);
		drm_crtc_send_vblank_event(crtc, mtk_crtc->event);
		drm_crtc_vblank_put(crtc);
		mtk_crtc->event = NULL;
		spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
	}
}

static void mtk_drm_finish_page_flip(struct mtk_crtc *mtk_crtc)
{
	unsigned long flags;

	drm_crtc_handle_vblank(&mtk_crtc->base);

#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	if (mtk_crtc->cmdq_client.chan)
		return;
#endif

	spin_lock_irqsave(&mtk_crtc->config_lock, flags);
	if (!mtk_crtc->config_updating && mtk_crtc->pending_needs_vblank) {
		mtk_crtc_finish_page_flip(mtk_crtc);
		mtk_crtc->pending_needs_vblank = false;
	}
	spin_unlock_irqrestore(&mtk_crtc->config_lock, flags);
}

static bool cmd_tick = true;
static bool cmd_ddp_irq = false;
static bool cmd_memory_mode = true;
module_param(cmd_memory_mode, bool, 0644);
MODULE_PARM_DESC(cmd_memory_mode, "Pull frames through RDMA memory mode (software RGB888 conversion)");
static bool te_pacing = true;
module_param(te_pacing, bool, 0644);
MODULE_PARM_DESC(te_pacing, "Pace command-mode frames with the panel TE pin instead of a timer");
static unsigned int cmd_period_ms;
module_param(cmd_period_ms, uint, 0644);
MODULE_PARM_DESC(cmd_period_ms, "Command-mode frame period in ms (0 = follow the mode refresh rate)");
module_param(cmd_ddp_irq, bool, 0644);
MODULE_PARM_DESC(cmd_ddp_irq, "Apply pending CRTC config from the command-mode tick");
module_param(cmd_tick, bool, 0644);
MODULE_PARM_DESC(cmd_tick, "Command-mode frame trigger tick (disable for single-shot bring-up tests)");

static bool cmd_sync_enable = true;
module_param(cmd_sync_enable, bool, 0644);
MODULE_PARM_DESC(cmd_sync_enable, "Enable dma_sync on the command-mode framebuffer (disabling it yields a black screen)");

static void mtk_crtc_ddp_irq(void *data);

/*
 * MT6789 command mode: DSI always sends 24bpp, while the DRM fbcon buffer
 * is XRGB8888 (32bpp). Convert the scanout buffer to packed RGB888 for the
 * RDMA memory-mode path.
 */
/*
 * NEON version of the XRGB8888 -> packed RGB888 conversion. The scalar
 * and DRM-helper loops are byte oriented and cost most of a CPU core at
 * 30 fps; ld4/st3 move 16 pixels per iteration.
 */
static void mtk_crtc_cmd_mode_convert_neon(u8 *dst, const u8 *src,
					   unsigned int width,
					   unsigned int height,
					   unsigned int src_pitch)
{
	struct user_fpsimd_state fpsimd_state;
	unsigned int x, y;

	/*
	 * The workqueue is preemptible, so the FP/SIMD state must be backed
	 * by a caller-provided buffer: with NULL the kernel would save the
	 * state to a NULL pointer when this task is preempted.
	 */
	kernel_neon_begin(&fpsimd_state);
	for (y = 0; y < height; y++) {
		const u8 *s = src + (size_t)y * src_pitch;
		u8 *d = dst + (size_t)y * width * 3;

		for (x = 0; x + 16 <= width; x += 16) {
			asm volatile(
				"ld4 {v0.16b, v1.16b, v2.16b, v3.16b}, [%0], #64\n"
				"st3 {v0.16b, v1.16b, v2.16b}, [%1], #48\n"
				: "+r"(s), "+r"(d)
				:
				: "v0", "v1", "v2", "v3", "memory");
		}
		for (; x < width; x++) {
			d[0] = s[0];
			d[1] = s[1];
			d[2] = s[2];
			s += 4;
			d += 3;
		}
	}
	kernel_neon_end(&fpsimd_state);
}

static void mtk_crtc_cmd_mode_convert(struct mtk_crtc *mtk_crtc,
				      struct drm_framebuffer *fb, void *dst_buf)
{
	struct drm_gem_dma_object *obj = drm_fb_dma_get_gem_obj(fb, 0);
	const u8 *src;

	if (!dst_buf || !obj || !obj->vaddr)
		return;

	src = (const u8 *)obj->vaddr + fb->offsets[0];

	if (may_use_simd()) {
		mtk_crtc_cmd_mode_convert_neon(dst_buf, src,
					       fb->width, fb->height,
					       fb->pitches[0]);
		return;
	}

	{
		struct drm_format_conv_state fmtcnv_state =
			DRM_FORMAT_CONV_STATE_INIT;
		struct iosys_map dst, srcmap;
		unsigned int dst_pitch[1];
		struct drm_rect clip;

		clip.x1 = 0;
		clip.y1 = 0;
		clip.x2 = fb->width;
		clip.y2 = fb->height;
		dst_pitch[0] = fb->width * 3;

		iosys_map_set_vaddr(&srcmap, (void *)src);
		iosys_map_set_vaddr(&dst, dst_buf);

		drm_fb_xrgb8888_to_rgb888(&dst, dst_pitch, &srcmap, fb, &clip,
					  &fmtcnv_state);
		drm_format_conv_state_release(&fmtcnv_state);
	}
}

/*
 * The command-mode tick runs outside any atomic commit, so it must not
 * dereference plane->state directly: fbcon commits replace the state and
 * free the old one concurrently.  Take a snapshot while the modeset locks
 * are held (atomic_begin) and keep a reference to the framebuffer for as
 * long as it is used.
 */
static void
mtk_crtc_cmd_cache_fb(struct mtk_crtc *mtk_crtc)
{
	struct drm_plane *plane = &mtk_crtc->planes[0];
	struct drm_framebuffer *fb = NULL, *old;
	dma_addr_t addr = 0;
	unsigned long flags;

	if (plane->state && plane->state->fb) {
		fb = plane->state->fb;
		addr = drm_fb_dma_get_gem_addr(fb, plane->state, 0);
		drm_framebuffer_get(fb);
	}

	spin_lock_irqsave(&mtk_crtc->cmd_fb_lock, flags);
	old = mtk_crtc->cmd_fb_cur;
	mtk_crtc->cmd_fb_cur = fb;
	mtk_crtc->cmd_fb_cur_addr = addr;
	spin_unlock_irqrestore(&mtk_crtc->cmd_fb_lock, flags);

	if (old)
		drm_framebuffer_put(old);
}

static struct drm_framebuffer *
mtk_crtc_cmd_peek_fb(struct mtk_crtc *mtk_crtc, dma_addr_t *addr)
{
	struct drm_framebuffer *fb;
	unsigned long flags;

	spin_lock_irqsave(&mtk_crtc->cmd_fb_lock, flags);
	fb = mtk_crtc->cmd_fb_cur;
	if (fb)
		drm_framebuffer_get(fb);
	*addr = mtk_crtc->cmd_fb_cur_addr;
	spin_unlock_irqrestore(&mtk_crtc->cmd_fb_lock, flags);

	return fb;
}

static irqreturn_t mtk_crtc_te_isr(int irq, void *data)
{
	struct mtk_crtc *mtk_crtc = data;

	if (!mtk_crtc->te_mode || !mtk_crtc->enabled || !mtk_crtc->ddp_comp_nr)
		return IRQ_HANDLED;

	mtk_crtc->te_count++;
	schedule_delayed_work(&mtk_crtc->cmd_mode_work, 0);

	return IRQ_HANDLED;
}

static unsigned int mtk_crtc_cmd_period_ms(struct mtk_crtc *mtk_crtc)
{
	unsigned int vrefresh = drm_mode_vrefresh(&mtk_crtc->base.mode);

	if (cmd_period_ms)
		return cmd_period_ms;
	if (!vrefresh)
		return 33;

	return DIV_ROUND_UP(1000, vrefresh);
}

static void mtk_crtc_cmd_mode_work(struct work_struct *work)
{
	struct mtk_crtc *mtk_crtc = container_of(work, struct mtk_crtc,
						 cmd_mode_work.work);
	struct drm_framebuffer *fb;
	dma_addr_t addr;
	bool need_config = false;
	ktime_t tick_start = ktime_get();
	unsigned int buf_idx;
	int i;

	if (!mtk_crtc->enabled)
		return;

	mtk_crtc->work_count++;

	/* Do not touch the pipeline while an atomic commit is applying. */
	if (mtk_crtc->config_updating) {
		mtk_crtc->w_upd++;
		goto resched;
	}

	/*
	 * Pace the frames with the hardware.  Reprogramming the RDMA while
	 * the DSI is still clocking out the previous frame makes that frame
	 * switch buffers mid-stream, which shows up as the picture jumping
	 * vertically.  Wait for the next idle window instead of guessing.
	 */
	if (mtk_ddp_comp_is_busy(mtk_crtc->ddp_comp[mtk_crtc->ddp_comp_nr - 1])) {
		mtk_crtc->w_busy++;
		goto resched;
	}

	fb = mtk_crtc_cmd_peek_fb(mtk_crtc, &addr);
	if (!fb)
		mtk_crtc->w_nofb++;
	if (!mtk_crtc->enabled) {
		if (fb)
			drm_framebuffer_put(fb);
		return;
	}

	/*
	 * This is the vblank callback the hardware would normally call: it
	 * applies pending config (non shadow-register SoCs) and completes
	 * page flips. Command mode panels get no hardware vblank, so we run
	 * it from the trigger tick.
	 */
	if (cmd_ddp_irq && fb && addr != mtk_crtc->cmd_fb_addr) {
		mtk_crtc->pending_planes = true;
		mtk_crtc->cmd_fb_addr = addr;
		need_config = true;
	}

	/*
	 * Apply pending CRTC/OVL config only when the scanout buffer
	 * changed: running ddp_config while frames are flowing breaks
	 * the transfer. Page flips are completed every tick.
	 */
	if (need_config)
		mtk_crtc_ddp_irq(&mtk_crtc->base);
	else
		mtk_drm_finish_page_flip(mtk_crtc);

	if (!cmd_memory_mode && fb && addr != mtk_crtc->cmd_fb_addr) {
		/*
		 * Direct path: the OVL samples the buffer userspace rendered
		 * (ARGB8888) and converts it to RGB888 in hardware on the way
		 * to the DSI, exactly like the vendor pipeline does.  Nothing
		 * is copied or converted in software.
		 *
		 * Mark the plane state dirty so the regular OVL/RDMA layer
		 * configuration picks up the new address, then let
		 * mtk_crtc_ddp_irq() below program the pipeline.
		 */
		mtk_crtc->pending_planes = true;
		mtk_crtc->cmd_fb_addr = addr;
		need_config = true;
	}

	if (cmd_memory_mode && fb &&
	    (fb->format->format == DRM_FORMAT_RGB888 ||
	     fb->format->format == DRM_FORMAT_BGR888)) {
		/*
		 * The framebuffer is already packed 24 bpp, which is exactly
		 * what the DSI sends: give the buffer to the RDMA as-is and
		 * skip the software conversion entirely.
		 */
		{
			ktime_t ts = ktime_get();

			if (cmd_sync_enable)
				dma_sync_single_for_device(mtk_crtc->base.dev->dev, addr,
							   fb->pitches[0] * fb->height,
							   DMA_TO_DEVICE);
			mtk_crtc->w_sync_ms += ktime_ms_delta(ktime_get(), ts);
		}
		/*
		 * Only reprogram RDMA when the scanout address changed: the
		 * address/pitch programming resets the RDMA FIFO and breaks the
		 * in-flight transfer, so doing it every frame (even when the
		 * same buffer is reused) caps the panel at ~half the frame rate.
		 */
		if (addr != mtk_crtc->cmd_fb_addr) {
			for (i = 0; i < mtk_crtc->ddp_comp_nr; i++) {
				struct mtk_ddp_comp *comp = mtk_crtc->ddp_comp[i];

				if (comp->funcs && comp->funcs->mem_config) {
					ktime_t ts = ktime_get();

					comp->funcs->mem_config(comp->dev, addr,
								fb->pitches[0],
								fb->format->format);
					mtk_crtc->w_cfg_ms += ktime_ms_delta(ktime_get(), ts);
					break;
				}
			}
			mtk_crtc->cmd_fb_addr = addr;
		}
	} else if (cmd_memory_mode && fb && mtk_crtc->cmd_conv_buf[0]) {
		/*
		 * Alternate the buffers: the previous frame is still being
		 * clocked out of the other one by the DSI.
		 */
		buf_idx = mtk_crtc->cmd_conv_idx;
		mtk_crtc->cmd_conv_idx ^= 1;
		mtk_crtc_cmd_mode_convert(mtk_crtc, fb,
					  mtk_crtc->cmd_conv_buf[buf_idx]);
		dma_sync_single_for_device(mtk_crtc->base.dev->dev,
					   mtk_crtc->cmd_conv_dma[buf_idx],
					   1080 * 2400 * 3, DMA_TO_DEVICE);
		for (i = 0; i < mtk_crtc->ddp_comp_nr; i++) {
			struct mtk_ddp_comp *comp = mtk_crtc->ddp_comp[i];

			if (comp->funcs && comp->funcs->mem_config) {
				comp->funcs->mem_config(comp->dev,
							mtk_crtc->cmd_conv_dma[buf_idx],
							fb->width * 3,
							DRM_FORMAT_RGB888);
				break;
			}
		}
	}

	if (fb)
		drm_framebuffer_put(fb);

	{
		ktime_t ts = ktime_get();

		/* Re-arm RDMA before each frame in the direct path. */
		if (!cmd_memory_mode && mtk_crtc->ddp_comp_nr > 1)
			mtk_ddp_comp_start(mtk_crtc->ddp_comp[1]);

		/* Single-shot mutex: one software trigger per frame. */
		mtk_mutex_enable(mtk_crtc->mutex);

		/*
		 * Give RDMA time to fill its FIFO before the DSI is told to
		 * start pulling a frame: if 0x2C is sent while the FIFO is
		 * still empty, the DSI FSM wedges in BUSY and cannot be
		 * recovered in software.
		 */
		usleep_range(1000, 2000);

		/* Then ask the DSI for one frame. */
		mtk_crtc->trig_count++;
		mtk_ddp_comp_trigger(mtk_crtc->ddp_comp[mtk_crtc->ddp_comp_nr - 1]);
		mtk_crtc->w_tail_ms += ktime_ms_delta(ktime_get(), ts);
	}
	{
		unsigned int d = ktime_ms_delta(ktime_get(), tick_start);
		if (d > mtk_crtc->w_maxms)
			mtk_crtc->w_maxms = d;
		mtk_crtc->w_sum_ms += d;
	}

resched:
	if (mtk_crtc->te_mode && !(mtk_crtc->work_count % 120))
		dev_info(mtk_crtc->base.dev->dev,
			 "te=%llu work=%llu trig=%llu busy=%llu nofb=%llu upd=%llu maxms=%llu avgms=%llu syncms=%llu cfgms=%llu tailms=%llu\n",
			 mtk_crtc->te_count, mtk_crtc->work_count,
			 mtk_crtc->trig_count, mtk_crtc->w_busy,
			 mtk_crtc->w_nofb, mtk_crtc->w_upd, mtk_crtc->w_maxms,
			 mtk_crtc->w_sum_ms / mtk_crtc->work_count,
			 mtk_crtc->w_sync_ms / mtk_crtc->work_count,
			 mtk_crtc->w_cfg_ms / mtk_crtc->work_count,
			 mtk_crtc->w_tail_ms / mtk_crtc->work_count);

	if (mtk_crtc->enabled) {
		unsigned int period_ms = mtk_crtc_cmd_period_ms(mtk_crtc);
		unsigned int target_ms = period_ms;
		unsigned int elapsed_ms;
		unsigned int delay_ms;

		/*
		 * TE and the timer must not fight over the same frame: if a
		 * TE edge arrived since the previous frame the display is
		 * paced by the panel, and the timer only has to act as a
		 * watchdog (two periods out).  Without a TE edge the timer
		 * drives the frames itself.
		 */
		if (mtk_crtc->te_mode &&
		    mtk_crtc->te_count != mtk_crtc->te_seen) {
			mtk_crtc->te_seen = mtk_crtc->te_count;
			target_ms = period_ms * 2;
		}

		/*
		 * Keep a fixed grid: the next frame starts target_ms after
		 * this one did, not after the work finished.  Otherwise the
		 * conversion time is added to every period and a 60 Hz panel
		 * ends up at ~40 fps.
		 */
		elapsed_ms = ktime_ms_delta(ktime_get(), tick_start);
		delay_ms = target_ms > elapsed_ms ? target_ms - elapsed_ms : 1;

		schedule_delayed_work(&mtk_crtc->cmd_mode_work,
				      msecs_to_jiffies(delay_ms));
	}
}

static void mtk_crtc_destroy(struct drm_crtc *crtc)
{
	struct mtk_crtc *mtk_crtc = to_mtk_crtc(crtc);
	int i;

	cancel_delayed_work_sync(&mtk_crtc->cmd_mode_work);

	/*
	 * The panel keeps pulsing TE after the CRTC is torn down.  Release the
	 * IRQ first: drm_crtc_cleanup() clears crtc->dev, and an interrupt
	 * arriving in between would dereference it.
	 */
	if (mtk_crtc->te_irq > 0) {
		mtk_crtc->te_mode = false;
		disable_irq(mtk_crtc->te_irq);
		devm_free_irq(mtk_crtc->te_dev, mtk_crtc->te_irq, mtk_crtc);
		mtk_crtc->te_irq = -1;
	}
	if (mtk_crtc->te_gpio) {
		gpiod_put(mtk_crtc->te_gpio);
		mtk_crtc->te_gpio = NULL;
	}

	if (mtk_crtc->cmd_fb_cur) {
		drm_framebuffer_put(mtk_crtc->cmd_fb_cur);
		mtk_crtc->cmd_fb_cur = NULL;
	}
	for (i = 0; i < ARRAY_SIZE(mtk_crtc->cmd_conv_buf); i++) {
		if (mtk_crtc->cmd_conv_buf[i])
			dma_free_noncoherent(crtc->dev->dev, 1080 * 2400 * 3,
					     mtk_crtc->cmd_conv_buf[i],
					     mtk_crtc->cmd_conv_dma[i],
					     DMA_TO_DEVICE);
	}
	mtk_mutex_put(mtk_crtc->mutex);
#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	if (mtk_crtc->cmdq_client.chan) {
		cmdq_pkt_destroy(&mtk_crtc->cmdq_client, &mtk_crtc->cmdq_handle);
		mbox_free_channel(mtk_crtc->cmdq_client.chan);
		mtk_crtc->cmdq_client.chan = NULL;
	}
#endif

	for (i = 0; i < mtk_crtc->ddp_comp_nr; i++) {
		struct mtk_ddp_comp *comp;

		comp = mtk_crtc->ddp_comp[i];
		mtk_ddp_comp_unregister_vblank_cb(comp);
	}

	drm_crtc_cleanup(crtc);
}

static void mtk_crtc_reset(struct drm_crtc *crtc)
{
	struct mtk_crtc_state *state;

	if (crtc->state)
		__drm_atomic_helper_crtc_destroy_state(crtc->state);

	kfree(to_mtk_crtc_state(crtc->state));
	crtc->state = NULL;

	state = kzalloc_obj(*state);
	if (state)
		__drm_atomic_helper_crtc_reset(crtc, &state->base);
}

static struct drm_crtc_state *mtk_crtc_duplicate_state(struct drm_crtc *crtc)
{
	struct mtk_crtc_state *state;

	state = kmalloc_obj(*state);
	if (!state)
		return NULL;

	__drm_atomic_helper_crtc_duplicate_state(crtc, &state->base);

	WARN_ON(state->base.crtc != crtc);
	state->base.crtc = crtc;
	state->pending_config = false;

	return &state->base;
}

static void mtk_crtc_destroy_state(struct drm_crtc *crtc,
				   struct drm_crtc_state *state)
{
	__drm_atomic_helper_crtc_destroy_state(state);
	kfree(to_mtk_crtc_state(state));
}

static enum drm_mode_status
mtk_crtc_mode_valid(struct drm_crtc *crtc, const struct drm_display_mode *mode)
{
	struct mtk_crtc *mtk_crtc = to_mtk_crtc(crtc);
	enum drm_mode_status status = MODE_OK;
	int i;

	for (i = 0; i < mtk_crtc->ddp_comp_nr; i++) {
		status = mtk_ddp_comp_mode_valid(mtk_crtc->ddp_comp[i], mode);
		if (status != MODE_OK)
			break;
	}
	return status;
}

static bool mtk_crtc_mode_fixup(struct drm_crtc *crtc,
				const struct drm_display_mode *mode,
				struct drm_display_mode *adjusted_mode)
{
	/* Nothing to do here, but this callback is mandatory. */
	return true;
}

static void mtk_crtc_mode_set_nofb(struct drm_crtc *crtc)
{
	struct mtk_crtc_state *state = to_mtk_crtc_state(crtc->state);

	state->pending_width = crtc->mode.hdisplay;
	state->pending_height = crtc->mode.vdisplay;
	state->pending_vrefresh = drm_mode_vrefresh(&crtc->mode);
	wmb();	/* Make sure the above parameters are set before update */
	state->pending_config = true;
}

static int mtk_crtc_ddp_clk_enable(struct mtk_crtc *mtk_crtc)
{
	struct drm_device *dev = mtk_crtc->base.dev;
	int ret;
	int i;

	for (i = 0; i < mtk_crtc->ddp_comp_nr; i++) {
		ret = mtk_ddp_comp_clk_enable(mtk_crtc->ddp_comp[i]);
		if (ret) {
			drm_err(dev, "Failed to enable clock %d: %d\n", i, ret);
			goto err;
		}
	}

	return 0;
err:
	while (--i >= 0)
		mtk_ddp_comp_clk_disable(mtk_crtc->ddp_comp[i]);
	return ret;
}

static void mtk_crtc_ddp_clk_disable(struct mtk_crtc *mtk_crtc)
{
	int i;

	for (i = 0; i < mtk_crtc->ddp_comp_nr; i++)
		mtk_ddp_comp_clk_disable(mtk_crtc->ddp_comp[i]);
}

static
struct mtk_ddp_comp *mtk_ddp_comp_for_plane(struct drm_crtc *crtc,
					    struct drm_plane *plane,
					    unsigned int *local_layer)
{
	struct mtk_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct mtk_ddp_comp *comp;
	int i, count = 0;
	unsigned int local_index = plane - mtk_crtc->planes;

	for (i = 0; i < mtk_crtc->ddp_comp_nr; i++) {
		comp = mtk_crtc->ddp_comp[i];
		if (local_index < (count + mtk_ddp_comp_layer_nr(comp))) {
			*local_layer = local_index - count;
			return comp;
		}
		count += mtk_ddp_comp_layer_nr(comp);
	}

	WARN(1, "Failed to find component for plane %d\n", plane->index);
	return NULL;
}

#if IS_REACHABLE(CONFIG_MTK_CMDQ)
static void ddp_cmdq_cb(struct mbox_client *cl, void *mssg)
{
	struct cmdq_cb_data *data = mssg;
	struct cmdq_client *cmdq_cl = container_of(cl, struct cmdq_client, client);
	struct mtk_crtc *mtk_crtc = container_of(cmdq_cl, struct mtk_crtc, cmdq_client);
	struct mtk_crtc_state *state;
	unsigned int i;
	unsigned long flags;

	/* release GCE HW usage and start autosuspend */
	pm_runtime_mark_last_busy(cmdq_cl->chan->mbox->dev);
	pm_runtime_put_autosuspend(cmdq_cl->chan->mbox->dev);

	if (data->sta < 0)
		return;

	state = to_mtk_crtc_state(mtk_crtc->base.state);

	spin_lock_irqsave(&mtk_crtc->config_lock, flags);
	if (mtk_crtc->config_updating)
		goto ddp_cmdq_cb_out;

	state->pending_config = false;

	if (mtk_crtc->pending_planes) {
		for (i = 0; i < mtk_crtc->layer_nr; i++) {
			struct drm_plane *plane = &mtk_crtc->planes[i];
			struct mtk_plane_state *plane_state;

			plane_state = to_mtk_plane_state(plane->state);

			plane_state->pending.config = false;
		}
		mtk_crtc->pending_planes = false;
	}

	if (mtk_crtc->pending_async_planes) {
		for (i = 0; i < mtk_crtc->layer_nr; i++) {
			struct drm_plane *plane = &mtk_crtc->planes[i];
			struct mtk_plane_state *plane_state;

			plane_state = to_mtk_plane_state(plane->state);

			plane_state->pending.async_config = false;
		}
		mtk_crtc->pending_async_planes = false;
	}

ddp_cmdq_cb_out:

	if (mtk_crtc->pending_needs_vblank) {
		mtk_crtc_finish_page_flip(mtk_crtc);
		mtk_crtc->pending_needs_vblank = false;
	}

	spin_unlock_irqrestore(&mtk_crtc->config_lock, flags);

	mtk_crtc->cmdq_vblank_cnt = 0;
	wake_up(&mtk_crtc->cb_blocking_queue);
}
#endif

static int mtk_crtc_ddp_hw_init(struct mtk_crtc *mtk_crtc)
{
	struct drm_crtc *crtc = &mtk_crtc->base;
	struct drm_connector *connector;
	struct drm_encoder *encoder;
	struct drm_connector_list_iter conn_iter;
	struct drm_device *dev = mtk_crtc->base.dev;
	unsigned int width, height, vrefresh, bpc = MTK_MAX_BPC;
	int ret;
	int i;

	if (WARN_ON(!crtc->state))
		return -EINVAL;

	width = crtc->state->adjusted_mode.hdisplay;
	height = crtc->state->adjusted_mode.vdisplay;
	vrefresh = drm_mode_vrefresh(&crtc->state->adjusted_mode);

	drm_for_each_encoder(encoder, crtc->dev) {
		if (encoder->crtc != crtc)
			continue;

		drm_connector_list_iter_begin(crtc->dev, &conn_iter);
		drm_for_each_connector_iter(connector, &conn_iter) {
			if (connector->encoder != encoder)
				continue;
			if (connector->display_info.bpc != 0 &&
			    bpc > connector->display_info.bpc)
				bpc = connector->display_info.bpc;
		}
		drm_connector_list_iter_end(&conn_iter);
	}

	ret = pm_runtime_resume_and_get(crtc->dev->dev);
	if (ret < 0) {
		drm_err(dev, "Failed to enable power domain: %d\n", ret);
		return ret;
	}

	ret = mtk_mutex_prepare(mtk_crtc->mutex);
	if (ret < 0) {
		drm_err(dev, "Failed to enable mutex clock: %d\n", ret);
		goto err_pm_runtime_put;
	}

	ret = mtk_crtc_ddp_clk_enable(mtk_crtc);
	if (ret < 0) {
		drm_err(dev, "Failed to enable component clocks: %d\n", ret);
		goto err_mutex_unprepare;
	}

	for (i = 0; i < mtk_crtc->ddp_comp_nr - 1; i++) {
		if (!mtk_ddp_comp_connect(mtk_crtc->ddp_comp[i], mtk_crtc->mmsys_dev,
					  mtk_crtc->ddp_comp[i + 1]->id))
			mtk_mmsys_ddp_connect(mtk_crtc->mmsys_dev,
					      mtk_crtc->ddp_comp[i]->id,
					      mtk_crtc->ddp_comp[i + 1]->id);
		if (!mtk_ddp_comp_add(mtk_crtc->ddp_comp[i], mtk_crtc->mutex))
			mtk_mutex_add_comp(mtk_crtc->mutex,
					   mtk_crtc->ddp_comp[i]->id);
	}
	if (!mtk_ddp_comp_add(mtk_crtc->ddp_comp[i], mtk_crtc->mutex))
		mtk_mutex_add_comp(mtk_crtc->mutex, mtk_crtc->ddp_comp[i]->id);
	mtk_mutex_enable(mtk_crtc->mutex);

	for (i = 0; i < mtk_crtc->ddp_comp_nr; i++) {
		struct mtk_ddp_comp *comp = mtk_crtc->ddp_comp[i];

		if (i == 1)
			mtk_ddp_comp_bgclr_in_on(comp);

		mtk_ddp_comp_config(comp, width, height, vrefresh, bpc, NULL);
		mtk_ddp_comp_start(comp);
	}

	/* Initially configure all planes */
	for (i = 0; i < mtk_crtc->layer_nr; i++) {
		struct drm_plane *plane = &mtk_crtc->planes[i];
		struct mtk_plane_state *plane_state;
		struct mtk_ddp_comp *comp;
		unsigned int local_layer;

		plane_state = to_mtk_plane_state(plane->state);

		/*
		 * Memory mode programs the RDMA buffer separately, so the OVL
		 * layers stay off until the command-mode tick feeds them.
		 *
		 * The direct path is different: the DSI is enabled right after
		 * this function, and switching it into command mode waits for
		 * the first frame.  With no OVL layer enabled the pipeline never
		 * produces one, the switch times out and the DSI FSM stays in
		 * BUSY.  So keep the layers enabled here, the way the vendor
		 * pipeline has them when the DSI comes up.
		 */
		if (cmd_memory_mode)
			plane_state->pending.enable = false;
		comp = mtk_ddp_comp_for_plane(crtc, plane, &local_layer);
		if (comp)
			mtk_ddp_comp_layer_config(comp, local_layer,
						  plane_state, NULL);
	}

	return 0;

err_mutex_unprepare:
	mtk_mutex_unprepare(mtk_crtc->mutex);
err_pm_runtime_put:
	pm_runtime_put(crtc->dev->dev);
	return ret;
}

static void mtk_crtc_ddp_hw_fini(struct mtk_crtc *mtk_crtc)
{
	struct drm_device *drm = mtk_crtc->base.dev;
	struct drm_crtc *crtc = &mtk_crtc->base;
	unsigned long flags;
	int i;

	for (i = 0; i < mtk_crtc->ddp_comp_nr; i++) {
		mtk_ddp_comp_stop(mtk_crtc->ddp_comp[i]);
		if (i == 1)
			mtk_ddp_comp_bgclr_in_off(mtk_crtc->ddp_comp[i]);
	}

	for (i = 0; i < mtk_crtc->ddp_comp_nr; i++)
		if (!mtk_ddp_comp_remove(mtk_crtc->ddp_comp[i], mtk_crtc->mutex))
			mtk_mutex_remove_comp(mtk_crtc->mutex,
					      mtk_crtc->ddp_comp[i]->id);
	mtk_mutex_disable(mtk_crtc->mutex);
	for (i = 0; i < mtk_crtc->ddp_comp_nr - 1; i++) {
		if (!mtk_ddp_comp_disconnect(mtk_crtc->ddp_comp[i], mtk_crtc->mmsys_dev,
					     mtk_crtc->ddp_comp[i + 1]->id))
			mtk_mmsys_ddp_disconnect(mtk_crtc->mmsys_dev,
						 mtk_crtc->ddp_comp[i]->id,
						 mtk_crtc->ddp_comp[i + 1]->id);
		if (!mtk_ddp_comp_remove(mtk_crtc->ddp_comp[i], mtk_crtc->mutex))
			mtk_mutex_remove_comp(mtk_crtc->mutex,
					      mtk_crtc->ddp_comp[i]->id);
	}
	if (!mtk_ddp_comp_remove(mtk_crtc->ddp_comp[i], mtk_crtc->mutex))
		mtk_mutex_remove_comp(mtk_crtc->mutex, mtk_crtc->ddp_comp[i]->id);
	mtk_crtc_ddp_clk_disable(mtk_crtc);
	mtk_mutex_unprepare(mtk_crtc->mutex);

	pm_runtime_put(drm->dev);

	if (crtc->state->event && !crtc->state->active) {
		spin_lock_irqsave(&crtc->dev->event_lock, flags);
		drm_crtc_send_vblank_event(crtc, crtc->state->event);
		crtc->state->event = NULL;
		spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
	}
}

static void mtk_crtc_ddp_config(struct drm_crtc *crtc,
				struct cmdq_pkt *cmdq_handle)
{
	struct mtk_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct mtk_crtc_state *state = to_mtk_crtc_state(mtk_crtc->base.state);
	struct mtk_ddp_comp *comp = mtk_crtc->ddp_comp[0];
	unsigned int i;
	unsigned int local_layer;

	/*
	 * TODO: instead of updating the registers here, we should prepare
	 * working registers in atomic_commit and let the hardware command
	 * queue update module registers on vblank.
	 */
	if (state->pending_config) {
		mtk_ddp_comp_config(comp, state->pending_width,
				    state->pending_height,
				    state->pending_vrefresh, 0,
				    cmdq_handle);

		if (!cmdq_handle)
			state->pending_config = false;
	}

	if (mtk_crtc->pending_planes) {
		for (i = 0; i < mtk_crtc->layer_nr; i++) {
			struct drm_plane *plane = &mtk_crtc->planes[i];
			struct mtk_plane_state *plane_state;

			plane_state = to_mtk_plane_state(plane->state);

			if (!plane_state->pending.config)
				continue;

			comp = mtk_ddp_comp_for_plane(crtc, plane, &local_layer);

			if (comp)
				mtk_ddp_comp_layer_config(comp, local_layer,
							  plane_state,
							  cmdq_handle);
			if (!cmdq_handle)
				plane_state->pending.config = false;
		}

		if (!cmdq_handle)
			mtk_crtc->pending_planes = false;
	}

	if (mtk_crtc->pending_async_planes) {
		for (i = 0; i < mtk_crtc->layer_nr; i++) {
			struct drm_plane *plane = &mtk_crtc->planes[i];
			struct mtk_plane_state *plane_state;

			plane_state = to_mtk_plane_state(plane->state);

			if (!plane_state->pending.async_config)
				continue;

			comp = mtk_ddp_comp_for_plane(crtc, plane, &local_layer);

			if (comp)
				mtk_ddp_comp_layer_config(comp, local_layer,
							  plane_state,
							  cmdq_handle);
			if (!cmdq_handle)
				plane_state->pending.async_config = false;
		}

		if (!cmdq_handle)
			mtk_crtc->pending_async_planes = false;
	}
}

static void mtk_crtc_update_config(struct mtk_crtc *mtk_crtc, bool needs_vblank)
{
#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	struct cmdq_pkt *cmdq_handle = &mtk_crtc->cmdq_handle;
#endif
	struct drm_crtc *crtc = &mtk_crtc->base;
	struct mtk_drm_private *priv = crtc->dev->dev_private;
	unsigned int pending_planes = 0, pending_async_planes = 0;
	int i;
	unsigned long flags;

	mutex_lock(&mtk_crtc->hw_lock);

	spin_lock_irqsave(&mtk_crtc->config_lock, flags);
	mtk_crtc->config_updating = true;
	spin_unlock_irqrestore(&mtk_crtc->config_lock, flags);

	if (needs_vblank)
		mtk_crtc->pending_needs_vblank = true;

	for (i = 0; i < mtk_crtc->layer_nr; i++) {
		struct drm_plane *plane = &mtk_crtc->planes[i];
		struct mtk_plane_state *plane_state;

		plane_state = to_mtk_plane_state(plane->state);
		if (plane_state->pending.dirty) {
			plane_state->pending.config = true;
			plane_state->pending.dirty = false;
			pending_planes |= BIT(i);
		} else if (plane_state->pending.async_dirty) {
			plane_state->pending.async_config = true;
			plane_state->pending.async_dirty = false;
			pending_async_planes |= BIT(i);
		}
	}
	if (pending_planes)
		mtk_crtc->pending_planes = true;
	if (pending_async_planes)
		mtk_crtc->pending_async_planes = true;

	if (priv->data->shadow_register) {
		mtk_mutex_acquire(mtk_crtc->mutex);
		mtk_crtc_ddp_config(crtc, NULL);
		mtk_mutex_release(mtk_crtc->mutex);
	}
#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	if (mtk_crtc->cmdq_client.chan) {
		mbox_flush(mtk_crtc->cmdq_client.chan, 2000);
		cmdq_handle->cmd_buf_size = 0;
		cmdq_pkt_clear_event(cmdq_handle, mtk_crtc->cmdq_event);
		cmdq_pkt_wfe(cmdq_handle, mtk_crtc->cmdq_event, false);
		mtk_crtc_ddp_config(crtc, cmdq_handle);
		cmdq_pkt_eoc(cmdq_handle);
		dma_sync_single_for_device(mtk_crtc->cmdq_client.chan->mbox->dev,
					   cmdq_handle->pa_base,
					   cmdq_handle->cmd_buf_size,
					   DMA_TO_DEVICE);
		/*
		 * CMDQ command should execute in next 3 vblank.
		 * One vblank interrupt before send message (occasionally)
		 * and one vblank interrupt after cmdq done,
		 * so it's timeout after 3 vblank interrupt.
		 * If it fail to execute in next 3 vblank, timeout happen.
		 */
		mtk_crtc->cmdq_vblank_cnt = 3;

		spin_lock_irqsave(&mtk_crtc->config_lock, flags);
		mtk_crtc->config_updating = false;
		spin_unlock_irqrestore(&mtk_crtc->config_lock, flags);

		if (pm_runtime_resume_and_get(mtk_crtc->cmdq_client.chan->mbox->dev) < 0)
			goto update_config_out;

		mbox_send_message(mtk_crtc->cmdq_client.chan, cmdq_handle);
		mbox_client_txdone(mtk_crtc->cmdq_client.chan, 0);
		goto update_config_out;
	}
#endif
	spin_lock_irqsave(&mtk_crtc->config_lock, flags);
	mtk_crtc->config_updating = false;
	spin_unlock_irqrestore(&mtk_crtc->config_lock, flags);

#if IS_REACHABLE(CONFIG_MTK_CMDQ)
update_config_out:
#endif
	mutex_unlock(&mtk_crtc->hw_lock);
}

static void mtk_crtc_ddp_irq(void *data)
{
	struct drm_crtc *crtc = data;
	struct mtk_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct mtk_drm_private *priv = crtc->dev->dev_private;

#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	struct drm_device *dev = mtk_crtc->base.dev;
	if (!priv->data->shadow_register && !mtk_crtc->cmdq_client.chan)
		mtk_crtc_ddp_config(crtc, NULL);
	else if (mtk_crtc->cmdq_vblank_cnt > 0 && --mtk_crtc->cmdq_vblank_cnt == 0)
		drm_err(dev, "mtk_crtc %d CMDQ execute command timeout!\n",
			drm_crtc_index(&mtk_crtc->base));
#else
	if (!priv->data->shadow_register)
		mtk_crtc_ddp_config(crtc, NULL);
#endif
	mtk_drm_finish_page_flip(mtk_crtc);
}

static int mtk_crtc_enable_vblank(struct drm_crtc *crtc)
{
	struct mtk_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct mtk_ddp_comp *comp = mtk_crtc->ddp_comp[0];

	mtk_ddp_comp_enable_vblank(comp);

	return 0;
}

static void mtk_crtc_disable_vblank(struct drm_crtc *crtc)
{
	struct mtk_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct mtk_ddp_comp *comp = mtk_crtc->ddp_comp[0];

	mtk_ddp_comp_disable_vblank(comp);
}

static void mtk_crtc_update_output(struct drm_crtc *crtc,
				   struct drm_atomic_state *state)
{
	int crtc_index = drm_crtc_index(crtc);
	int i;
	struct device *dev;
	struct drm_crtc_state *crtc_state = state->crtcs[crtc_index].new_state;
	struct mtk_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct mtk_drm_private *priv;
	unsigned int encoder_mask = crtc_state->encoder_mask;

	if (!crtc_state->connectors_changed)
		return;

	if (!mtk_crtc->num_conn_routes)
		return;

	priv = ((struct mtk_drm_private *)crtc->dev->dev_private)->all_drm_private[crtc_index];
	dev = priv->dev;

	dev_dbg(dev, "connector change:%d, encoder mask:0x%x for crtc:%d\n",
		crtc_state->connectors_changed, encoder_mask, crtc_index);

	for (i = 0; i < mtk_crtc->num_conn_routes; i++) {
		unsigned int comp_id = mtk_crtc->conn_routes[i].route_ddp;
		struct mtk_ddp_comp *comp = &priv->ddp_comp[comp_id];

		if (comp->encoder_index >= 0 &&
		    (encoder_mask & BIT(comp->encoder_index))) {
			mtk_crtc->ddp_comp[mtk_crtc->ddp_comp_nr - 1] = comp;
			dev_dbg(dev, "Add comp_id: %d at path index %d\n",
				comp->id, mtk_crtc->ddp_comp_nr - 1);
			break;
		}
	}
}

int mtk_crtc_plane_check(struct drm_crtc *crtc, struct drm_plane *plane,
			 struct mtk_plane_state *state)
{
	unsigned int local_layer;
	struct mtk_ddp_comp *comp;

	comp = mtk_ddp_comp_for_plane(crtc, plane, &local_layer);
	if (comp)
		return mtk_ddp_comp_layer_check(comp, local_layer, state);
	return 0;
}

void mtk_crtc_plane_disable(struct drm_crtc *crtc, struct drm_plane *plane)
{
#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	struct mtk_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct mtk_plane_state *plane_state = to_mtk_plane_state(plane->state);
	int i;

	/* no need to wait for disabling the plane by CPU */
	if (!mtk_crtc->cmdq_client.chan)
		return;

	if (!mtk_crtc->enabled)
		return;

	/* set pending plane state to disabled */
	for (i = 0; i < mtk_crtc->layer_nr; i++) {
		struct drm_plane *mtk_plane = &mtk_crtc->planes[i];
		struct mtk_plane_state *mtk_plane_state = to_mtk_plane_state(mtk_plane->state);

		if (mtk_plane->index == plane->index) {
			memcpy(mtk_plane_state, plane_state, sizeof(*plane_state));
			break;
		}
	}
	mtk_crtc_update_config(mtk_crtc, false);

	/* wait for planes to be disabled by CMDQ */
	wait_event_timeout(mtk_crtc->cb_blocking_queue,
			   mtk_crtc->cmdq_vblank_cnt == 0,
			   msecs_to_jiffies(500));
#endif
}

void mtk_crtc_async_update(struct drm_crtc *crtc, struct drm_plane *plane,
			   struct drm_atomic_state *state)
{
	struct mtk_crtc *mtk_crtc = to_mtk_crtc(crtc);

	if (!mtk_crtc->enabled)
		return;

	mtk_crtc_update_config(mtk_crtc, false);
}

static void mtk_crtc_atomic_enable(struct drm_crtc *crtc,
				   struct drm_atomic_state *state)
{
	struct mtk_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct mtk_ddp_comp *comp = mtk_crtc->ddp_comp[0];
	struct drm_device *dev = mtk_crtc->base.dev;
	int ret;

	drm_dbg_driver(dev, "%s %d\n", __func__, crtc->base.id);

	ret = mtk_ddp_comp_power_on(comp);
	if (ret < 0) {
		DRM_DEV_ERROR(comp->dev, "Failed to enable power domain: %d\n", ret);
		return;
	}

	mtk_crtc_update_output(crtc, state);

	ret = mtk_crtc_ddp_hw_init(mtk_crtc);
	if (ret) {
		mtk_ddp_comp_power_off(comp);
		return;
	}

	drm_crtc_vblank_on(crtc);
	mtk_crtc->enabled = true;
	if (cmd_tick)
		/* Let the panel init and DPHY settle before the first
		 * frame request, otherwise the DSI FSM wedges in BUSY.
		 */
		schedule_delayed_work(&mtk_crtc->cmd_mode_work,
				      msecs_to_jiffies(100));
}

static void mtk_crtc_atomic_disable(struct drm_crtc *crtc,
				    struct drm_atomic_state *state)
{
	struct mtk_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct mtk_ddp_comp *comp = mtk_crtc->ddp_comp[0];
	struct drm_device *dev = mtk_crtc->base.dev;
	int i;

	drm_dbg_driver(dev, "%s %d\n", __func__, crtc->base.id);
	if (!mtk_crtc->enabled)
		return;

	/*
	 * Do NOT cancel_delayed_work_sync() here: the tick takes the plane
	 * lock, and the atomic commit that called us holds it until
	 * commit_tail returns - waiting for the work would deadlock
	 * (reproduced by Xorg: fb_blank -> dpms off -> crtc disable, Xorg
	 * stuck forever in D state). Clear ->enabled first, stop re-arming,
	 * and let a work instance already waiting on the lock bail out.
	 */
	mtk_crtc->enabled = false;
	cancel_delayed_work(&mtk_crtc->cmd_mode_work);

	/* Set all pending plane state to disabled */
	for (i = 0; i < mtk_crtc->layer_nr; i++) {
		struct drm_plane *plane = &mtk_crtc->planes[i];
		struct mtk_plane_state *plane_state;

		plane_state = to_mtk_plane_state(plane->state);
		plane_state->pending.enable = false;
		plane_state->pending.config = true;
	}
	mtk_crtc->pending_planes = true;

	mtk_crtc_update_config(mtk_crtc, false);
#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	/* Wait for planes to be disabled by cmdq */
	if (mtk_crtc->cmdq_client.chan)
		wait_event_timeout(mtk_crtc->cb_blocking_queue,
				   mtk_crtc->cmdq_vblank_cnt == 0,
				   msecs_to_jiffies(500));
#endif
	/* Wait for planes to be disabled */
	drm_crtc_wait_one_vblank(crtc);

	drm_crtc_vblank_off(crtc);
	mtk_crtc_ddp_hw_fini(mtk_crtc);
	mtk_ddp_comp_power_off(comp);

	mtk_crtc->enabled = false;
}

static void mtk_crtc_atomic_begin(struct drm_crtc *crtc,
				  struct drm_atomic_state *state)
{
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state,
									  crtc);
	struct mtk_crtc_state *mtk_crtc_state = to_mtk_crtc_state(crtc_state);
	struct mtk_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct drm_device *dev = mtk_crtc->base.dev;
	unsigned long flags;

	mtk_crtc_cmd_cache_fb(mtk_crtc);

	if (mtk_crtc->event && mtk_crtc_state->base.event)
		drm_err(dev, "new event while there is still a pending event\n");

	if (mtk_crtc_state->base.event) {
		mtk_crtc_state->base.event->pipe = drm_crtc_index(crtc);
		WARN_ON(drm_crtc_vblank_get(crtc) != 0);

		spin_lock_irqsave(&crtc->dev->event_lock, flags);
		mtk_crtc->event = mtk_crtc_state->base.event;
		spin_unlock_irqrestore(&crtc->dev->event_lock, flags);

		mtk_crtc_state->base.event = NULL;
	}
}

static void mtk_crtc_atomic_flush(struct drm_crtc *crtc,
				  struct drm_atomic_state *state)
{
	struct mtk_crtc *mtk_crtc = to_mtk_crtc(crtc);
	int i;

	if (crtc->state->color_mgmt_changed)
		for (i = 0; i < mtk_crtc->ddp_comp_nr; i++) {
			mtk_ddp_gamma_set(mtk_crtc->ddp_comp[i], crtc->state);
			mtk_ddp_ctm_set(mtk_crtc->ddp_comp[i], crtc->state);
		}
	mtk_crtc_update_config(mtk_crtc, !!mtk_crtc->event);
}

static const struct drm_crtc_funcs mtk_crtc_funcs = {
	.set_config		= drm_atomic_helper_set_config,
	.page_flip		= drm_atomic_helper_page_flip,
	.destroy		= mtk_crtc_destroy,
	.reset			= mtk_crtc_reset,
	.atomic_duplicate_state	= mtk_crtc_duplicate_state,
	.atomic_destroy_state	= mtk_crtc_destroy_state,
	.enable_vblank		= mtk_crtc_enable_vblank,
	.disable_vblank		= mtk_crtc_disable_vblank,
};

static const struct drm_crtc_helper_funcs mtk_crtc_helper_funcs = {
	.mode_fixup	= mtk_crtc_mode_fixup,
	.mode_set_nofb	= mtk_crtc_mode_set_nofb,
	.mode_valid	= mtk_crtc_mode_valid,
	.atomic_begin	= mtk_crtc_atomic_begin,
	.atomic_flush	= mtk_crtc_atomic_flush,
	.atomic_enable	= mtk_crtc_atomic_enable,
	.atomic_disable	= mtk_crtc_atomic_disable,
};

static int mtk_crtc_init(struct drm_device *drm, struct mtk_crtc *mtk_crtc,
			 unsigned int pipe)
{
	struct drm_plane *primary = NULL;
	struct drm_plane *cursor = NULL;
	int i, ret;

	for (i = 0; i < mtk_crtc->layer_nr; i++) {
		if (mtk_crtc->planes[i].type == DRM_PLANE_TYPE_PRIMARY)
			primary = &mtk_crtc->planes[i];
		else if (mtk_crtc->planes[i].type == DRM_PLANE_TYPE_CURSOR)
			cursor = &mtk_crtc->planes[i];
	}

	ret = drm_crtc_init_with_planes(drm, &mtk_crtc->base, primary, cursor,
					&mtk_crtc_funcs, NULL);
	if (ret)
		goto err_cleanup_crtc;

	drm_crtc_helper_add(&mtk_crtc->base, &mtk_crtc_helper_funcs);

	return 0;

err_cleanup_crtc:
	drm_crtc_cleanup(&mtk_crtc->base);
	return ret;
}

static int mtk_crtc_num_comp_planes(struct mtk_crtc *mtk_crtc, int comp_idx)
{
	struct mtk_ddp_comp *comp;

	if (comp_idx > 1)
		return 0;

	comp = mtk_crtc->ddp_comp[comp_idx];
	if (!comp->funcs)
		return 0;

	if (comp_idx == 1 && !comp->funcs->bgclr_in_on)
		return 0;

	return mtk_ddp_comp_layer_nr(comp);
}

static inline
enum drm_plane_type mtk_crtc_plane_type(unsigned int plane_idx,
					unsigned int num_planes)
{
	if (plane_idx == 0)
		return DRM_PLANE_TYPE_PRIMARY;
	else if (plane_idx == (num_planes - 1))
		return DRM_PLANE_TYPE_CURSOR;
	else
		return DRM_PLANE_TYPE_OVERLAY;

}

static int mtk_crtc_init_comp_planes(struct drm_device *drm_dev,
				     struct mtk_crtc *mtk_crtc,
				     int comp_idx, int pipe)
{
	int num_planes = mtk_crtc_num_comp_planes(mtk_crtc, comp_idx);
	struct mtk_ddp_comp *comp = mtk_crtc->ddp_comp[comp_idx];
	int i, ret;

	for (i = 0; i < num_planes; i++) {
		ret = mtk_plane_init(drm_dev,
				&mtk_crtc->planes[mtk_crtc->layer_nr],
				BIT(pipe),
				mtk_crtc_plane_type(mtk_crtc->layer_nr, num_planes),
				mtk_ddp_comp_supported_rotations(comp),
				mtk_ddp_comp_get_blend_modes(comp),
				mtk_ddp_comp_get_formats(comp),
				mtk_ddp_comp_get_num_formats(comp),
				mtk_ddp_comp_is_afbc_supported(comp), i);
		if (ret)
			return ret;

		mtk_crtc->layer_nr++;
	}
	return 0;
}

struct device *mtk_crtc_dma_dev_get(struct drm_crtc *crtc)
{
	struct mtk_crtc *mtk_crtc = NULL;

	if (!crtc)
		return NULL;

	mtk_crtc = to_mtk_crtc(crtc);
	if (!mtk_crtc)
		return NULL;

	return mtk_crtc->dma_dev;
}

int mtk_crtc_create(struct drm_device *drm_dev, const unsigned int *path,
		    unsigned int path_len, int priv_data_index,
		    const struct mtk_drm_route *conn_routes,
		    unsigned int num_conn_routes)
{
	struct mtk_drm_private *priv = drm_dev->dev_private;
	struct device *dev = drm_dev->dev;
	struct mtk_crtc *mtk_crtc;
	unsigned int num_comp_planes = 0;
	int ret;
	int i;
	bool has_ctm = false;
	uint gamma_lut_size = 0;
	struct drm_crtc *tmp;
	int crtc_i = 0;

	if (!path)
		return 0;

	priv = priv->all_drm_private[priv_data_index];

	drm_for_each_crtc(tmp, drm_dev)
		crtc_i++;

	for (i = 0; i < path_len; i++) {
		enum mtk_ddp_comp_id comp_id = path[i];
		struct device_node *node;
		struct mtk_ddp_comp *comp;

		node = priv->comp_node[comp_id];
		comp = &priv->ddp_comp[comp_id];

		/* Not all drm components have a DTS device node, such as ovl_adaptor,
		 * which is the drm bring up sub driver
		 */
		if (!node && comp_id != DDP_COMPONENT_DRM_OVL_ADAPTOR) {
			dev_info(dev,
				"Not creating crtc %d because component %d is disabled or missing\n",
				crtc_i, comp_id);
			return 0;
		}

		if (!comp->dev) {
			dev_err(dev, "Component %pOF not initialized\n", node);
			return -ENODEV;
		}
	}

	mtk_crtc = devm_kzalloc(dev, sizeof(*mtk_crtc), GFP_KERNEL);
	if (!mtk_crtc)
		return -ENOMEM;

	mtk_crtc->mmsys_dev = priv->mmsys_dev;

	/*
	 * The panel drives a TE pulse at its own refresh rate (60 Hz on this
	 * panel).  Using it as the frame trigger keeps the DSI in step with
	 * the panel instead of pushing from a timer.
	 */
	mtk_crtc->ddp_comp_nr = path_len;

	/* Packed RGB888 frame for the command-mode RDMA memory path. */
	/*
	 * Use cacheable memory and sync it explicitly before each frame.
	 * dma_alloc_coherent() gives non-cacheable memory on this SoC, and
	 * writing the 7.8 MiB frame into it costs most of a CPU core at
	 * 30 fps; the cacheable buffer makes the conversion cheap and the
	 * one dma_sync per frame cleans the lines for the RDMA engine.
	 */
	for (i = 0; i < ARRAY_SIZE(mtk_crtc->cmd_conv_buf); i++) {
		mtk_crtc->cmd_conv_buf[i] =
			dma_alloc_noncoherent(dev, 1080 * 2400 * 3,
					      &mtk_crtc->cmd_conv_dma[i],
					      DMA_TO_DEVICE, GFP_KERNEL);
	}
	INIT_DELAYED_WORK(&mtk_crtc->cmd_mode_work, mtk_crtc_cmd_mode_work);
	spin_lock_init(&mtk_crtc->cmd_fb_lock);
	mtk_crtc->ddp_comp = devm_kcalloc(dev,
					  mtk_crtc->ddp_comp_nr + (conn_routes ? 1 : 0),
					  sizeof(*mtk_crtc->ddp_comp),
					  GFP_KERNEL);
	if (!mtk_crtc->ddp_comp)
		return -ENOMEM;

	mtk_crtc->mutex = mtk_mutex_get(priv->mutex_dev);
	if (IS_ERR(mtk_crtc->mutex)) {
		ret = PTR_ERR(mtk_crtc->mutex);
		dev_err(dev, "Failed to get mutex: %d\n", ret);
		return ret;
	}

	for (i = 0; i < mtk_crtc->ddp_comp_nr; i++) {
		unsigned int comp_id = path[i];
		struct mtk_ddp_comp *comp;

		comp = &priv->ddp_comp[comp_id];
		mtk_crtc->ddp_comp[i] = comp;

		if (comp->funcs) {
			if (comp->funcs->gamma_set && comp->funcs->gamma_get_lut_size) {
				unsigned int lut_sz = mtk_ddp_gamma_get_lut_size(comp);

				if (lut_sz)
					gamma_lut_size = lut_sz;
			}

			if (comp->funcs->ctm_set)
				has_ctm = true;
		}

		mtk_ddp_comp_register_vblank_cb(comp, mtk_crtc_ddp_irq,
						&mtk_crtc->base);
	}

	for (i = 0; i < mtk_crtc->ddp_comp_nr; i++)
		num_comp_planes += mtk_crtc_num_comp_planes(mtk_crtc, i);

	mtk_crtc->planes = devm_kcalloc(dev, num_comp_planes,
					sizeof(struct drm_plane), GFP_KERNEL);
	if (!mtk_crtc->planes)
		return -ENOMEM;

	for (i = 0; i < mtk_crtc->ddp_comp_nr; i++) {
		ret = mtk_crtc_init_comp_planes(drm_dev, mtk_crtc, i, crtc_i);
		if (ret)
			return ret;
	}

	/*
	 * Default to use the first component as the dma dev.
	 * In the case of ovl_adaptor sub driver, it needs to use the
	 * dma_dev_get function to get representative dma dev.
	 */
	mtk_crtc->dma_dev = mtk_ddp_comp_dma_dev_get(&priv->ddp_comp[path[0]]);

	ret = mtk_crtc_init(drm_dev, mtk_crtc, crtc_i);
	if (ret < 0)
		return ret;

	if (gamma_lut_size)
		drm_mode_crtc_set_gamma_size(&mtk_crtc->base, gamma_lut_size);
	drm_crtc_enable_color_mgmt(&mtk_crtc->base, 0, has_ctm, gamma_lut_size);
	mutex_init(&mtk_crtc->hw_lock);
	spin_lock_init(&mtk_crtc->config_lock);

#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	i = priv->mbox_index++;
	mtk_crtc->cmdq_client.client.dev = mtk_crtc->mmsys_dev;
	mtk_crtc->cmdq_client.client.tx_block = false;
	mtk_crtc->cmdq_client.client.knows_txdone = true;
	mtk_crtc->cmdq_client.client.rx_callback = ddp_cmdq_cb;
	mtk_crtc->cmdq_client.chan =
			mbox_request_channel(&mtk_crtc->cmdq_client.client, i);
	if (IS_ERR(mtk_crtc->cmdq_client.chan)) {
		dev_dbg(dev, "mtk_crtc %d failed to create mailbox client, writing register by CPU now\n",
			drm_crtc_index(&mtk_crtc->base));
		mtk_crtc->cmdq_client.chan = NULL;
	}

	if (mtk_crtc->cmdq_client.chan) {
		ret = of_property_read_u32_index(priv->mutex_node,
						 "mediatek,gce-events",
						 i,
						 &mtk_crtc->cmdq_event);
		if (ret) {
			/*
			 * No mainline "mediatek,gce-events" property: leave the
			 * channel alone.  Forcing a vendor event here (0x1b2,
			 * disp_mutex0_eof) keeps the channel allocated, but the
			 * command-mode flip completion is then skipped and every
			 * atomic commit waits the full 10 s flip_done timeout.
			 */
		}
		if (ret) {
			dev_dbg(dev, "mtk_crtc %d failed to get mediatek,gce-events property\n",
				drm_crtc_index(&mtk_crtc->base));
			mbox_free_channel(mtk_crtc->cmdq_client.chan);
			mtk_crtc->cmdq_client.chan = NULL;
		} else {
			dev_info(dev, "mtk_crtc %d: CMDQ active, event 0x%x\n",
				 drm_crtc_index(&mtk_crtc->base),
				 mtk_crtc->cmdq_event);
			ret = cmdq_pkt_create(&mtk_crtc->cmdq_client,
					      &mtk_crtc->cmdq_handle,
					      PAGE_SIZE);
			if (ret) {
				dev_dbg(dev, "mtk_crtc %d failed to create cmdq packet\n",
					drm_crtc_index(&mtk_crtc->base));
				mbox_free_channel(mtk_crtc->cmdq_client.chan);
				mtk_crtc->cmdq_client.chan = NULL;
			}
		}

		/* for sending blocking cmd in crtc disable */
		init_waitqueue_head(&mtk_crtc->cb_blocking_queue);
	}
#endif

	if (conn_routes) {
		for (i = 0; i < num_conn_routes; i++) {
			unsigned int comp_id = conn_routes[i].route_ddp;
			struct device_node *node = priv->comp_node[comp_id];
			struct mtk_ddp_comp *comp = &priv->ddp_comp[comp_id];

			if (!comp->dev) {
				dev_dbg(dev, "comp_id:%d, Component %pOF not initialized\n",
					comp_id, node);
				/* mark encoder_index to -1, if route comp device is not enabled */
				comp->encoder_index = -1;
				continue;
			}

			mtk_ddp_comp_encoder_index_set(&priv->ddp_comp[comp_id]);
		}

		mtk_crtc->num_conn_routes = num_conn_routes;
		mtk_crtc->conn_routes = conn_routes;

		/* increase ddp_comp_nr at the end of mtk_crtc_create */
		mtk_crtc->ddp_comp_nr++;
	}

	/*
	 * The "te-gpios" property lives on the mmsys node, but the GPIO and the
	 * IRQ must be owned by this module: the mmsys device belongs to a
	 * different driver, so devm resources attached to it would survive an
	 * rmmod of mediatek_drm and leave a dangling interrupt handler behind.
	 */
	if (te_pacing) {
		mtk_crtc->te_gpio = fwnode_gpiod_get_index(
			of_fwnode_handle(mtk_crtc->mmsys_dev->of_node),
			"te", 0, GPIOD_IN, "x676b-te");
		if (IS_ERR(mtk_crtc->te_gpio))
			mtk_crtc->te_gpio = NULL;
	}
	if (mtk_crtc->te_gpio) {
		mtk_crtc->te_dev = dev;
		mtk_crtc->te_irq = gpiod_to_irq(mtk_crtc->te_gpio);
		if (mtk_crtc->te_irq > 0) {
			if (!devm_request_irq(dev, mtk_crtc->te_irq,
					      mtk_crtc_te_isr,
					      IRQF_TRIGGER_RISING,
					      "x676b-te", mtk_crtc)) {
				mtk_crtc->te_mode = true;
				dev_info(dev, "command mode frames paced by TE (irq %d)\n",
					 mtk_crtc->te_irq);
			} else {
				mtk_crtc->te_irq = -1;
			}
		}
	}
	
	return 0;
}
