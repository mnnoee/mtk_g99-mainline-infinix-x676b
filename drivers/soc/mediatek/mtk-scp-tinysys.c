// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6789 tinysys SCP loader and mailbox/IPI transport.
 *
 * The stock MT6789 SCP firmware is not an ELF image: the bootloader loads a
 * 0x2000-byte loader into TCM at 0x10500000 and describes the DRAM-resident
 * firmware images with a struct scp_region_info at 0x10500004.  The SCP then
 * talks to the AP over five small mailboxes at 0x107fb000..0x107ff0ff with
 * shared tables taken from the stock device tree.
 *
 * This driver is experimental.  It deliberately does not release the SCP
 * unless the user writes "1" to the "start" sysfs attribute (or loads the
 * module with autostart=1).  Probe only prepares TCM/DRAM and leaves the
 * core in reset.
 */

#include <linux/arm-smccc.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/unaligned.h>

#include <linux/soc/mediatek/mtk-scp-tinysys.h>
#include <linux/wait.h>

#define SCP_TCM_BASE_DEFAULT	0x10500000UL
#define SCP_TCM_SIZE_DEFAULT	0x000c0000UL
#define SCP_DRAM_BASE_DEFAULT	0xbfa00000UL
#define SCP_DRAM_SIZE_DEFAULT	0x00400000UL

#define SCP_LOADER_SIZE		0x2000UL
#define SCP_FW_DRAM_OFFSET	0x2000UL
#define SCP_DRAM_IMG_OFFSET	0x200000UL

#define SCP_CFG_CORE0_OFFSET	0x10000UL
#define SCP_CFG_CORE1_OFFSET	0x20000UL
#define SCP_CFG_CLKREG_OFFSET	0x1000UL

#define SCP_CORE_SW_RSTN_CLR	0x0000
#define SCP_CORE_SW_RSTN_SET	0x0004
#define SCP_CORE_MEM_ATT_PREDEF	0x0008
#define SCP_CORE_DRAM_RESV_ADDR	0x0044
#define SCP_CORE_DRAM_RESV_SIZE	0x0048
#define SCP_CORE_STATUS		0x0070
#define SCP_CORE_MON_PC		0x0080
#define SCP_CORE_MON_LR		0x0084
#define SCP_CORE_MON_SP		0x0088

#define SCP_CLK_SW_SEL		0x0000
#define SCP_CLK_ENABLE		0x0004
#define SCP_CLK_L1_SRAM_PD	0x002c
#define SCP_CLK_CPU0_SRAM_PD	0x0080
#define SCP_CLK_CPU1_SRAM_PD	0x0084
#define SCP_CLK_TCM_TAIL_SRAM_PD 0x0094
#define SCP_CLK_L2TCM_PD0	0x10c0
#define SCP_CLK_L2TCM_PD1	0x10c4
#define SCP_CLK_L2TCM_PD2	0x10c8

#define SCP_CFG_SCP2SPM_IPC_CLR	0x4094
#define SCP_CFG_GIPC_IN_SET	0x4098

#define SCP_MAX_MBOX		5
#define SCP_MAX_IPI		64
#define SCP_MBOX_SLOT_SIZE	4
#define SCP_MBOX_SLOTS		64

#define SCP_IPI_ACTION_DONE	0
#define SCP_IPI_DEV_ILLEGAL	-1
#define SCP_IPI_UNAVAILABLE	-3
#define SCP_IPI_NO_MSGBUF	-4
#define SCP_IPI_NO_MEMORY	-5
#define SCP_IPI_PIN_BUSY	-6

#define SCP_IPI_IN_SCP_READY_0	9

/* struct scp_region_info_st from the vendor scp_helper.h */
struct scp_region_info {
	u32 ap_loader_start;
	u32 ap_loader_size;
	u32 ap_firmware_start;
	u32 ap_firmware_size;
	u32 ap_dram_start;
	u32 ap_dram_size;
	u32 ap_dram_backup_start;
	u32 struct_size;
	u32 scp_log_thru_ap_uart;
	u32 task_context_ptr;
	u32 scpctl;
	u32 regdump_start;
	u32 regdump_size;
	u32 ap_params_start;
} __packed;

struct scp_send_pin {
	u32 chan_id;
	u32 mbox;
	u32 msg_size;		/* slots, 4 bytes each */
	u32 offset;		/* slot offset in the mbox window */
	u32 pin_index;
};

struct scp_recv_pin {
	u32 chan_id;
	u32 mbox;
	u32 msg_size;
	u32 recv_opt;
	u32 offset;
	u32 pin_index;
	int (*cb)(unsigned int id, void *prdata, void *data, unsigned int len);
	void *prdata;
	void *buf;
	u32 buf_size;
	bool used;
};

struct scp_mbox {
	void __iomem *base;
	void __iomem *set;
	void __iomem *clr;
	int irq;
	bool is64d;
};

struct mtk_ipi_device {
	const char *name;
	struct scp_dev *scp;
};

struct scp_dev {
	struct device *dev;
	struct clk *clk;

	void __iomem *tcm;
	phys_addr_t tcm_phys;
	size_t tcm_size;
	void __iomem *cfg;
	void __iomem *scpsys;
	void __iomem *clkreg;
	void __iomem *dram;
	phys_addr_t dram_phys;
	size_t dram_size;

	struct scp_mbox mbox[SCP_MAX_MBOX];
	struct scp_send_pin send[SCP_MAX_IPI];
	unsigned int send_count;
	struct scp_recv_pin recv[SCP_MAX_IPI];
	unsigned int recv_count;

	struct mtk_ipi_device ipidev;
	struct mutex lock;
	wait_queue_head_t ready_wq;
	struct work_struct notify_work;
	bool prepared;
	bool running;
	bool ready;
	u32 last_ipi;
	u32 last_ipi_len;
	u32 ipi_count;
	u32 fw_sram_size;
	u32 fw_dram_size;
};

/*
 * Stock MT6789 send/recv tables (dt: send_item_num=3, recv_item_num=4).
 * send: chan_id, mbox, msg_size(slots)
 * recv: chan_id, mbox, msg_size(slots), recv_opt
 */
static const u32 mt6789_send_table[] = {
	0x00, 0, 0x09,	0x03, 1, 0x02,	0x04, 1, 0x01,
	0x05, 1, 0x02,	0x06, 1, 0x01,	0x1a, 1, 0x09,
	0x1d, 2, 0x10,	0x1f, 2, 0x07,	0x0e, 3, 0x01,
	0x0f, 3, 0x02,	0x10, 3, 0x01,	0x11, 3, 0x06,
	0x12, 3, 0x02,	0x18, 4, 0x04,
};

static const u32 mt6789_recv_table[] = {
	0x01, 0, 0x02, 0,	0x02, 0, 0x1a, 0,
	0x07, 1, 0x02, 0,	0x08, 1, 0x0a, 0,
	0x09, 1, 0x01, 0,	0x0a, 1, 0x02, 0,
	0x1b, 1, 0x02, 0,	0x1c, 1, 0x05, 0,
	0x05, 1, 0x01, 1,	0x1e, 2, 0x02, 0,
	0x20, 2, 0x07, 0,	0x2a, 2, 0x08, 0,
	0x14, 3, 0x0a, 0,	0x15, 3, 0x06, 0,
	0x16, 3, 0x01, 0,	0x17, 3, 0x02, 0,
	0x0f, 3, 0x01, 1,	0x19, 4, 0x04, 0,
};

static bool autostart;
module_param(autostart, bool, 0444);
MODULE_PARM_DESC(autostart, "Release the SCP during probe (default: 0)");

static int release_method;
module_param(release_method, int, 0644);
MODULE_PARM_DESC(release_method,
		 "0 = direct SW_RSTN_CLR, 1 = ATF SMC 0xc2000528 op3");

static unsigned int log_offset;
module_param(log_offset, uint, 0644);
MODULE_PARM_DESC(log_offset, "Offset inside the SCP logger ring");

static bool assert_on_timeout = true;
module_param(assert_on_timeout, bool, 0644);
MODULE_PARM_DESC(assert_on_timeout,
		 "Assert SCP reset when the ready IPI times out (default: 1)");

int mtk_ipi_register(struct mtk_ipi_device *ipidev, int id,
		     int (*cb)(unsigned int id, void *prdata, void *data,
			       unsigned int len),
		     void *prdata, void *msg);
int mtk_ipi_unregister(struct mtk_ipi_device *ipidev, int id);
int mtk_ipi_send(struct mtk_ipi_device *ipidev, int id, int opt, void *data,
		 int len, int timeout);
static int scp_awake_lock(struct scp_dev *scp);
static void scp_awake_unlock(struct scp_dev *scp);

struct mtk_ipi_device scp_ipidev;
EXPORT_SYMBOL_GPL(scp_ipidev);

/*
 * Stock MT6789 SCP share-memory table (dt: <id size alignment>).
 * The sensorhub blocks are SENS_MEM_ID (2), SENS_SUPER_MEM_ID (8) and
 * SENS_LIST_MEM_ID (9); the offsets below are cumulative.
 */
static const u32 mt6789_share_sizes[NUMS_MEM_ID] = {
	0x200000, 0x4e300, 0x100000, 0x180000, 0x19000, 0x5a00, 0x100,
	0x19000, 0x10000, 0x1000, 0x2000, 0x100, 0x100, 0x19000,
};
static phys_addr_t scp_share_phys = 0x8f000000;
static void __iomem *scp_share_virt;

static BLOCKING_NOTIFIER_HEAD(scp_platform_notifier);
static bool scp_platform_ready_state;

phys_addr_t scp_get_reserve_mem_phys(enum scp_reserve_mem_id id)
{
	u32 off = 0;
	int i;

	if (id >= NUMS_MEM_ID || !scp_share_phys)
		return 0;
	for (i = 0; i < id; i++)
		off += mt6789_share_sizes[i];
	return scp_share_phys + off;
}
EXPORT_SYMBOL_GPL(scp_get_reserve_mem_phys);

phys_addr_t scp_get_reserve_mem_virt(enum scp_reserve_mem_id id)
{
	u32 off = 0;
	int i;

	if (id >= NUMS_MEM_ID || !scp_share_virt)
		return 0;
	for (i = 0; i < id; i++)
		off += mt6789_share_sizes[i];
	return (phys_addr_t)(uintptr_t)(scp_share_virt + off);
}
EXPORT_SYMBOL_GPL(scp_get_reserve_mem_virt);

phys_addr_t scp_get_reserve_mem_size(enum scp_reserve_mem_id id)
{
	if (id >= NUMS_MEM_ID)
		return 0;
	return mt6789_share_sizes[id];
}
EXPORT_SYMBOL_GPL(scp_get_reserve_mem_size);

void scp_A_register_notify(struct notifier_block *nb)
{
	blocking_notifier_chain_register(&scp_platform_notifier, nb);
	if (READ_ONCE(scp_platform_ready_state))
		nb->notifier_call(nb, SCP_EVENT_READY, NULL);
}
EXPORT_SYMBOL_GPL(scp_A_register_notify);

void scp_A_unregister_notify(struct notifier_block *nb)
{
	blocking_notifier_chain_unregister(&scp_platform_notifier, nb);
}
EXPORT_SYMBOL_GPL(scp_A_unregister_notify);

int scp_wdt_reset(unsigned int core)
{
	pr_warn("mtk-scp-tinysys: scp_wdt_reset(%u) is not implemented yet\n",
		core);
	return 0;
}
EXPORT_SYMBOL_GPL(scp_wdt_reset);

int scp_register_sensor(int feature, int sensor)
{
	return 0;
}
EXPORT_SYMBOL_GPL(scp_register_sensor);

int scp_deregister_sensor(int feature, int sensor)
{
	return 0;
}
EXPORT_SYMBOL_GPL(scp_deregister_sensor);

static void scp_core_writel(struct scp_dev *scp, u32 core, u32 off, u32 val)
{
	void __iomem *base = scp->cfg + (core ? SCP_CFG_CORE1_OFFSET :
						 SCP_CFG_CORE0_OFFSET);
	writel(val, base + off);
}

static u32 scp_core_readl(struct scp_dev *scp, u32 core, u32 off)
{
	void __iomem *base = scp->cfg + (core ? SCP_CFG_CORE1_OFFSET :
						 SCP_CFG_CORE0_OFFSET);
	return readl(base + off);
}

static void scp_dump_regs(struct scp_dev *scp)
{
	dev_info(scp->dev, "TCM %#lx+%#zx DRAM %#lx+%#zx\n",
		 (unsigned long)scp->tcm_phys, scp->tcm_size,
		 (unsigned long)scp->dram_phys, scp->dram_size);
	dev_info(scp->dev, "core0 reset clr=%#x set=%#x status=%#x pc=%#x lr=%#x sp=%#x\n",
		 scp_core_readl(scp, 0, SCP_CORE_SW_RSTN_CLR),
		 scp_core_readl(scp, 0, SCP_CORE_SW_RSTN_SET),
		 scp_core_readl(scp, 0, SCP_CORE_STATUS),
		 scp_core_readl(scp, 0, SCP_CORE_MON_PC),
		 scp_core_readl(scp, 0, SCP_CORE_MON_LR),
		 scp_core_readl(scp, 0, SCP_CORE_MON_SP));
	dev_info(scp->dev, "core1 reset clr=%#x set=%#x status=%#x\n",
		 scp_core_readl(scp, 1, SCP_CORE_SW_RSTN_CLR),
		 scp_core_readl(scp, 1, SCP_CORE_SW_RSTN_SET),
		 scp_core_readl(scp, 1, SCP_CORE_STATUS));
	dev_info(scp->dev, "mbox init %#x %#x %#x %#x %#x\n",
		 readl(scp->cfg + 0x85020), readl(scp->cfg + 0x85024),
		 readl(scp->cfg + 0x85028), readl(scp->cfg + 0x8502c),
		 readl(scp->cfg + 0x85030));
	dev_info(scp->dev, "clk sw_sel=%#x enable=%#x l1pd=%#x cpu0pd=%#x l2pd=%#x/%#x/%#x\n",
		 readl(scp->clkreg + SCP_CLK_SW_SEL),
		 readl(scp->clkreg + SCP_CLK_ENABLE),
		 readl(scp->clkreg + SCP_CLK_L1_SRAM_PD),
		 readl(scp->clkreg + SCP_CLK_CPU0_SRAM_PD),
		 readl(scp->clkreg + SCP_CLK_L2TCM_PD0),
		 readl(scp->clkreg + SCP_CLK_L2TCM_PD1),
		 readl(scp->clkreg + SCP_CLK_L2TCM_PD2));
}

static int scp_parse_container(struct scp_dev *scp, const struct firmware *fw,
			       const u8 **sram, u32 *sram_size,
			       const u8 **dram, u32 *dram_size)
{
	static const char sram_name[] = "tinysys-scp-RV55_A";
	static const char dram_name[] = "tinysys-scp-RV55_A_dram";
	const u8 *p, *end = fw->data + fw->size;
	const u8 *s = NULL, *d = NULL;
	u32 ss = 0, ds = 0;

	p = fw->data;
	while (p + sizeof(u32) < end) {
		const u8 *q = memchr(p, 't', end - p);
		const u8 *hdr;
		u32 magic, dsize, hdr_size;

		if (!q)
			break;
		p = q + 1;
		if (q + 8 + sizeof(sram_name) > end)
			continue;
		if (!memcmp(q, sram_name, sizeof(sram_name) - 1) && !s) {
			hdr = q - 8;
			if (hdr < fw->data)
				continue;
			magic = get_unaligned_le32(hdr);
			if (magic != 0x58881688)
				continue;
			dsize = get_unaligned_le32(hdr + 4);
			hdr_size = get_unaligned_le32(hdr + 0x34);
			if (get_unaligned_le32(hdr + 0x30) != 0x58891689 ||
			    hdr_size != 0x200)
				continue;
			if (hdr + hdr_size + dsize > end || dsize < SCP_LOADER_SIZE)
				continue;
			s = hdr + hdr_size;
			ss = dsize;
		} else if (!memcmp(q, dram_name, sizeof(dram_name) - 1) && !d) {
			hdr = q - 8;
			if (hdr < fw->data)
				continue;
			magic = get_unaligned_le32(hdr);
			if (magic != 0x58881688)
				continue;
			dsize = get_unaligned_le32(hdr + 4);
			hdr_size = get_unaligned_le32(hdr + 0x34);
			if (get_unaligned_le32(hdr + 0x30) != 0x58891689 ||
			    hdr_size != 0x200)
				continue;
			if (hdr + hdr_size + dsize > end || !dsize)
				continue;
			d = hdr + hdr_size;
			ds = dsize;
		}
		if (s && d)
			break;
	}

	if (!s || !d) {
		dev_err(scp->dev, "firmware container: payloads not found (sram=%p dram=%p)\n",
			s, d);
		return -EINVAL;
	}
	if (ss > scp->tcm_size) {
		dev_err(scp->dev, "SRAM image too large: %#x > %#zx\n", ss, scp->tcm_size);
		return -E2BIG;
	}
	if (ds > scp->dram_size - SCP_DRAM_IMG_OFFSET) {
		dev_err(scp->dev, "DRAM image too large: %#x\n", ds);
		return -E2BIG;
	}

	*sram = s;
	*sram_size = ss;
	*dram = d;
	*dram_size = ds;
	return 0;
}

static int scp_load_firmware(struct scp_dev *scp)
{
	const struct firmware *fw;
	const u8 *sram, *dram;
	u32 sram_size, dram_size;
	struct scp_region_info ri;
	int ret;

	ret = request_firmware(&fw, "mediatek/mt6789/scp.img", scp->dev);
	if (ret) {
		dev_err(scp->dev, "request_firmware failed: %d\n", ret);
		return ret;
	}

	ret = scp_parse_container(scp, fw, &sram, &sram_size, &dram, &dram_size);
	if (ret)
		goto out;

	dev_info(scp->dev, "firmware: TCM %#x bytes, DRAM %#x bytes\n",
		 sram_size, dram_size);

	/* Clear TCM and copy the 0x2000-byte loader. */
	memset_io(scp->tcm, 0, scp->tcm_size);
	memcpy_toio(scp->tcm, sram, SCP_LOADER_SIZE);
	{
		u32 check[4];

		memcpy_fromio(check, scp->tcm, sizeof(check));
		dev_info(scp->dev, "TCM readback: %08x %08x %08x %08x\n",
			 check[0], check[1], check[2], check[3]);
	}

	/*
	 * DRAM layout expected by the SCP loader:
	 *   +0x000000 loader (0x2000)
	 *   +0x002000 full TCM image
	 *   +0x200000 DRAM image
	 */
	memset_io(scp->dram, 0, scp->dram_size);
	memcpy_toio(scp->dram, sram, SCP_LOADER_SIZE);
	memcpy_toio(scp->dram + SCP_FW_DRAM_OFFSET, sram, sram_size);
	memcpy_toio(scp->dram + SCP_DRAM_IMG_OFFSET, dram, dram_size);

	memset(&ri, 0, sizeof(ri));
	ri.ap_loader_start = lower_32_bits(scp->dram_phys);
	ri.ap_loader_size = SCP_LOADER_SIZE;
	ri.ap_firmware_start = lower_32_bits(scp->dram_phys + SCP_FW_DRAM_OFFSET);
	ri.ap_firmware_size = sram_size;
	ri.ap_dram_start = lower_32_bits(scp->dram_phys + SCP_DRAM_IMG_OFFSET);
	ri.ap_dram_size = dram_size;
	ri.ap_dram_backup_start = lower_32_bits(scp->dram_phys +
					       SCP_DRAM_IMG_OFFSET +
					       ALIGN(dram_size, 1024));
	ri.struct_size = sizeof(ri);
	memcpy_toio(scp->tcm + 4, &ri, sizeof(ri));

	scp->fw_sram_size = sram_size;
	scp->fw_dram_size = dram_size;
out:
	release_firmware(fw);
	return ret;
}

static void scp_assert_reset(struct scp_dev *scp)
{
	scp_core_writel(scp, 0, SCP_CORE_SW_RSTN_SET, 1);
	scp_core_writel(scp, 1, SCP_CORE_SW_RSTN_SET, 1);
	usleep_range(100, 200);
}

static void scp_prepare_hw(struct scp_dev *scp)
{
	/* Keep the core in reset while the SRAM comes up. */
	scp_assert_reset(scp);

	/*
	 * Power up all TCM banks.  The vendor enables the CPU SRAM banks one
	 * block at a time; keep that sequence to avoid an inrush glitch.
	 */
	{
		u32 mask;

		for (mask = 0xffffffff; mask; ) {
			mask >>= 1;
			writel(mask, scp->clkreg + SCP_CLK_CPU0_SRAM_PD);
			writel(mask, scp->clkreg + SCP_CLK_CPU1_SRAM_PD);
		}
	}
	writel(0, scp->clkreg + SCP_CLK_L1_SRAM_PD);
	writel(0, scp->clkreg + SCP_CLK_TCM_TAIL_SRAM_PD);
	writel(0, scp->clkreg + SCP_CLK_L2TCM_PD0);
	writel(0, scp->clkreg + SCP_CLK_L2TCM_PD1);
	writel(0, scp->clkreg + SCP_CLK_L2TCM_PD2);

	writel(0, scp->clkreg + SCP_CLK_SW_SEL);
	/* Point the SCP-side mailbox base at the AP data windows. */
	{
		u32 i;

		for (i = 0; i < SCP_MAX_MBOX; i++)
			writel(0x107fb000 + i * 0x1000,
			       scp->cfg + 0x85020 + i * 4);
	}
	/* Clear the SCP->SPM IPC and allow all memory attributes. */
	writel(1, scp->clkreg + SCP_CLK_ENABLE);
	writel(0xff, scp->cfg + SCP_CFG_SCP2SPM_IPC_CLR);
	scp_core_writel(scp, 0, SCP_CORE_MEM_ATT_PREDEF, 0xff);

	/* Tell the SCP where its DRAM window is. */
	scp_core_writel(scp, 0, SCP_CORE_DRAM_RESV_ADDR,
			lower_32_bits(scp->dram_phys));
	scp_core_writel(scp, 0, SCP_CORE_DRAM_RESV_SIZE, scp->dram_size);
}

static int scp_prepare(struct scp_dev *scp)
{
	int ret;

	mutex_lock(&scp->lock);
	if (scp->prepared) {
		mutex_unlock(&scp->lock);
		return 0;
	}

	/* Power the TCM before writing the loader into it. */
	scp_prepare_hw(scp);
	ret = scp_load_firmware(scp);
	if (ret)
		goto out;

	scp->prepared = true;
	dev_info(scp->dev, "SCP prepared (still in reset)\n");
	scp_dump_regs(scp);
out:
	mutex_unlock(&scp->lock);
	return ret;
}

static void scp_mbox_isr_work(struct scp_dev *scp, unsigned int mbox)
{
	struct scp_mbox *mb = &scp->mbox[mbox];
	u32 status, handled = 0;
	unsigned int i;

	status = readl(mb->clr);
	if (!status)
		return;

	for (i = 0; i < scp->recv_count; i++) {
		struct scp_recv_pin *pin = &scp->recv[i];
		u32 len;

		if (pin->mbox != mbox)
			continue;
		if (!(status & BIT(pin->pin_index)))
			continue;

		handled |= BIT(pin->pin_index);
		if (!pin->buf || pin->buf_size < pin->msg_size * SCP_MBOX_SLOT_SIZE) {
			dev_warn(scp->dev, "IPI %u: no receive buffer\n", pin->chan_id);
			continue;
		}

		len = pin->msg_size * SCP_MBOX_SLOT_SIZE;
		memcpy_fromio(pin->buf, mb->base + pin->offset * SCP_MBOX_SLOT_SIZE, len);

		scp->last_ipi = pin->chan_id;
		scp->last_ipi_len = len;
		scp->ipi_count++;

		dev_dbg(scp->dev, "IPI in: id=%u len=%u\n", pin->chan_id, len);
		if (pin->cb)
			pin->cb(pin->chan_id, pin->prdata, pin->buf, len);
	}

	if (handled)
		writel(handled, mb->clr);
}

static irqreturn_t scp_mbox_isr(int irq, void *data)
{
	struct scp_dev *scp = data;
	unsigned int i;

	for (i = 0; i < SCP_MAX_MBOX; i++) {
		if (scp->mbox[i].irq == irq) {
			scp_mbox_isr_work(scp, i);
			return IRQ_HANDLED;
		}
	}
	return IRQ_NONE;
}

static int scp_setup_pins(struct scp_dev *scp)
{
	unsigned int i, n;
	int last_ofs = 0, last_idx = 0, last_slot = 0, last_sz = 0;
	int mbox;

	scp->send_count = ARRAY_SIZE(mt6789_send_table) / 3;
	scp->recv_count = ARRAY_SIZE(mt6789_recv_table) / 4;
	if (scp->send_count > SCP_MAX_IPI || scp->recv_count > SCP_MAX_IPI)
		return -EINVAL;

	for (n = 0; n < scp->send_count; n++) {
		scp->send[n].chan_id = mt6789_send_table[n * 3 + 0];
		scp->send[n].mbox = mt6789_send_table[n * 3 + 1];
		scp->send[n].msg_size = mt6789_send_table[n * 3 + 2];
	}
	for (n = 0; n < scp->recv_count; n++) {
		scp->recv[n].chan_id = mt6789_recv_table[n * 4 + 0];
		scp->recv[n].mbox = mt6789_recv_table[n * 4 + 1];
		scp->recv[n].msg_size = mt6789_recv_table[n * 4 + 2];
		scp->recv[n].recv_opt = mt6789_recv_table[n * 4 + 3];
	}

	/* Same packing as vendor mbox_setup_pin_table(). */
	for (mbox = 0; mbox < SCP_MAX_MBOX; mbox++) {
		last_ofs = last_idx = last_slot = last_sz = 0;
		for (i = 0; i < scp->send_count; i++) {
			struct scp_send_pin *p = &scp->send[i];

			if (p->mbox != mbox)
				continue;
			p->offset = last_ofs + last_slot;
			p->pin_index = last_idx + last_sz;
			last_idx = p->pin_index;
			if (scp->mbox[mbox].is64d) {
				last_sz = DIV_ROUND_UP(p->msg_size, 2);
				last_ofs = last_sz * 2;
				last_slot = last_idx * 2;
			} else {
				last_sz = p->msg_size;
				last_ofs = last_sz;
				last_slot = last_idx;
			}
		}
		/* Vendor mbox_setup_pin_table() continues from the send pins. */
		for (i = 0; i < scp->recv_count; i++) {
			struct scp_recv_pin *p = &scp->recv[i];
			u32 len;

			if (p->mbox != mbox)
				continue;
			p->offset = last_ofs + last_slot;
			p->pin_index = last_idx + last_sz;
			last_idx = p->pin_index;
			if (scp->mbox[mbox].is64d) {
				last_sz = DIV_ROUND_UP(p->msg_size, 2);
				last_ofs = last_sz * 2;
				last_slot = last_idx * 2;
			} else {
				last_sz = p->msg_size;
				last_ofs = last_sz;
				last_slot = last_idx;
			}

			len = p->msg_size * SCP_MBOX_SLOT_SIZE;
			p->buf = devm_kzalloc(scp->dev, len, GFP_KERNEL);
			if (!p->buf)
				return -ENOMEM;
			p->buf_size = len;
		}
		if (last_idx > 32 ||
		    (last_ofs + last_slot) > (scp->mbox[mbox].is64d + 1) * 32) {
			dev_err(scp->dev, "mbox%d layout overflow: idx=%d ofs=%d\n",
				mbox, last_idx, last_ofs + last_slot);
			return -EINVAL;
		}
	}
	return 0;
}

static struct scp_send_pin *scp_find_send(struct scp_dev *scp, int id)
{
	unsigned int i;

	for (i = 0; i < scp->send_count; i++)
		if (scp->send[i].chan_id == id)
			return &scp->send[i];
	return NULL;
}

static struct scp_recv_pin *scp_find_recv(struct scp_dev *scp, int id)
{
	unsigned int i;

	for (i = 0; i < scp->recv_count; i++)
		if (scp->recv[i].chan_id == id)
			return &scp->recv[i];
	return NULL;
}

int mtk_ipi_register(struct mtk_ipi_device *ipidev, int id,
		     int (*cb)(unsigned int id, void *prdata, void *data,
			       unsigned int len),
		     void *prdata, void *msg)
{
	struct scp_dev *scp;
	struct scp_recv_pin *pin;

	if (!ipidev || !ipidev->scp)
		return SCP_IPI_DEV_ILLEGAL;
	scp = ipidev->scp;
	pin = scp_find_recv(scp, id);
	if (!pin)
		return SCP_IPI_UNAVAILABLE;

	mutex_lock(&scp->lock);
	pin->cb = cb;
	pin->prdata = prdata;
	if (msg) {
		pin->buf = msg;
		pin->buf_size = pin->msg_size * SCP_MBOX_SLOT_SIZE;
	}
	pin->used = true;
	mutex_unlock(&scp->lock);
	return SCP_IPI_ACTION_DONE;
}
EXPORT_SYMBOL_GPL(mtk_ipi_register);

int mtk_ipi_unregister(struct mtk_ipi_device *ipidev, int id)
{
	struct scp_dev *scp;
	struct scp_recv_pin *pin;

	if (!ipidev || !ipidev->scp)
		return SCP_IPI_DEV_ILLEGAL;
	scp = ipidev->scp;
	pin = scp_find_recv(scp, id);
	if (!pin)
		return SCP_IPI_UNAVAILABLE;

	mutex_lock(&scp->lock);
	pin->cb = NULL;
	pin->prdata = NULL;
	pin->used = false;
	mutex_unlock(&scp->lock);
	return SCP_IPI_ACTION_DONE;
}
EXPORT_SYMBOL_GPL(mtk_ipi_unregister);

int mtk_ipi_send(struct mtk_ipi_device *ipidev, int id, int opt, void *data,
		 int len, int timeout)
{
	struct scp_dev *scp;
	struct scp_send_pin *pin;
	struct scp_mbox *mb;
	u32 bytes;
	int ret = 0;

	if (!ipidev || !ipidev->scp)
		return SCP_IPI_DEV_ILLEGAL;
	scp = ipidev->scp;
	pin = scp_find_send(scp, id);
	if (!pin)
		return SCP_IPI_UNAVAILABLE;
	if (!data || len <= 0)
		return SCP_IPI_NO_MEMORY;
	if (len > pin->msg_size)
		return SCP_IPI_NO_MEMORY;

	mb = &scp->mbox[pin->mbox];
	bytes = len * SCP_MBOX_SLOT_SIZE;

	mutex_lock(&scp->lock);
	if (readl(mb->set) & BIT(pin->pin_index)) {
		ret = SCP_IPI_PIN_BUSY;
		goto out;
	}
	scp_awake_lock(scp);
	memcpy_toio(mb->base + pin->offset * SCP_MBOX_SLOT_SIZE, data, bytes);
	/* Make sure the payload is visible before the doorbell. */
	wmb();
	writel(BIT(pin->pin_index), mb->set);
	scp_awake_unlock(scp);
out:
	mutex_unlock(&scp->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mtk_ipi_send);

static void scp_notify_work(struct work_struct *work)
{
	blocking_notifier_call_chain(&scp_platform_notifier, SCP_EVENT_READY,
				     NULL);
}

static int scp_ready_handler(unsigned int id, void *prdata, void *data,
			     unsigned int len)
{
	struct scp_dev *scp = prdata;
	u32 image_size = 0;

	if (len >= 4)
		image_size = get_unaligned_le32(data);
	dev_info(scp->dev, "SCP ready IPI: id=%u len=%u image_size=%#x (expected %#x)\n",
		 id, len, image_size, scp->fw_sram_size);
	scp->ready = true;
	WRITE_ONCE(scp_platform_ready_state, true);
	schedule_work(&scp->notify_work);
	wake_up(&scp->ready_wq);
	return 0;
}

static int scp_awake_lock(struct scp_dev *scp)
{
	u32 val;
	int i;

	if (!scp->ready)
		return -1;
	writel(0xA1, scp->scpsys + 0x0b14);
	for (i = 0; i < 100000; i++) {
		val = readl(scp->scpsys + 0x0b14);
		if ((val & 0xA0) != 0xA0 || !(val & BIT(0)))
			break;
		udelay(10);
	}
	writel(0xA1, scp->scpsys + 0x0b18);
	return (i < 100000) ? 0 : -1;
}

static void scp_awake_unlock(struct scp_dev *scp)
{
	if (!scp->ready)
		return;
	writel(0xA0, scp->scpsys + 0x0b14);
	udelay(20);
	writel(0xA0, scp->scpsys + 0x0b18);
}

static int scp_start(struct scp_dev *scp)
{
	unsigned long timeout;
	int ret;

	mutex_lock(&scp->lock);
	if (!scp->prepared) {
		ret = -EINVAL;
		goto out;
	}
	if (scp->running) {
		ret = 0;
		goto out;
	}

	scp->ready = false;
	scp->running = true;
	/* Re-program the DRAM window immediately before release. */
	scp_core_writel(scp, 0, SCP_CORE_DRAM_RESV_ADDR,
			lower_32_bits(scp->dram_phys));
	scp_core_writel(scp, 0, SCP_CORE_DRAM_RESV_SIZE, scp->dram_size);
	wmb();
	if (release_method == 1) {
		struct arm_smccc_res res;

		dev_info(scp->dev, "releasing SCP via ATF SMC op3\n");
		arm_smccc_smc(0xc2000528, 3, 0, 0, 0, 0, 0, 0, &res);
		dev_info(scp->dev, "SMC op3 -> a0=%#lx a1=%#lx a2=%#lx a3=%#lx\n",
			 res.a0, res.a1, res.a2, res.a3);
	} else {
		dev_info(scp->dev, "releasing SCP core0 (SW_RSTN_CLR)\n");
		scp_core_writel(scp, 0, SCP_CORE_SW_RSTN_CLR, 1);
	}
	mutex_unlock(&scp->lock);

	timeout = wait_event_timeout(scp->ready_wq, scp->ready,
				     msecs_to_jiffies(10000));
	mutex_lock(&scp->lock);
	if (!timeout) {
		dev_err(scp->dev, "SCP did not send ready IPI in 10s\n");
		scp->running = false;
		if (assert_on_timeout)
			scp_assert_reset(scp);
		scp_dump_regs(scp);
		ret = -ETIMEDOUT;
	} else {
		dev_info(scp->dev, "SCP is running\n");
		ret = 0;
	}
	mutex_unlock(&scp->lock);
	return ret;

out:
	mutex_unlock(&scp->lock);
	return ret;
}

static void scp_stop(struct scp_dev *scp)
{
	mutex_lock(&scp->lock);
	scp_assert_reset(scp);
	scp->running = false;
	scp->ready = false;
	mutex_unlock(&scp->lock);
	WRITE_ONCE(scp_platform_ready_state, false);
	blocking_notifier_call_chain(&scp_platform_notifier, SCP_EVENT_STOP, NULL);
	dev_info(scp->dev, "SCP held in reset\n");
}

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct scp_dev *scp = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s%s\n",
			  scp->running ? "running" : (scp->prepared ? "prepared" : "off"),
			  scp->ready ? " ready" : "");
}
static DEVICE_ATTR_RO(state);

static ssize_t start_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct scp_dev *scp = dev_get_drvdata(dev);
	bool val;
	int ret;

	ret = kstrtobool(buf, &val);
	if (ret)
		return ret;
	if (val) {
		ret = scp_start(scp);
		if (ret)
			return ret;
	} else {
		scp_stop(scp);
	}
	return count;
}
static DEVICE_ATTR_WO(start);

static ssize_t regs_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct scp_dev *scp = dev_get_drvdata(dev);

	scp_dump_regs(scp);
	return sysfs_emit(buf, "dumped to dmesg\n");
}
static DEVICE_ATTR_RO(regs);

static ssize_t mbox_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct scp_dev *scp = dev_get_drvdata(dev);
	ssize_t n = 0;
	unsigned int i;

	n += sysfs_emit_at(buf, n, "mboxes:\n");
	for (i = 0; i < SCP_MAX_MBOX; i++)
		n += sysfs_emit_at(buf, n, "  %u base=%p set=%#x clr=%#x irq=%d\n",
				i, scp->mbox[i].base,
				readl(scp->mbox[i].set), readl(scp->mbox[i].clr),
				scp->mbox[i].irq);
	n += sysfs_emit_at(buf, n, "send pins:\n");
	for (i = 0; i < scp->send_count; i++)
		n += sysfs_emit_at(buf, n, "  id=%u mbox=%u size=%u off=%u pin=%u\n",
				scp->send[i].chan_id, scp->send[i].mbox,
				scp->send[i].msg_size, scp->send[i].offset,
				scp->send[i].pin_index);
	n += sysfs_emit_at(buf, n, "recv pins:\n");
	for (i = 0; i < scp->recv_count; i++)
		n += sysfs_emit_at(buf, n, "  id=%u mbox=%u size=%u opt=%u off=%u pin=%u%s\n",
				scp->recv[i].chan_id, scp->recv[i].mbox,
				scp->recv[i].msg_size, scp->recv[i].recv_opt,
				scp->recv[i].offset, scp->recv[i].pin_index,
				scp->recv[i].used ? " used" : "");
	return n;
}
static DEVICE_ATTR_RO(mbox);

static ssize_t dump_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct scp_dev *scp = dev_get_drvdata(dev);
	u8 tcm[64], dram0[64], dram1[64];
	ssize_t n = 0;
	int i;

	memcpy_fromio(tcm, scp->tcm, sizeof(tcm));
	memcpy_fromio(dram0, scp->dram, sizeof(dram0));
	memcpy_fromio(dram1, scp->dram + SCP_FW_DRAM_OFFSET, sizeof(dram1));
	n += sysfs_emit_at(buf, n, "TCM+0x0000: ");
	for (i = 0; i < sizeof(tcm); i++)
		n += sysfs_emit_at(buf, n, "%02x", tcm[i]);
	n += sysfs_emit_at(buf, n, "\nDRAM+0x0000: ");
	for (i = 0; i < sizeof(dram0); i++)
		n += sysfs_emit_at(buf, n, "%02x", dram0[i]);
	n += sysfs_emit_at(buf, n, "\nDRAM+0x2000: ");
	for (i = 0; i < sizeof(dram1); i++)
		n += sysfs_emit_at(buf, n, "%02x", dram1[i]);
	n += sysfs_emit_at(buf, n, "\n");
	return n;
}
static DEVICE_ATTR_RO(dump);

#define SCP_LOGGER_OFFSET 0x34e300
static ssize_t log_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	u8 tmp[1024];
	ssize_t n = 0;
	int i;

	if (!scp_share_virt)
		return sysfs_emit(buf, "share memory not mapped\n");
	memcpy_fromio(tmp, scp_share_virt + SCP_LOGGER_OFFSET + log_offset,
		      sizeof(tmp));
	n += sysfs_emit_at(buf, n, "logger @%#x+%#x:\n", SCP_LOGGER_OFFSET,
			   log_offset);
	for (i = 0; i < sizeof(tmp); i++) {
		char c = tmp[i];

		if ((c >= 0x20 && c < 0x7f) || c == '\n' || c == '\t')
			n += sysfs_emit_at(buf, n, "%c", c);
		else
			n += sysfs_emit_at(buf, n, ".");
		if ((i & 63) == 63)
			n += sysfs_emit_at(buf, n, "\n");
	}
	return n;
}
static DEVICE_ATTR_RO(log);

static ssize_t stats_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct scp_dev *scp = dev_get_drvdata(dev);

	return sysfs_emit(buf, "running=%d ready=%d ipi_count=%u last_ipi=%u last_len=%u\n",
			  scp->running, scp->ready, scp->ipi_count,
			  scp->last_ipi, scp->last_ipi_len);
}
static DEVICE_ATTR_RO(stats);

static ssize_t force_ready_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct scp_dev *scp = dev_get_drvdata(dev);
	bool val;
	int ret;

	ret = kstrtobool(buf, &val);
	if (ret)
		return ret;
	if (val) {
		scp->running = true;
		scp->ready = true;
		WRITE_ONCE(scp_platform_ready_state, true);
		schedule_work(&scp->notify_work);
		wake_up(&scp->ready_wq);
		dev_info(scp->dev, "SCP marked ready from userspace\n");
	}
	return count;
}
static DEVICE_ATTR_WO(force_ready);

static struct attribute *scp_attrs[] = {
	&dev_attr_state.attr,
	&dev_attr_start.attr,
	&dev_attr_regs.attr,
	&dev_attr_mbox.attr,
	&dev_attr_dump.attr,
	&dev_attr_log.attr,
	&dev_attr_stats.attr,
	&dev_attr_force_ready.attr,
	NULL,
};
ATTRIBUTE_GROUPS(scp);

static void __iomem *scp_map_resource(struct scp_dev *scp, const char *name)
{
	struct resource *res;

	res = platform_get_resource_byname(to_platform_device(scp->dev),
					   IORESOURCE_MEM, name);
	if (!res) {
		dev_err(scp->dev, "missing resource %s\n", name);
		return ERR_PTR(-ENODEV);
	}
	{
		void __iomem *ptr = devm_ioremap(scp->dev, res->start,
						 resource_size(res));

		return ptr ? ptr : ERR_PTR(-ENOMEM);
	}
}

static int scp_probe(struct platform_device *pdev)
{
	struct scp_dev *scp;
	struct device_node *np;
	struct reserved_mem *rmem;
	unsigned int i;
	int ret;

	scp = devm_kzalloc(&pdev->dev, sizeof(*scp), GFP_KERNEL);
	if (!scp)
		return -ENOMEM;
	scp->dev = &pdev->dev;
	mutex_init(&scp->lock);
	init_waitqueue_head(&scp->ready_wq);
	INIT_WORK(&scp->notify_work, scp_notify_work);
	platform_set_drvdata(pdev, scp);
	dev_set_drvdata(&pdev->dev, scp);

	scp->tcm = devm_ioremap_wc(scp->dev,
				   platform_get_resource_byname(pdev,
						IORESOURCE_MEM, "sram")->start,
				   resource_size(platform_get_resource_byname(pdev,
						IORESOURCE_MEM, "sram")));
	if (!scp->tcm)
		return -ENOMEM;
	scp->cfg = scp_map_resource(scp, "cfg");
	if (IS_ERR(scp->cfg))
		return PTR_ERR(scp->cfg);
	/*
	 * The clock-control registers live at cfg + 0x1000; the L2TCM
	 * power-down registers at +0x10c0 are outside the 0x1000 DT window,
	 * so use the larger cfg mapping instead of a separate ioremap.
	 */
	scp->clkreg = scp->cfg + SCP_CFG_CLKREG_OFFSET;
	scp->scpsys = scp_map_resource(scp, "scpsys");
	if (IS_ERR(scp->scpsys))
		return PTR_ERR(scp->scpsys);

	scp->clk = devm_clk_get(scp->dev, "main");
	if (IS_ERR(scp->clk))
		return dev_err_probe(scp->dev, PTR_ERR(scp->clk), "clock\n");
	ret = clk_prepare_enable(scp->clk);
	if (ret)
		return ret;

	np = of_parse_phandle(pdev->dev.of_node, "memory-region", 0);
	if (!np) {
		dev_err(scp->dev, "missing memory-region\n");
		return -ENODEV;
	}
	rmem = of_reserved_mem_lookup(np);
	of_node_put(np);
	if (!rmem) {
		dev_err(scp->dev, "reserved memory not found\n");
		return -ENODEV;
	}
	scp->dram_phys = rmem->base;
	scp->dram_size = rmem->size;
	scp->dram = devm_ioremap_wc(scp->dev, rmem->base, rmem->size);
	if (!scp->dram)
		return -ENOMEM;

	/* Share-memory pool used by the sensorhub stack. */
	scp_share_virt = devm_ioremap_wc(scp->dev, scp_share_phys, 0x5a0000);
	if (!scp_share_virt)
		dev_warn(scp->dev, "cannot map SCP share memory\n");

	scp->tcm_size = resource_size(platform_get_resource_byname(pdev,
						IORESOURCE_MEM, "sram"));
	scp->tcm_phys = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						    "sram")->start;

	/* Mailboxes: base/set/clr, then the IRQ. */
	for (i = 0; i < SCP_MAX_MBOX; i++) {
		char name[32];

		snprintf(name, sizeof(name), "mbox%u_base", i);
		scp->mbox[i].base = scp_map_resource(scp, name);
		if (IS_ERR(scp->mbox[i].base))
			return PTR_ERR(scp->mbox[i].base);
		snprintf(name, sizeof(name), "mbox%u_set", i);
		scp->mbox[i].set = scp_map_resource(scp, name);
		if (IS_ERR(scp->mbox[i].set))
			return PTR_ERR(scp->mbox[i].set);
		snprintf(name, sizeof(name), "mbox%u_clr", i);
		scp->mbox[i].clr = scp_map_resource(scp, name);
		if (IS_ERR(scp->mbox[i].clr))
			return PTR_ERR(scp->mbox[i].clr);
		scp->mbox[i].is64d = true;
		snprintf(name, sizeof(name), "mbox%u", i);
		scp->mbox[i].irq = platform_get_irq_byname(pdev, name);
		if (scp->mbox[i].irq < 0)
			return dev_err_probe(scp->dev, scp->mbox[i].irq,
					     "irq %s\n", name);
	}

	ret = scp_setup_pins(scp);
	if (ret)
		return ret;

	scp->ipidev.name = "scp_ipidev";
	scp->ipidev.scp = scp;
	scp_ipidev = scp->ipidev;

	/* Ready IPI must be armed before the core is released. */
	ret = mtk_ipi_register(&scp_ipidev, SCP_IPI_IN_SCP_READY_0,
			       scp_ready_handler, scp, NULL);
	if (ret) {
		dev_err(scp->dev, "cannot register ready IPI: %d\n", ret);
		return ret;
	}

	for (i = 0; i < SCP_MAX_MBOX; i++) {
		/* Drop stale SCP->AP bits left by the bootloader. */
		writel(0xffffffff, scp->mbox[i].clr);
	}
	for (i = 0; i < SCP_MAX_MBOX; i++) {
		ret = devm_request_irq(scp->dev, scp->mbox[i].irq,
				       scp_mbox_isr, 0,
				       dev_name(scp->dev), scp);
		if (ret)
			return dev_err_probe(scp->dev, ret, "request mbox%u irq\n", i);
	}

	ret = device_add_groups(scp->dev, scp_groups);
	if (ret)
		return ret;

	ret = scp_prepare(scp);
	if (ret)
		return ret;

	dev_info(scp->dev, "ready: write 1 to start (autostart=%d)\n", autostart);
	if (autostart)
		return scp_start(scp);
	return 0;
}

static void scp_remove(struct platform_device *pdev)
{
	struct scp_dev *scp = platform_get_drvdata(pdev);

	scp_stop(scp);
	device_remove_groups(&pdev->dev, scp_groups);
	clk_disable_unprepare(scp->clk);
}

static const struct of_device_id scp_of_match[] = {
	{ .compatible = "mediatek,mt6789-scp-tinysys" },
	{ }
};
MODULE_DEVICE_TABLE(of, scp_of_match);

static struct platform_driver scp_driver = {
	.probe = scp_probe,
	.remove = scp_remove,
	.driver = {
		.name = "mtk-scp-tinysys",
		.of_match_table = scp_of_match,
	},
};
module_platform_driver(scp_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek MT6789 tinysys SCP loader and mbox/IPI transport");
