/* SPDX-License-Identifier: GPL-2.0-only */
/* G04 panel backlight, using the PWM_D route configured by its bootloader. */
#include <linux/backlight.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define G04_PWM_D_DUTY		0
#define G04_PWM_CD_CTRL		4
#define G04_PWM_D_ENABLE	BIT(1)
#define G04_PWM_D_CLK_ENABLE	BIT(23)
#define G04_BL_MAX		255
#define G04_BL_DEFAULT		128
#define G04_BL_FIRMWARE_PERIOD	400

struct g04_backlight {
	void __iomem *pwm;
	unsigned int period;
};

static int g04_bl_update(struct backlight_device *bd)
{
	struct g04_backlight *bl = bl_get_data(bd);
	unsigned int level = bd->props.brightness;
	unsigned int high;
	u32 ctrl;

	if (bd->props.power != FB_BLANK_UNBLANK ||
	    bd->props.state & (BL_CORE_SUSPENDED | BL_CORE_FBBLANK))
		level = 0;

	ctrl = readl(bl->pwm + G04_PWM_CD_CTRL);
	if (!level) {
		writel(ctrl & ~G04_PWM_D_ENABLE, bl->pwm + G04_PWM_CD_CTRL);
		return 0;
	}

	high = DIV_ROUND_UP(bl->period * level, G04_BL_MAX);
	writel((high << 16) | (bl->period - high), bl->pwm + G04_PWM_D_DUTY);
	writel(ctrl | G04_PWM_D_CLK_ENABLE | G04_PWM_D_ENABLE,
	       bl->pwm + G04_PWM_CD_CTRL);
	return 0;
}

static const struct backlight_ops g04_bl_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = g04_bl_update,
};

static int g04_bl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct g04_backlight *bl;
	struct backlight_device *bd;
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.max_brightness = G04_BL_MAX,
		.brightness = G04_BL_DEFAULT,
		.power = FB_BLANK_UNBLANK,
	};
	u32 duty;

	bl = devm_kzalloc(dev, sizeof(*bl), GFP_KERNEL);
	if (!bl)
		return -ENOMEM;
	bl->pwm = devm_ioremap_resource(dev,
			platform_get_resource(pdev, IORESOURCE_MEM, 0));
	if (IS_ERR(bl->pwm))
		return PTR_ERR(bl->pwm);

	/* Capture the firmware period and leave the clock mux/divider alone. */
	duty = readl(bl->pwm + G04_PWM_D_DUTY);
	bl->period = (duty >> 16) + (duty & 0xffff);
	if (!bl->period)
		bl->period = G04_BL_FIRMWARE_PERIOD;
	if (bl->period > 0xffff)
		return -EINVAL;

	bd = devm_backlight_device_register(dev, "g04-backlight", dev, bl,
					   &g04_bl_ops, &props);
	if (IS_ERR(bd))
		return PTR_ERR(bd);
	backlight_update_status(bd);
	return 0;
}

static const struct of_device_id g04_bl_of_match[] = {
	{ .compatible = "ddev-io,meson6-g04-backlight" },
	{ }
};
MODULE_DEVICE_TABLE(of, g04_bl_of_match);

static struct platform_driver g04_bl_driver = {
	.probe = g04_bl_probe,
	.driver = {
		.name = "meson6-g04-backlight",
		.of_match_table = g04_bl_of_match,
	},
};
module_platform_driver(g04_bl_driver);
MODULE_LICENSE("GPL");
