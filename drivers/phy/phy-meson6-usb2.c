/*
 * Amlogic Meson6 USB2 PHY support
 *
 * The register sequence and clock gates are derived from the GPL Meson6
 * vendor usbclock.c implementation.  Although only port A is exposed to the
 * DWC2 device controller, the shared vendor sequence configures both PHYs.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>

#define MESON6_USB_CONFIG		0x00
#define MESON6_USB_CTRL			0x04
#define MESON6_USB_PHY_A_OFFSET		0x00
#define MESON6_USB_PHY_B_OFFSET		0x20

#define MESON6_USB_CONFIG_CLK_EN	BIT(0)
#define MESON6_USB_CONFIG_CLK_SEL_MASK	GENMASK(3, 1)
#define MESON6_USB_CONFIG_CLK_DIV_MASK	GENMASK(10, 4)
#define MESON6_USB_CONFIG_CLK_DIV_1	(1 << 4)

#define MESON6_USB_CTRL_CLK_DETECTED	BIT(8)
#define MESON6_USB_CTRL_POR		BIT(15)
#define MESON6_USB_CTRL_FSEL_MASK	GENMASK(22, 20)
#define MESON6_USB_CTRL_FSEL_12MHZ	(2 << 20)

#define MESON6_RESET1_USB		BIT(2)
#define MESON6_GCLK_MPEG1_USB0		BIT(21)
#define MESON6_GCLK_MPEG2_USB0_DDR	BIT(9)

struct meson6_usb2_phy {
	struct device *dev;
	void __iomem *regs;
	void __iomem *reset1;
	void __iomem *gclk1;
	void __iomem *gclk2;
	bool initialized;
};

static void meson6_usb2_update_bits(void __iomem *reg, u32 mask, u32 value)
{
	u32 val = readl(reg);

	val &= ~mask;
	val |= value & mask;
	writel(val, reg);
}

static int meson6_usb2_phy_init(struct phy *phy)
{
	struct meson6_usb2_phy *priv = phy_get_drvdata(phy);
	void __iomem *phy_a = priv->regs + MESON6_USB_PHY_A_OFFSET;
	void __iomem *phy_b = priv->regs + MESON6_USB_PHY_B_OFFSET;
	u32 config_a, config_b, ctrl_a, ctrl_b;

	if (priv->initialized)
		return 0;

	/* Enable the DWC2 and its DDR bridge gates before touching the core. */
	meson6_usb2_update_bits(priv->gclk1, MESON6_GCLK_MPEG1_USB0,
				MESON6_GCLK_MPEG1_USB0);
	meson6_usb2_update_bits(priv->gclk2, MESON6_GCLK_MPEG2_USB0_DDR,
				MESON6_GCLK_MPEG2_USB0_DDR);

	/* Vendor Meson6 code asserts RESET1[2] once and waits 500 ms. */
	meson6_usb2_update_bits(priv->reset1, MESON6_RESET1_USB,
				MESON6_RESET1_USB);
	msleep(500);

	/* XTAL source, divide by one, both PHY clocks enabled. */
	meson6_usb2_update_bits(phy_a + MESON6_USB_CONFIG,
		MESON6_USB_CONFIG_CLK_EN | MESON6_USB_CONFIG_CLK_SEL_MASK |
		MESON6_USB_CONFIG_CLK_DIV_MASK,
		MESON6_USB_CONFIG_CLK_EN | MESON6_USB_CONFIG_CLK_DIV_1);
	meson6_usb2_update_bits(phy_b + MESON6_USB_CONFIG,
		MESON6_USB_CONFIG_CLK_EN | MESON6_USB_CONFIG_CLK_SEL_MASK |
		MESON6_USB_CONFIG_CLK_DIV_MASK,
		MESON6_USB_CONFIG_CLK_EN | MESON6_USB_CONFIG_CLK_DIV_1);

	/*
	 * Match the vendor sequence exactly: select the 12 MHz reference and
	 * assert POR on both PHYs, then release only the device PHY (A).  PHY-B
	 * remains clocked but held in POR in the working 3.0.101 kernel.
	 */
	meson6_usb2_update_bits(phy_b + MESON6_USB_CTRL,
		MESON6_USB_CTRL_FSEL_MASK | MESON6_USB_CTRL_POR,
		MESON6_USB_CTRL_FSEL_12MHZ | MESON6_USB_CTRL_POR);
	meson6_usb2_update_bits(phy_a + MESON6_USB_CTRL,
		MESON6_USB_CTRL_FSEL_MASK | MESON6_USB_CTRL_POR,
		MESON6_USB_CTRL_FSEL_12MHZ | MESON6_USB_CTRL_POR);
	udelay(500);
	meson6_usb2_update_bits(phy_a + MESON6_USB_CTRL,
		MESON6_USB_CTRL_POR, 0);
	udelay(500);

	config_a = readl(phy_a + MESON6_USB_CONFIG);
	ctrl_a = readl(phy_a + MESON6_USB_CTRL);
	config_b = readl(phy_b + MESON6_USB_CONFIG);
	ctrl_b = readl(phy_b + MESON6_USB_CTRL);
	if (!(ctrl_a & MESON6_USB_CTRL_CLK_DETECTED))
		dev_warn(priv->dev, "USB-A PHY clock was not detected\n");
	if (!(ctrl_b & MESON6_USB_CTRL_CLK_DETECTED))
		dev_warn(priv->dev, "USB-B PHY clock was not detected\n");
	dev_info(priv->dev,
		 "G326 PHY A config=%08x ctrl=%08x B config=%08x ctrl=%08x\n",
		 config_a, ctrl_a, config_b, ctrl_b);

	priv->initialized = true;
	return 0;
}

static int meson6_usb2_phy_exit(struct phy *phy)
{
	struct meson6_usb2_phy *priv = phy_get_drvdata(phy);

	/*
	 * The vendor driver leaves both PHY clocks and the shared USB gates on.
	 * DWC2 calls phy_exit() once after endpoint creation and phy_init() again
	 * when Android gadget binds; tearing the hardware down here makes that
	 * transition differ from the working 3.0.101 lifetime.
	 */
	if (priv->initialized)
		dev_dbg(priv->dev, "G326 keeping Meson6 USB PHY initialized\n");

	return 0;
}

static int meson6_usb2_phy_power_on(struct phy *phy)
{
	return 0;
}

static int meson6_usb2_phy_power_off(struct phy *phy)
{
	return 0;
}

static const struct phy_ops meson6_usb2_phy_ops = {
	.init = meson6_usb2_phy_init,
	.exit = meson6_usb2_phy_exit,
	.power_on = meson6_usb2_phy_power_on,
	.power_off = meson6_usb2_phy_power_off,
	.owner = THIS_MODULE,
};

static int meson6_usb2_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct meson6_usb2_phy *priv;
	struct phy_provider *provider;
	struct resource *res;
	struct phy *phy;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->dev = dev;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "phy");
	priv->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(priv->regs))
		return PTR_ERR(priv->regs);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "reset");
	priv->reset1 = devm_ioremap_resource(dev, res);
	if (IS_ERR(priv->reset1))
		return PTR_ERR(priv->reset1);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "gclk1");
	priv->gclk1 = devm_ioremap_resource(dev, res);
	if (IS_ERR(priv->gclk1))
		return PTR_ERR(priv->gclk1);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "gclk2");
	priv->gclk2 = devm_ioremap_resource(dev, res);
	if (IS_ERR(priv->gclk2))
		return PTR_ERR(priv->gclk2);

	phy = devm_phy_create(dev, NULL, &meson6_usb2_phy_ops);
	if (IS_ERR(phy))
		return PTR_ERR(phy);

	phy_set_bus_width(phy, 8);
	phy_set_drvdata(phy, priv);
	platform_set_drvdata(pdev, phy);

	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	return PTR_ERR_OR_ZERO(provider);
}

static const struct of_device_id meson6_usb2_phy_of_match[] = {
	{ .compatible = "amlogic,meson6-usb2-phy" },
	{ },
};
MODULE_DEVICE_TABLE(of, meson6_usb2_phy_of_match);

static struct platform_driver meson6_usb2_phy_driver = {
	.probe = meson6_usb2_phy_probe,
	.driver = {
		.name = "meson6-usb2-phy",
		.of_match_table = meson6_usb2_phy_of_match,
	},
};
module_platform_driver(meson6_usb2_phy_driver);

MODULE_DESCRIPTION("Amlogic Meson6 USB2 PHY driver");
MODULE_AUTHOR("ddev-io");
MODULE_LICENSE("GPL v2");
