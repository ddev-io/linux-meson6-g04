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
#include <linux/of.h>
#include <linux/of_platform.h>
#include <asm/mach/arch.h>

#define MESON6_WDT_PHYS		0xc1109900
#define MESON6_WDT_SIZE		0x8
#define MESON6_WDT_TC		0x0
#define MESON6_WDT_RESET	0x4
#define MESON6_WDT_TC_EN	(1 << 22)

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
