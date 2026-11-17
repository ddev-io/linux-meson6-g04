/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/spinlock.h>

static DEFINE_SPINLOCK(g04_sdio_gate_lock);
static struct clk_gate g04_sdio_gate = {
	.bit_idx = 17, /* HHI_GCLK_MPEG0: GCLK_SDIO */
	.lock = &g04_sdio_gate_lock,
};

static int __init g04_sdio_clock_init(void)
{
	struct device_node *np;
	struct clk_init_data init = {
		.name = "g04-sdio-core",
		.ops = &clk_gate_ops,
		.flags = CLK_IS_BASIC,
		.num_parents = 1,
	};
	struct clk *clk;
	const char *parent;
	void __iomem *base;
	int ret;

	if (!of_machine_is_compatible("ddev-io,meson6-g04"))
		return 0;
	np = of_find_compatible_node(NULL, NULL, "ddev-io,g04-sdio-gate");
	if (!np)
		return 0;
	parent = of_clk_get_parent_name(np, 0);
	if (!parent) {
		ret = -EINVAL;
		goto put_node;
	}
	base = of_iomap(np, 0);
	if (!base) {
		ret = -ENOMEM;
		goto put_node;
	}
	init.parent_names = &parent;
	g04_sdio_gate.reg = base;
	g04_sdio_gate.hw.init = &init;
	clk = clk_register(NULL, &g04_sdio_gate.hw);
	if (IS_ERR(clk)) {
		ret = PTR_ERR(clk);
		iounmap(base);
		goto put_node;
	}
	ret = of_clk_add_provider(np, of_clk_src_simple_get, clk);
	if (ret) {
		clk_unregister(clk);
		iounmap(base);
	} else {
		pr_info("meson6-g04: registered SDIO core clock gate\n");
	}
put_node:
	of_node_put(np);
	return ret;
}
arch_initcall(g04_sdio_clock_init);
