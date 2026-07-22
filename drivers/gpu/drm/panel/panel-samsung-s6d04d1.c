// SPDX-License-Identifier: GPL-2.0
/*
 * Panel driver for the Samsung S6D04D1 240x400 DPI RGB panel.
 * Found in the Samsung Galaxy Apollo GT-I5800 mobile phone.
 */

#include <drm/drm_mipi_dbi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/media-bus-format.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>

#include <linux/debugfs.h>

#include <video/mipi_display.h>

#define S6D04D1_RDDIDIF			0x04	/* Read Display ID */
#define S6D04D1_SLPOUT			0x11
#define S6D04D1_DISPON			0x29
#define S6D04D1_CASET			0x2A
#define S6D04D1_PASET			0x2B
#define S6D04D1_RAMWR			0x2C
#define S6D04D1_MADCTL			0x36
#define S6D04D1_COLMOD			0x3A
#define S6D04D1_WRDISBV			0x51	/* Write Manual Brightness */
#define S6D04D1_WRCTRLD			0x53
#define S6D04D1_WRCABC			0x55
#define S6D04D1_READID1			0xDA	/* Read panel ID 1 */
#define S6D04D1_READID2			0xDB	/* Read panel ID 2 */
#define S6D04D1_READID3			0xDC	/* Read panel ID 3 */
#define S6D04D1_WRCABCMB		0x5E
#define S6D04D1_MIECTL1			0xCA
#define S6D04D1_SRBCMODECCTL	0xCB
#define S6D04D1_MIECTL2			0xCC
#define S6D04D1_MIECTL3			0xCD
#define S6D04D1_PASSWD_L2		0xF0	/* Password Command for Level 2 Control */
#define S6D04D1_DISPCTL			0xF2	/* Display Control */
#define S6D04D1_MANPWR			0xF3	/* Manual Control */
#define S6D04D1_PWRCTL1			0xF4	/* Power Control */
#define S6D04D1_SRCCTL			0xF5	/* Source Control */
#define S6D04D1_IFCTL			0xF6
#define S6D04D1_PANELCTL		0xF7	/* Panel Control*/
#define S6D04D1_RNGAMCTL		0xF8
#define S6D04D1_GPGAMCTL		0xF9
#define S6D04D1_GNGAMCTL		0xFA
#define S6D04D1_BPGAMCTL		0xFB
#define S6D04D1_BNGAMCTL		0xFC
#define S6D04D1_GATECTL			0xFD

static const u8 s6d04d1_dbi_read_commands[] = {
	S6D04D1_RDDIDIF,
	S6D04D1_READID1,
	S6D04D1_READID2,
	S6D04D1_READID3,
	0, /* sentinel */
};

struct s6d04d1 {
	struct device *dev;
	struct mipi_dbi dbi;
	struct drm_panel panel;
	struct gpio_desc *reset;
	struct regulator_bulk_data regulators[2];
	struct backlight_device *bl_dev;
	bool prepared;
};

static const struct drm_display_mode s6d04d1_video_mode = {
	.clock = 6800,
	.hdisplay = 240,
	.hsync_start = 240 + 10,
	.hsync_end = 240 + 10 + 10,
	.htotal = 240 + 10 + 10 + 24,
	.vdisplay = 400,
	.vsync_start = 400 + 4,
	.vsync_end = 400 + 4 + 2,
	.vtotal = 400 + 4 + 2 + 12,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
	.width_mm = 39, 				
	.height_mm = 65, 				
};

static inline struct s6d04d1 *to_s6d04d1(struct drm_panel *panel)
{
	return container_of(panel, struct s6d04d1, panel);
}

struct setting_table {
	u8 command;	
	u8 parameters;
	u8 parameter[15];
	s32 wait;
};

struct backlight_properties backlight_props = {
	.type = BACKLIGHT_RAW,
	.brightness = 255,
	.max_brightness = 255,
};

#define S6D04D1_MAX_BRIGHTNESS_LEVEL   0xFF
#define S6D04D1_LOW_BRIGHTNESS_LEVEL   0x1E

#define S6D04D1_MAX_BACKLIGHT_VALUE_SMD 0x9B
#define S6D04D1_LOW_BACKLIGHT_VALUE_SMD 0x1F
#define S6D04D1_DIM_BACKLIGHT_VALUE_SMD 0x12

static u8 s6d04d1_get_tune(int level)
{
	int tune_value;

	if (level > S6D04D1_MAX_BRIGHTNESS_LEVEL)
		level = S6D04D1_MAX_BRIGHTNESS_LEVEL;

	if (level >= S6D04D1_LOW_BRIGHTNESS_LEVEL)
		tune_value = (level - S6D04D1_LOW_BRIGHTNESS_LEVEL) *
			     (S6D04D1_MAX_BACKLIGHT_VALUE_SMD - S6D04D1_LOW_BACKLIGHT_VALUE_SMD) /
			     (S6D04D1_MAX_BRIGHTNESS_LEVEL - S6D04D1_LOW_BRIGHTNESS_LEVEL) +
			     S6D04D1_LOW_BACKLIGHT_VALUE_SMD;
	else if (level > 0)
		tune_value = S6D04D1_DIM_BACKLIGHT_VALUE_SMD;
	else
		tune_value = level; /* i.e. 0 */

	if (tune_value > S6D04D1_MAX_BACKLIGHT_VALUE_SMD)
		tune_value = S6D04D1_MAX_BACKLIGHT_VALUE_SMD;

	if (level && !tune_value)
		tune_value = 1;

	return (u8)tune_value;
}

static int s6d04d1_set_backlight(struct backlight_device *bd)
{
	struct s6d04d1 *ctx = bl_get_data(bd);
	struct mipi_dbi *dbi = &ctx->dbi;
	int brightness = backlight_get_brightness(bd);

	if (!ctx->prepared)
		return 0; /* nothing to do until the panel is powered/init'd */

	mipi_dbi_command(dbi, S6D04D1_WRDISBV, s6d04d1_get_tune(brightness)); /* WRDISBV */

	return 0;
}

static const struct backlight_ops s6d04d1_backlight_ops = {
	.update_status = s6d04d1_set_backlight,
};

static int s6d04d1_read_ldi_register(struct mipi_dbi *dbi, u8 cmd,
				      u8 *data, size_t len)
{
	struct spi_device *spi = dbi->spi;
	u32 speed_hz = min_t(u32, 200000, spi->max_speed_hz / 2);
	u16 *cmdbuf = dbi->tx_buf9;
	struct spi_transfer tr[2] = {
		{
			.speed_hz = speed_hz,
			.bits_per_word = 10,
			.tx_buf = cmdbuf,
			.len = 2,
		}, {
			.speed_hz = speed_hz,
			.bits_per_word = 8,
			.rx_buf = data,
			.len = len,
		},
	};
	struct spi_message m;

	if (!spi_is_bpw_supported(spi, 10))
		return -EOPNOTSUPP;

	cmdbuf[0] = (u16)cmd << 1;

	spi_message_init_with_transfers(&m, tr, ARRAY_SIZE(tr));
	return spi_sync(spi, &m);
}

static void s6d04d1_read_mtp_id(struct s6d04d1 *ctx)
{
	struct mipi_dbi *dbi = &ctx->dbi;
	u8 cmd = S6D04D1_RDDIDIF;
	u8 id_buf[3] = {0};
	int ret;

	ret = s6d04d1_read_ldi_register(dbi, cmd, id_buf, 3);
	if (ret) {
		dev_err(ctx->dev, "RDDIDIF read failed: %d\n", ret);
		return;
	}

	dev_info(ctx->dev, "RDDIDIF: %02x %02x %02x\n",
		 id_buf[0], id_buf[1], id_buf[2]);

}

static int s6d04d1_power_on(struct s6d04d1 *ctx)
{
	struct mipi_dbi *dbi = &ctx->dbi;
	int ret;

	/* Power up */
	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->regulators),
				    ctx->regulators);
	if (ret) {
		dev_err(ctx->dev, "failed to enable regulators: %d\n", ret);
		return ret;
	}
	msleep(20);

	/* Reset Display */
	gpiod_set_value_cansleep(ctx->reset, 0);
	msleep(10);
	gpiod_set_value_cansleep(ctx->reset, 1);
	msleep(10);
	gpiod_set_value_cansleep(ctx->reset, 0);
	msleep(20);

	/* Dummy command after reset to prime controller */
	u8 dummy[3];
	s6d04d1_read_ldi_register(dbi, S6D04D1_RDDIDIF, dummy, 3);
	
	s6d04d1_read_mtp_id(ctx);

	/* Magic startup code from 2.6.32 Kernel Driver */
	mipi_dbi_command(dbi, S6D04D1_MANPWR, 0x80, 0x00, 0x00, 0x0B, 0x33, 0x7F, 0x7F);
	mipi_dbi_command(dbi, S6D04D1_PWRCTL1, 0x6E, 0x6E, 0x7F, 0x7F, 0x33);
	mipi_dbi_command(dbi, S6D04D1_SRCCTL, 0x12, 0x00, 0x03, 0xF0, 0x70);
	mipi_dbi_command(dbi, S6D04D1_SLPOUT);
	msleep(120);

	mipi_dbi_command(dbi, S6D04D1_MADCTL, 0x98);
	mipi_dbi_command(dbi, S6D04D1_COLMOD, 0x77);
	msleep(30);

	mipi_dbi_command(dbi, S6D04D1_DISPCTL, 0x14, 0x14, 0x03, 0x03, 0x04, 0x03, 0x04, 0x10, 0x04, 0x14, 0x14);
	mipi_dbi_command(dbi, S6D04D1_IFCTL, 0x00, 0x81, 0x30, 0x10);
	mipi_dbi_command(dbi, S6D04D1_GATECTL, 0x22, 0x01);
	mipi_dbi_command(dbi, S6D04D1_WRDISBV, 0x00);
	mipi_dbi_command(dbi, S6D04D1_WRCABCMB, 0x00);
	mipi_dbi_command(dbi, S6D04D1_MIECTL1, 0x80, 0x80, 0x20);
	mipi_dbi_command(dbi, S6D04D1_SRBCMODECCTL, 0x03);
	mipi_dbi_command(dbi, S6D04D1_MIECTL2, 0x20, 0x01, 0x8F);
	mipi_dbi_command(dbi, S6D04D1_MIECTL3, 0x7C, 0x01);
	mipi_dbi_command(dbi, S6D04D1_PANELCTL, 0x00, 0x23, 0x15, 0x15, 0x1C, 0x19, 0x18, 0x1E, 0x24, 0x25, 0x25, 0x20, 0x10, 0x22, 0x21);
	mipi_dbi_command(dbi, S6D04D1_RNGAMCTL, 0x19, 0x00, 0x15, 0x15, 0x1C, 0x1F, 0x1E, 0x24, 0x1E, 0x1F, 0x25, 0x20, 0x10, 0x22, 0x21);
	mipi_dbi_command(dbi, S6D04D1_GPGAMCTL, 0x06, 0x23, 0x14, 0x14, 0x1D, 0x1A, 0x19, 0x1F, 0x24, 0x26, 0x30, 0x1E, 0x1E, 0x22, 0x21);
	mipi_dbi_command(dbi, S6D04D1_GNGAMCTL, 0x19, 0x06, 0x14, 0x14, 0x1D, 0x20, 0x1F, 0x25, 0x1E, 0x20, 0x30, 0x1E, 0x1E, 0x22, 0x21);
	mipi_dbi_command(dbi, S6D04D1_BPGAMCTL, 0x2C, 0x23, 0x20, 0x20, 0x23, 0x2F, 0x30, 0x39, 0x09, 0x09, 0x18, 0x13, 0x13, 0x22, 0x21);
	mipi_dbi_command(dbi, S6D04D1_BNGAMCTL, 0x19, 0x2C, 0x20, 0x20, 0x23, 0x35, 0x36, 0x3F, 0x03, 0x03, 0x18, 0x13, 0x13, 0x22, 0x21);
	mipi_dbi_command(dbi, S6D04D1_CASET, 0x00, 0x00, 0x00, 0xEF);
	mipi_dbi_command(dbi, S6D04D1_PASET, 0x00, 0x00, 0x01, 0x8F);
	mipi_dbi_command(dbi, S6D04D1_RAMWR);
	mipi_dbi_command(dbi, S6D04D1_WRCTRLD, 0x2C);
	mipi_dbi_command(dbi, S6D04D1_WRCABC, 0x00);
	mipi_dbi_command(dbi, S6D04D1_DISPON);
	msleep(120);

	/* lock the level 2 control */
	mipi_dbi_command(dbi, S6D04D1_PASSWD_L2, 0xA5, 0xA5);

	ctx->prepared = true;

	if (ctx->bl_dev)
		backlight_update_status(ctx->bl_dev);

	return 0;
}

static int s6d04d1_power_off(struct s6d04d1 *ctx)
{
	/* Go into RESET and disable regulators */
	gpiod_set_value_cansleep(ctx->reset, 1);
	ctx->prepared = false;

	return regulator_bulk_disable(ARRAY_SIZE(ctx->regulators),
				      ctx->regulators);
}

static int s6d04d1_unprepare(struct drm_panel *panel)
{
	struct s6d04d1 *ctx = to_s6d04d1(panel);
	struct mipi_dbi *dbi = &ctx->dbi;

	mipi_dbi_command(dbi, MIPI_DCS_ENTER_SLEEP_MODE);
	msleep(120);
	return s6d04d1_power_off(to_s6d04d1(panel));
}

static int s6d04d1_disable(struct drm_panel *panel)
{
	struct s6d04d1 *ctx = to_s6d04d1(panel);
	struct mipi_dbi *dbi = &ctx->dbi;

	mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_OFF);
	msleep(25);

	return 0;
}

static int s6d04d1_prepare(struct drm_panel *panel)
{
	return s6d04d1_power_on(to_s6d04d1(panel));
}

static int s6d04d1_enable(struct drm_panel *panel)
{
	struct s6d04d1 *ctx = to_s6d04d1(panel);
	struct mipi_dbi *dbi = &ctx->dbi;

	mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_ON);
	msleep(20);

	return 0;
}

static int s6d04d1_get_modes(struct drm_panel *panel,
			    struct drm_connector *connector)
{
	struct s6d04d1 *ctx = to_s6d04d1(panel);
	struct drm_display_mode *mode;
	static const u32 bus_format = MEDIA_BUS_FMT_RGB888_1X24;

	mode = drm_mode_duplicate(connector->dev, &s6d04d1_video_mode);
	if (!mode) {
		dev_err(ctx->dev, "failed to add mode\n");
		return -ENOMEM;
	}

	connector->display_info.bpc = 8;
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	connector->display_info.bus_flags =
		DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_DRIVE_NEGEDGE;
	drm_display_info_set_bus_formats(&connector->display_info,
					 &bus_format, 1);

	drm_mode_set_name(mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;

	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs s6d04d1_drm_funcs = {
	.disable = s6d04d1_disable,
	.unprepare = s6d04d1_unprepare,
	.prepare = s6d04d1_prepare,
	.enable = s6d04d1_enable,
	.get_modes = s6d04d1_get_modes,
};

static ssize_t s6d04d1_debug_power_on(struct file *f, const char __user *buf,
				       size_t count, loff_t *ppos)
{
	struct s6d04d1 *ctx = file_inode(f)->i_private;

	s6d04d1_power_on(ctx);
	return count;
}

static const struct file_operations s6d04d1_debug_power_on_fops = {
	.write = s6d04d1_debug_power_on,
	.open = simple_open,
	.llseek = default_llseek,
};

static ssize_t s6d04d1_debug_read_mtp(struct file *f, const char __user *buf,
				       size_t count, loff_t *ppos)
{
	struct s6d04d1 *ctx = file_inode(f)->i_private;

	s6d04d1_read_mtp_id(ctx);
	return count;
}

static const struct file_operations s6d04d1_debug_read_mtp_fops = {
	.write = s6d04d1_debug_read_mtp,
	.open = simple_open,
	.llseek = default_llseek,
};

static int s6d04d1_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct s6d04d1 *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dev = dev;

	ctx->regulators[0].supply = "vci";
	ctx->regulators[1].supply = "vdd3";
	ret = devm_regulator_bulk_get(dev,
				      ARRAY_SIZE(ctx->regulators),
				      ctx->regulators);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	ctx->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW); // was HIGH
	if (IS_ERR(ctx->reset)) {
		ret = PTR_ERR(ctx->reset);
		return dev_err_probe(dev, ret, "failed to claim reset GPIO\n");
	}

	ret = mipi_dbi_spi_init(spi, &ctx->dbi, NULL);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialise SPI\n");

	drm_panel_init(&ctx->panel, dev, &s6d04d1_drm_funcs,
		       DRM_MODE_CONNECTOR_DPI);

#if 0
	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add backlight\n");
#endif

	ctx->bl_dev = devm_backlight_device_register(dev, "s6d04d1-bl", dev,
			ctx, &s6d04d1_backlight_ops, &backlight_props);
	if (IS_ERR(ctx->bl_dev))
		return dev_err_probe(dev, PTR_ERR(ctx->bl_dev),
					"failed to register backlight\n");

	ctx->panel.backlight = ctx->bl_dev;

	spi_set_drvdata(spi, ctx);
	drm_panel_add(&ctx->panel);

	debugfs_create_file("trigger_power_on", 0200, NULL, ctx,
		     &s6d04d1_debug_power_on_fops);
	debugfs_create_file("trigger_read_mtp", 0200, NULL, ctx,
		     &s6d04d1_debug_read_mtp_fops);

	return 0;
}

static void s6d04d1_remove(struct spi_device *spi)
{
	struct s6d04d1 *ctx = spi_get_drvdata(spi);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id s6d04d1_match[] = {
	{ .compatible = "samsung,s6d04d1", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, s6d04d1_match);

static struct spi_driver s6d04d1_driver = {
	.probe		= s6d04d1_probe,
	.remove		= s6d04d1_remove,
	.driver		= {
		.name	= "s6d04d1-panel",
		.of_match_table = s6d04d1_match,
	},
};
module_spi_driver(s6d04d1_driver);

MODULE_AUTHOR("Mark Kennard <markkennard4@gmail.com>");
MODULE_DESCRIPTION("Samsung S6D04D1 panel driver");
MODULE_LICENSE("GPL v2");
