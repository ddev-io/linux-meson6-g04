/*
 * Minimal GPIO support for Amlogic Meson6.
 *
 * The Meson6 GPIO direction register uses active-low output enables:
 * a set bit selects input mode and a clear bit selects output mode.
 */

#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#define MESON6_GPIO_EN_N	0
#define MESON6_GPIO_OUT		1
#define MESON6_GPIO_IN		2
#define MESON6_GPIO_IRQ_ROUTER	3

#define MESON6_GPIO_IRQ_EDGE_POL	0x0
#define MESON6_GPIO_IRQ_SEL0		0x4
#define MESON6_GPIO_IRQ_SEL1		0x8
#define MESON6_GPIO_IRQ_FILTER		0xc

#define MESON6_GPIO_IRQ_HIGH		0
#define MESON6_GPIO_IRQ_LOW		1
#define MESON6_GPIO_IRQ_RISING		2
#define MESON6_GPIO_IRQ_FALLING		3

struct meson6_gpio {
	struct gpio_chip chip;
	void __iomem *en_n;
	void __iomem *out;
	void __iomem *in;
	void __iomem *irq_router;
	spinlock_t lock;
};

static inline struct meson6_gpio *to_meson6_gpio(struct gpio_chip *chip)
{
	return container_of(chip, struct meson6_gpio, chip);
}

static int meson6_gpio_get_direction(struct gpio_chip *chip,
				     unsigned int offset)
{
	struct meson6_gpio *gpio = to_meson6_gpio(chip);

	return !!(readl(gpio->en_n) & BIT(offset));
}

static int meson6_gpio_direction_input(struct gpio_chip *chip,
				       unsigned int offset)
{
	struct meson6_gpio *gpio = to_meson6_gpio(chip);
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&gpio->lock, flags);
	val = readl(gpio->en_n);
	writel(val | BIT(offset), gpio->en_n);
	spin_unlock_irqrestore(&gpio->lock, flags);

	return 0;
}

static void meson6_gpio_set(struct gpio_chip *chip, unsigned int offset,
			    int value)
{
	struct meson6_gpio *gpio = to_meson6_gpio(chip);
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&gpio->lock, flags);
	val = readl(gpio->out);
	if (value)
		val |= BIT(offset);
	else
		val &= ~BIT(offset);
	writel(val, gpio->out);
	spin_unlock_irqrestore(&gpio->lock, flags);
}

static int meson6_gpio_direction_output(struct gpio_chip *chip,
					unsigned int offset, int value)
{
	struct meson6_gpio *gpio = to_meson6_gpio(chip);
	unsigned long flags;
	u32 val;

	meson6_gpio_set(chip, offset, value);

	spin_lock_irqsave(&gpio->lock, flags);
	val = readl(gpio->en_n);
	writel(val & ~BIT(offset), gpio->en_n);
	spin_unlock_irqrestore(&gpio->lock, flags);

	return 0;
}

static int meson6_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	struct meson6_gpio *gpio = to_meson6_gpio(chip);

	return !!(readl(gpio->in) & BIT(offset));
}

static int meson6_gpio_map_resource(struct platform_device *pdev,
				    unsigned int index, void __iomem **base)
{
	struct resource *res;

	res = platform_get_resource(pdev, IORESOURCE_MEM, index);
	if (!res)
		return -EINVAL;

	*base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(*base))
		return PTR_ERR(*base);

	return 0;
}

static int meson6_gpio_setup_irq_router(struct platform_device *pdev,
					struct meson6_gpio *gpio)
{
	struct device_node *np = pdev->dev.of_node;
	u32 slot, selector, filter, trigger;
	u32 edge_pol, sel, val;
	unsigned int sel_offset, sel_shift;
	int ret;

	if (!of_find_property(np, "amlogic,irq-slot", NULL))
		return 0;

	ret = meson6_gpio_map_resource(pdev, MESON6_GPIO_IRQ_ROUTER,
					&gpio->irq_router);
	if (ret)
		return ret;

	if (of_property_read_u32(np, "amlogic,irq-slot", &slot) ||
	    of_property_read_u32(np, "amlogic,irq-selector", &selector) ||
	    of_property_read_u32(np, "amlogic,irq-filter", &filter) ||
	    of_property_read_u32(np, "amlogic,irq-trigger", &trigger))
		return -EINVAL;

	if (slot >= 8 || selector > 0xff || filter > 7 ||
	    trigger > MESON6_GPIO_IRQ_FALLING)
		return -EINVAL;

	edge_pol = readl(gpio->irq_router + MESON6_GPIO_IRQ_EDGE_POL);
	edge_pol &= ~(BIT(slot) | BIT(slot + 16));
	if (trigger == MESON6_GPIO_IRQ_RISING ||
	    trigger == MESON6_GPIO_IRQ_FALLING)
		edge_pol |= BIT(slot);
	if (trigger == MESON6_GPIO_IRQ_LOW ||
	    trigger == MESON6_GPIO_IRQ_FALLING)
		edge_pol |= BIT(slot + 16);
	writel(edge_pol, gpio->irq_router + MESON6_GPIO_IRQ_EDGE_POL);

	sel_offset = slot < 4 ? MESON6_GPIO_IRQ_SEL0 : MESON6_GPIO_IRQ_SEL1;
	sel_shift = (slot & 3) * 8;
	sel = readl(gpio->irq_router + sel_offset);
	sel &= ~(0xff << sel_shift);
	sel |= selector << sel_shift;
	writel(sel, gpio->irq_router + sel_offset);

	val = readl(gpio->irq_router + MESON6_GPIO_IRQ_FILTER);
	val &= ~(0x7 << (slot * 4));
	val |= filter << (slot * 4);
	writel(val, gpio->irq_router + MESON6_GPIO_IRQ_FILTER);

	dev_info(&pdev->dev,
		 "IRQ router slot %u: selector 0x%02x, trigger %u, filter %u\n",
		 slot, selector, trigger, filter);

	return 0;
}

static int meson6_gpio_probe(struct platform_device *pdev)
{
	struct meson6_gpio *gpio;
	u32 ngpios;
	int ret;

	gpio = devm_kzalloc(&pdev->dev, sizeof(*gpio), GFP_KERNEL);
	if (!gpio)
		return -ENOMEM;

	ret = meson6_gpio_map_resource(pdev, MESON6_GPIO_EN_N, &gpio->en_n);
	if (ret)
		return ret;
	ret = meson6_gpio_map_resource(pdev, MESON6_GPIO_OUT, &gpio->out);
	if (ret)
		return ret;
	ret = meson6_gpio_map_resource(pdev, MESON6_GPIO_IN, &gpio->in);
	if (ret)
		return ret;

	if (of_property_read_u32(pdev->dev.of_node, "ngpios", &ngpios) ||
	    !ngpios || ngpios > 32)
		return -EINVAL;

	spin_lock_init(&gpio->lock);
	gpio->chip.label = pdev->dev.of_node->name;
	gpio->chip.dev = &pdev->dev;
	gpio->chip.owner = THIS_MODULE;
	gpio->chip.base = -1;
	gpio->chip.ngpio = ngpios;
	gpio->chip.get_direction = meson6_gpio_get_direction;
	gpio->chip.direction_input = meson6_gpio_direction_input;
	gpio->chip.direction_output = meson6_gpio_direction_output;
	gpio->chip.get = meson6_gpio_get;
	gpio->chip.set = meson6_gpio_set;

	ret = meson6_gpio_setup_irq_router(pdev, gpio);
	if (ret) {
		dev_err(&pdev->dev, "invalid GPIO IRQ router configuration\n");
		return ret;
	}

	platform_set_drvdata(pdev, gpio);
	ret = gpiochip_add(&gpio->chip);
	if (ret)
		dev_err(&pdev->dev, "failed to register GPIO bank\n");

	return ret;
}

static int meson6_gpio_remove(struct platform_device *pdev)
{
	struct meson6_gpio *gpio = platform_get_drvdata(pdev);

	gpiochip_remove(&gpio->chip);
	return 0;
}

static const struct of_device_id meson6_gpio_match[] = {
	{ .compatible = "amlogic,meson6-gpio" },
	{ }
};
MODULE_DEVICE_TABLE(of, meson6_gpio_match);

static struct platform_driver meson6_gpio_driver = {
	.probe = meson6_gpio_probe,
	.remove = meson6_gpio_remove,
	.driver = {
		.name = "meson6-gpio",
		.of_match_table = meson6_gpio_match,
	},
};
module_platform_driver(meson6_gpio_driver);

MODULE_DESCRIPTION("Amlogic Meson6 GPIO driver");
MODULE_LICENSE("GPL v2");
