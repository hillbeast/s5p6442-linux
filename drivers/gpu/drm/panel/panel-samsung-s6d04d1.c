// SPDX-License-Identifier: GPL-2.0
/*
 * Panel driver for the Samsung S6D04D1 240x400 DPI RGB panel.
 * Found in the Samsung Galaxy Apollo GT-I5800 mobile phone.
 */

#include <drm/drm_mipi_dbi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/media-bus-format.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>

#include <video/mipi_display.h>

#define S6D04D1_RDDIDIF		0x04	/* Read Display ID */
#define S6D04D1_WRDISBV		0x51	/* Write Manual Brightness */
#define S6D04D1_READID1		0xDA	/* Read panel ID 1 */
#define S6D04D1_READID2		0xDB	/* Read panel ID 2 */
#define S6D04D1_READID3		0xDC	/* Read panel ID 3 */
#define S6D04D1_PASSWD_L2	0xF0	/* Password Command for Level 2 Control */
#define S6D04D1_DISPCTL		0xF2	/* Display Control */
#define S6D04D1_MANPWR		0xF3	/* Manual Control */
#define S6D04D1_PWRCTL1		0xF4	/* Power Control */
#define S6D04D1_SRCCTL		0xF6	/* Source Control */
#define S6D04D1_PANELCTL	0xF7	/* Panel Control*/

#define MAX_BRIGHTNESS_LEVEL 0xFF
#define LOW_BRIGHTNESS_LEVEL 0x1E

#define MAX_BACKLIGHT_VALUE_SMD 0x9B
#define LOW_BACKLIGHT_VALUE_SMD 0x1F
#define DIM_BACKLIGHT_VALUE_SMD 0x12

#define MAX_BACKLIGHT_VALUE_SONY 0x9B //0xB4
#define LOW_BACKLIGHT_VALUE_SONY 0x1F
#define DIM_BACKLIGHT_VALUE_SONY 0x12

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

static struct setting_table backlight_setting_table[] = {
	{ S6D04D1_WRDISBV,  1, { 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },   0 }
};


static int s6d04d1_write_setting_table(struct s6d04d1 *ctx, 
				       struct setting_table *table, 
				       size_t count)
{
	struct mipi_dbi *dbi = &ctx->dbi;
	size_t i;
	int ret;

	for (i = 0; i < count; i++) {
		/* Force execution through standard MIPI DBI buffers */
		ret = mipi_dbi_command_buf(dbi, table[i].command, 
					   table[i].parameter, 
					   table[i].parameters);
		if (ret) {
			dev_err(ctx->dev, "Failed to write cmd 0x%02x: %d\n", 
				table[i].command, ret);
			return ret;
		}

		/* Apply the specific hardware pause instruction if specified */
		if (table[i].wait > 0)
			msleep(table[i].wait);
	}

	return 0;
}

static int s6d04d1_get_tune(int level)
{
	int tune_value;

	// SMD LCD
	if(level > MAX_BRIGHTNESS_LEVEL)
		level = MAX_BRIGHTNESS_LEVEL;

	if(level >= LOW_BRIGHTNESS_LEVEL)
		tune_value = (level - LOW_BRIGHTNESS_LEVEL) * (MAX_BACKLIGHT_VALUE_SMD-LOW_BACKLIGHT_VALUE_SMD) / (MAX_BRIGHTNESS_LEVEL-LOW_BRIGHTNESS_LEVEL) + LOW_BACKLIGHT_VALUE_SMD;
	else if(level > 0)
		tune_value = DIM_BACKLIGHT_VALUE_SMD;
	else
		tune_value = level;
	
	if(tune_value > MAX_BACKLIGHT_VALUE_SMD)
		tune_value = MAX_BACKLIGHT_VALUE_SMD;			// led_val must be less than or equal to MAX_BACKLIGHT_VALUE

	if(level && !tune_value)
		tune_value = 1;

	return tune_value;
}

static int s6d04d1_set_brightness(struct s6d04d1 *ctx, int level)
{
	unsigned int led_val;

	led_val = s6d04d1_get_tune(level);

	// backlight_setting_table.parameter[0] = led_val;

	printk("%s brightness:0x%x\n", __func__, backlight_setting_table[0].parameter[0]);
	s6d04d1_write_setting_table(ctx, backlight_setting_table, ARRAY_SIZE(backlight_setting_table));

	return 0;
}

static void s6d04d1_read_mtp_id(struct s6d04d1 *ctx)
{
	struct mipi_dbi *dbi = &ctx->dbi;
	struct spi_device *spi = dbi->spi;
	struct spi_message msg;
	struct spi_transfer xfers[2];
	u8 cmd = 0x04;
	u8 id_buf[4] = {0};
	int ret;

	if (!spi)
		return;

	spi_message_init(&msg);
	memset(xfers, 0, sizeof(xfers));

	xfers[0].tx_buf = &cmd;
	xfers[0].len = 1;
	
	xfers[0].delay.value = 10;
	xfers[0].delay.unit = SPI_DELAY_UNIT_USECS;
	spi_message_add_tail(&xfers[0], &msg);

	xfers[1].rx_buf = id_buf;
	xfers[1].len = 4;
	spi_message_add_tail(&xfers[1], &msg);

	ret = spi_sync(spi, &msg);
	if (ret) {
		dev_err(ctx->dev, "SPI sync multi-phase read failed: %d\n", ret);
		return;
	}

	pr_info("%s: NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE\n", __func__);
	pr_info("%s:   SPI DOES NOT APPEAR TO BE BIDIRECTIONAL AND IS RETURNING 0x00\n", __func__);
	pr_info("%s: NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE NOTE\n", __func__);
	dev_info(ctx->dev, "RDDIDIF: %02x %02x %02x %02x\n",
		 id_buf[0], id_buf[1], id_buf[2], id_buf[3]);

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
	gpiod_set_value_cansleep(ctx->reset, 1);
	usleep_range(1000, 5000);
	gpiod_set_value_cansleep(ctx->reset, 0);
	msleep(20);

	/* Magic startup code from 2.6.32 Kernel Driver */
	mipi_dbi_command(dbi, 0xF3, 0x80, 0x00, 0x00, 0x0B, 0x33, 0x7F, 0x7F); 													// POWCTL
	mipi_dbi_command(dbi, 0xF4, 0x6E, 0x6E, 0x7F, 0x7F, 0x33); 																// VCMCTL
	mipi_dbi_command(dbi, 0xF5, 0x12, 0x00, 0x03, 0xF0, 0x70); 																// SRCCTL
	mipi_dbi_command(dbi, 0x11);  																							// SLPOUT
	msleep(120);

	mipi_dbi_command(dbi, 0x36, 0x88); 	 																					// MADCTL
	mipi_dbi_command(dbi, 0x3A, 0x77); 	 																					// COLMOD
	msleep(30);

	mipi_dbi_command(dbi, 0xF2, 0x14, 0x14, 0x03, 0x03, 0x04, 0x03, 0x04, 0x10, 0x04, 0x14, 0x14); 	 						// DISCTL
	mipi_dbi_command(dbi, 0xF6, 0x00, 0x81, 0x30, 0x10); 	 																// IFCTL
	mipi_dbi_command(dbi, 0xFD, 0x22, 0x01); 	 																			// GATECTL
	mipi_dbi_command(dbi, 0x51, 0x00);  //BRIGHTNESS	 																	// WRDISBV
	mipi_dbi_command(dbi, 0x5E, 0x00); 	 																					// WRCABCMB
	mipi_dbi_command(dbi, 0xCA, 0x80, 0x80, 0x20); 	 																		// MIECTL1
	mipi_dbi_command(dbi, 0xCB, 0x03); 	 																					// SRBCMODECCTL
	mipi_dbi_command(dbi, 0xCC, 0x20, 0x01, 0x8F);  																		// MIECTL2
	mipi_dbi_command(dbi, 0xCD, 0x7C, 0x01); 	 																			// MIECTL3
	mipi_dbi_command(dbi, 0xF7, 0x00, 0x23, 0x15, 0x15, 0x1C, 0x1D, 0x1D, 0x21, 0x22, 0x28, 0x2C, 0x2C, 0x2C, 0x22, 0x21);	// RPGAMCTL 
	mipi_dbi_command(dbi, 0xF8, 0x19, 0x00, 0x15, 0x15, 0x1C, 0x1D, 0x1D, 0x21, 0x22, 0x28, 0x2C, 0x2C, 0x2C, 0x22, 0x21); 	// RNGAMCTL 
	mipi_dbi_command(dbi, 0xF9, 0x00, 0x23, 0x15, 0x15, 0x1C, 0x1C, 0x1B, 0x1F, 0x24, 0x28, 0x2C, 0x2C, 0x2C, 0x22, 0x21); 	// GPGAMCTL 
	mipi_dbi_command(dbi, 0xFA, 0x19, 0x00, 0x15, 0x15, 0x1C, 0x1C, 0x1B, 0x1F, 0x24, 0x28, 0x2C, 0x2C, 0x2C, 0x22, 0x21); 	// GNGAMCTL 
	mipi_dbi_command(dbi, 0xFB, 0x00, 0x23, 0x15, 0x15, 0x1A, 0x18, 0x15, 0x17, 0x2E, 0x37, 0x3F, 0x3F, 0x3F, 0x22, 0x21); 	// BPGAMCTL 
	mipi_dbi_command(dbi, 0xFC, 0x19, 0x00, 0x15, 0x15, 0x1A, 0x18, 0x15, 0x17, 0x2E, 0x37, 0x3F, 0x3F, 0x3F, 0x22, 0x21); 	// BNGAMCTL 
	mipi_dbi_command(dbi, 0x2A, 0x00, 0x00, 0x00, 0xEF);  	 																// CASET
	mipi_dbi_command(dbi, 0x2B, 0x00, 0x00, 0x01, 0x8F);  	 																// PASET
	mipi_dbi_command(dbi, 0x2C);  	 																						// RAMWR
	mipi_dbi_command(dbi, 0x53, 0x2C);  	 																				// WRCTRLD
	mipi_dbi_command(dbi, 0x55, 0x00); 	 	 																				// WRCABC
	mipi_dbi_command(dbi, 0x29);  	 																						// DISPON
	msleep(120);

	/* lock the level 2 control */
	mipi_dbi_command(dbi, S6D04D1_PASSWD_L2, 0xA5, 0xA5);

	return 0;
}

static int s6d04d1_power_off(struct s6d04d1 *ctx)
{
	/* Go into RESET and disable regulators */
	gpiod_set_value_cansleep(ctx->reset, 1);
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

	s6d04d1_read_mtp_id(ctx);

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

	ctx->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset)) {
		ret = PTR_ERR(ctx->reset);
		return dev_err_probe(dev, ret, "no RESET GPIO\n");
	}

	spi->mode |= SPI_3WIRE;

	ret = spi_setup(spi);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to apply 3-wire SPI configuration\n");

	ret = mipi_dbi_spi_init(spi, &ctx->dbi, NULL);
	if (ret)
		return dev_err_probe(dev, ret, "MIPI DBI init failed\n");

	ctx->dbi.read_commands = s6d04d1_dbi_read_commands;

	drm_panel_init(&ctx->panel, dev, &s6d04d1_drm_funcs,
		       DRM_MODE_CONNECTOR_DPI);

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add backlight\n");

	spi_set_drvdata(spi, ctx);
	drm_panel_add(&ctx->panel);

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
