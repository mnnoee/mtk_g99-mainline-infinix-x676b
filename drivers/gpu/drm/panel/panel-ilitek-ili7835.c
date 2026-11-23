// SPDX-License-Identifier: GPL-2.0-only
/*
 * Ilitek ILI7835 AMOLED panel driver
 *
 * 1080x2400 @ 60 Hz, 4 lanes, DSI command mode.
 * Found on the Infinix Note 12 Pro (X676B, MediaTek MT6789).
 *
 * The init sequence, power/reset timing and backlight command were
 * reverse engineered from the vendor module
 * ili7835_fhdp_dsi_cmd_edo_edo_x676b.ko (5.10 vendor kernel); see
 * docs/re/modules/ili7835_fhdp_dsi_cmd_edo_edo_x676b/ in the project.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>

#include <drm/drm_connector.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

#include <video/mipi_display.h>

struct ili7835_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *bias_gpio;
	bool prepared;
};

static const struct drm_display_mode ili7835_mode = {
	.clock = 167670,
	.hdisplay = 1080,
	.hsync_start = 1120,
	.hsync_end = 1130,
	.htotal = 1150,
	.vdisplay = 2400,
	.vsync_start = 2420,
	.vsync_end = 2422,
	.vtotal = 2430,
};

static inline struct ili7835_panel *to_ili7835_panel(struct drm_panel *panel)
{
	return container_of(panel, struct ili7835_panel, panel);
}

static void ili7835_reset(struct ili7835_panel *ctx)
{
	/* Vendor: reset=0, 80us, bias=1, 160us, reset=1, 160us,
	 * reset=0, 64us, reset=1, 15ms.
	 */
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	udelay(80);
	gpiod_set_value_cansleep(ctx->bias_gpio, 1);
	udelay(160);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	udelay(160);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	udelay(64);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	msleep(15);
}

static void ili7835_init_sequence(struct mipi_dsi_multi_context *ctx)
{
	/* manufacturer page 0x06 */
	mipi_dsi_generic_write_seq_multi(ctx, 0xff, 0x78, 0x35, 0x06);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x45, 0x4c);
	mipi_dsi_generic_write_seq_multi(ctx, 0xc7, 0x00);

	/* manufacturer page 0x02 */
	mipi_dsi_generic_write_seq_multi(ctx, 0xff, 0x78, 0x35, 0x02);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x46, 0x19);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x47, 0x1f);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x48, 0x2b);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x51, 0x20);
	mipi_dsi_generic_write_seq_multi(ctx, 0xb4, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x02, 0xf8);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x05, 0x85);

	/* manufacturer page 0x09 */
	mipi_dsi_generic_write_seq_multi(ctx, 0xff, 0x78, 0x35, 0x09);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x91, 0x51);

	/* manufacturer page 0x05 */
	mipi_dsi_generic_write_seq_multi(ctx, 0xff, 0x78, 0x35, 0x05);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x4d, 0x50);

	/* manufacturer page 0x02 */
	mipi_dsi_generic_write_seq_multi(ctx, 0xff, 0x78, 0x35, 0x02);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x0a, 0x22);

	/* manufacturer page 0x01 */
	mipi_dsi_generic_write_seq_multi(ctx, 0xff, 0x78, 0x35, 0x01);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x0b, 0x20);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x42, 0x41);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x4b, 0x41);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x54, 0x43);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x5d, 0x43);
	mipi_dsi_generic_write_seq_multi(ctx, 0xcb, 0x20);
	mipi_dsi_generic_write_seq_multi(ctx, 0xd0, 0x20);

	/* manufacturer page 0x00 */
	mipi_dsi_generic_write_seq_multi(ctx, 0xff, 0x78, 0x35, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x53, 0x2c);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x35, 0x00);	/* TE on, V-blank only */
	mipi_dsi_dcs_write_seq_multi(ctx, 0x51, 0x00, 0x00);

	mipi_dsi_dcs_write_seq_multi(ctx, MIPI_DCS_EXIT_SLEEP_MODE);
	mipi_dsi_msleep(ctx, 120);

	mipi_dsi_generic_write_seq_multi(ctx, 0xff, 0x78, 0x35, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, MIPI_DCS_SET_DISPLAY_ON);
	mipi_dsi_msleep(ctx, 20);
}

static int ili7835_prepare(struct drm_panel *panel)
{
	struct ili7835_panel *ctx = to_ili7835_panel(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	ili7835_reset(ctx);

	ili7835_init_sequence(&dsi_ctx);
	if (dsi_ctx.accum_err)
		return dsi_ctx.accum_err;

	ctx->prepared = true;

	return 0;
}

static int ili7835_disable(struct drm_panel *panel)
{
	struct ili7835_panel *ctx = to_ili7835_panel(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 10);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	return dsi_ctx.accum_err;
}

static int ili7835_unprepare(struct drm_panel *panel)
{
	struct ili7835_panel *ctx = to_ili7835_panel(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	/* The panel is already off/sleeping here; bias off and park reset. */
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xff, 0x78, 0x35, 0x06);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xc5, 0x55);
	mipi_dsi_msleep(&dsi_ctx, 50);

	gpiod_set_value_cansleep(ctx->bias_gpio, 0);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);

	ctx->prepared = false;

	return 0;
}

static int ili7835_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
	connector->display_info.width_mm = 70;
	connector->display_info.height_mm = 155;

	return drm_connector_helper_get_modes_fixed(connector, &ili7835_mode);
}

static const struct drm_panel_funcs ili7835_panel_funcs = {
	.prepare = ili7835_prepare,
	.disable = ili7835_disable,
	.unprepare = ili7835_unprepare,
	.get_modes = ili7835_get_modes,
};

static int ili7835_bl_update_status(struct backlight_device *bl)
{
	struct ili7835_panel *ctx = bl_get_data(bl);
	u16 brightness = backlight_get_brightness(bl);

	if (!ctx->prepared)
		return 0;

	return mipi_dsi_dcs_set_display_brightness_large(ctx->dsi, brightness);
}

static const struct backlight_ops ili7835_bl_ops = {
	.update_status = ili7835_bl_update_status,
};

static struct backlight_device *ili7835_create_backlight(struct ili7835_panel *ctx)
{
	struct device *dev = &ctx->dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 512,
		.max_brightness = 1023,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, ctx,
					      &ili7835_bl_ops, &props);
}

static int ili7835_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct ili7835_panel *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct ili7835_panel, panel,
				   &ili7835_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->bias_gpio = devm_gpiod_get(dev, "bias", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->bias_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->bias_gpio),
				     "Failed to get bias-gpios\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_NO_EOT_PACKET |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS |
			  MIPI_DSI_MODE_LPM;

	ctx->panel.prepare_prev_first = true;

	ctx->panel.backlight = ili7835_create_backlight(ctx);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "Failed to create backlight\n");

	drm_panel_add(&ctx->panel);

	ret = devm_mipi_dsi_attach(dev, dsi);
	if (ret) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "Failed to attach to DSI host\n");
	}

	return 0;
}

static const struct of_device_id ili7835_of_match[] = {
	{ .compatible = "ilitek,ili7835" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ili7835_of_match);

static struct mipi_dsi_driver ili7835_driver = {
	.probe = ili7835_probe,
	.driver = {
		.name = "panel-ilitek-ili7835",
		.of_match_table = ili7835_of_match,
	},
};
module_mipi_dsi_driver(ili7835_driver);

MODULE_AUTHOR("mnnoee");
MODULE_DESCRIPTION("Ilitek ILI7835 AMOLED DSI panel (Infinix X676B)");
MODULE_LICENSE("GPL");
