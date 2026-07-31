/*
 * Copyright (C) 2014 Carlo Caione <carlo@caione.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */

#include <linux/io.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <asm/mach/arch.h>

#define MESON6_WDT_PHYS		0xc1109900
#define MESON6_WDT_SIZE		0x8
#define MESON6_WDT_TC		0x0
#define MESON6_WDT_RESET	0x4
#define MESON6_WDT_TC_EN	(1 << 22)

#define MESON6_G04_CLK81_RATE	200000000UL

/*
 * The vendor Meson6 kernel exposes clk81 through its legacy clock tree.
 * Mainline 3.19 has no Meson6 clock provider yet, but the legacy NAND
 * driver still looks it up by name before it can initialise the flash.
 * Register the measured G04 clock early enough for all platform probes.
 */
static int __init meson6_g04_clk81_init(void)
{
	struct clk *clk;
	int ret;

	if (!of_machine_is_compatible("ddev-io,meson6-g04"))
		return 0;

	clk = clk_register_fixed_rate(NULL, "clk81", NULL, CLK_IS_ROOT,
				      MESON6_G04_CLK81_RATE);
	if (IS_ERR(clk)) {
		pr_err("meson6-g04: failed to register clk81: %ld\n",
		       PTR_ERR(clk));
		return PTR_ERR(clk);
	}

	ret = clk_register_clkdev(clk, NULL, "clk81");
	if (ret) {
		pr_err("meson6-g04: failed to publish clk81: %d\n", ret);
		clk_unregister(clk);
		return ret;
	}

	pr_info("meson6-g04: registered legacy clk81 at %lu Hz\n",
		MESON6_G04_CLK81_RATE);
	return 0;
}
arch_initcall(meson6_g04_clk81_init);

static void __init meson_init_early(void)
{
	void __iomem *wdt;
	u32 value;

	if (!of_machine_is_compatible("ddev-io,meson6-g04"))
		return;

	/* U-Boot may leave this watchdog running.  Stop it before probes. */
	wdt = ioremap(MESON6_WDT_PHYS, MESON6_WDT_SIZE);
	if (!wdt)
		return;

	writel_relaxed(0, wdt + MESON6_WDT_RESET);
	value = readl_relaxed(wdt + MESON6_WDT_TC);
	writel_relaxed(value & ~MESON6_WDT_TC_EN, wdt + MESON6_WDT_TC);
	readl_relaxed(wdt + MESON6_WDT_TC);
	iounmap(wdt);
}

static const char * const meson_common_board_compat[] = {
	"amlogic,meson6",
	"amlogic,meson8",
	NULL,
};

DT_MACHINE_START(MESON, "Amlogic Meson platform")
	.dt_compat	= meson_common_board_compat,
	.l2c_aux_val	= 0,
	.l2c_aux_mask	= ~0,
	.init_early	= meson_init_early,
MACHINE_END
