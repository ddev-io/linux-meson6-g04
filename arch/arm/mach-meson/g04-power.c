/*
 * G04 bootloader power/restart ABI, retained from the working vendor kernel.
 * SPDX-License-Identifier: GPL-2.0
 */
#include <linux/bitops.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/pm.h>
#include <linux/reboot.h>
#include <linux/string.h>
#include <asm/system_misc.h>

#define G04_REBOOT_REASON_PHYS 0xc8100004
#define G04_WDT_PHYS           0xc1109900
#define G04_WDT_TC             0
#define G04_WDT_RESET          4
#define G04_WDT_RESTART        ((3U << 24) | BIT(22) | 100)

static void __iomem *g04_reboot_reason;
static void __iomem *g04_watchdog;

static u32 g04_bootloader_reason(const char *cmd)
{
	if (!cmd)
		return 0x01010101;
	if (!strcmp(cmd, "charging_reboot"))
		return 0;
	if (!strcmp(cmd, "recovery") || !strcmp(cmd, "factory_reset"))
		return 0x02020202;
	if (!strcmp(cmd, "update"))
		return 0x03030303;
	if (!strcmp(cmd, "report_crash"))
		return 0x04040404;
	if (!strcmp(cmd, "factory_testl_reboot"))
		return 0x05050505;
	if (!strcmp(cmd, "switch_system"))
		return 0x06060606;
	if (!strcmp(cmd, "safe_mode"))
		return 0x07070707;
	if (!strcmp(cmd, "lock_system"))
		return 0x08080808;
	if (!strcmp(cmd, "usb_burner_reboot"))
		return 0x09090909;
	return 0x01010101;
}

static void g04_restart(enum reboot_mode mode, const char *cmd)
{
	u32 reason = g04_bootloader_reason(cmd);

	pr_emerg("meson6-g04: restart command=%s reason=%08x\n",
		 cmd ? cmd : "normal", reason);
	writel(reason, g04_reboot_reason);
	readl(g04_reboot_reason);
	/* Match vendor arch_reset: clear the counter, then arm both CPUs once. */
	writel(0, g04_watchdog + G04_WDT_RESET);
	writel(G04_WDT_RESTART, g04_watchdog + G04_WDT_TC);
	readl(g04_watchdog + G04_WDT_TC);
	while (1)
		cpu_relax();
}

static void g04_power_off(void)
{
	/* machine_power_off has already stopped secondary CPUs and interrupts. */
	pr_emerg("meson6-g04: power off through bootloader charging mode\n");
	g04_restart(REBOOT_HARD, "charging_reboot");
}

static int __init g04_power_init(void)
{
	if (!of_machine_is_compatible("ddev-io,meson6-g04"))
		return 0;
	if (pm_power_off || arm_pm_restart) {
		pr_err("meson6-g04: power/restart hooks already registered\n");
		return -EBUSY;
	}
	g04_reboot_reason = ioremap(G04_REBOOT_REASON_PHYS, 4);
	if (!g04_reboot_reason)
		return -ENOMEM;
	/* The watchdog driver also maps these registers; only shutdown uses us. */
	g04_watchdog = ioremap(G04_WDT_PHYS, 8);
	if (!g04_watchdog) {
		iounmap(g04_reboot_reason);
		g04_reboot_reason = NULL;
		return -ENOMEM;
	}
	arm_pm_restart = g04_restart;
	pm_power_off = g04_power_off;
	pr_info("meson6-g04: registered vendor poweroff and reboot-mode hooks\n");
	return 0;
}
arch_initcall(g04_power_init);
