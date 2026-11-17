/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Meson6 G04 secondary CPU boot, using the SRAM mailbox and wakeup
 * sequence from the Amlogic 3.0.101 board kernel.
 */
#include <linux/bitops.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/smp.h>
#include <asm/barrier.h>
#include <asm/smp_plat.h>
#include <asm/smp_scu.h>

#define G04_SCU_PHYS		0xc4300000
#define G04_CPU_MAILBOX_PHYS	0xd901ff80
#define G04_CPU_CONTROL		0
#define G04_CPU1_BOOT_ADDR	4

static void __iomem *g04_cpu_mailbox;

static void __init g04_smp_prepare_cpus(unsigned int max_cpus)
{
	void __iomem *scu;

	scu = ioremap(G04_SCU_PHYS, 0x100);
	if (!scu) {
		pr_err("meson6-g04: cannot map SCU\n");
		return;
	}

	scu_enable(scu);
	iounmap(scu);

	g04_cpu_mailbox = ioremap(G04_CPU_MAILBOX_PHYS, 8);
	if (!g04_cpu_mailbox)
		pr_err("meson6-g04: cannot map CPU1 boot mailbox\n");
}

static int g04_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	if (!g04_cpu_mailbox)
		return -ENODEV;
	if (cpu_logical_map(cpu) != 0x201)
		return -EINVAL;

	/* ARM's __cpu_up publishes the bootstrap state before calling us. */
	writel(virt_to_phys(secondary_startup),
	       g04_cpu_mailbox + G04_CPU1_BOOT_ADDR);
	writel(BIT(0) | BIT(1), g04_cpu_mailbox + G04_CPU_CONTROL);
	dsb(sy);
	sev();
	arch_send_wakeup_ipi_mask(cpumask_of(cpu));
	return 0;
}

#ifdef CONFIG_HOTPLUG_CPU
static int g04_cpu_disable(unsigned int cpu)
{
	/* Runtime offlining needs a separate Meson6 shutdown sequence. */
	return -EPERM;
}
#endif

static struct smp_operations g04_smp_ops __initdata = {
	.smp_prepare_cpus = g04_smp_prepare_cpus,
	.smp_boot_secondary = g04_boot_secondary,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_disable = g04_cpu_disable,
#endif
};
CPU_METHOD_OF_DECLARE(meson6_g04, "ddev-io,meson6-g04-smp", &g04_smp_ops);
