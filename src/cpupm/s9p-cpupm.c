// SPDX-License-Identifier: GPL-2.0
/*
 * Galaxy S9+ (exynos9810): CPU power-down in idle (C2) and on hotplug.
 *
 * On this SoC a core is powered down by the APM firmware (FlexPMU), not by
 * EL3 alone: PSCI CPU_SUSPEND / CPU_OFF only park the core in WFI, and the
 * APM cuts its power when it sees that WFI *and* the kernel has told the PMU
 * beforehand. That is what the vendor kernel's exynos-powermode.c does
 * around every C2 entry and every CPU_OFF (cal_cpu_disable / cal_cpu_enable,
 * flexpmu_cal_cpu_exynos9810.h core*_off / core*_on), and what mainline has
 * no place for -- which is why a plain PSCI idle state in the device tree
 * hung the boot. Per core, with b its bit (A55 n: 4 + n, M3 n: n):
 *
 *   off: CPU_INFORM[phy] (0x14060860 + 4 * phy) = 1 (C2)
 *        GRP2_INTR_BID_ENABLE (0x14070200) |= b
 *        GRP1_INTR_BID_CLEAR (0x1407010c) = b if GRP1 pending (0x14070108)
 *        PMU_EVENT_INTERRUPT_ENABLE_GRP2 (0x14067f08) |= b
 *   on:  CPU_INFORM[phy] = 0, GRP2 enable &= ~b, GRP2 pending cleared
 *        (0x1407020c), event enable &= ~b
 *
 * The idle state is the vendor's C2: PSCI power_state 0x10000, latencies of
 * the vendor DT (A55 70 + 160 us, residency 2 ms; M3 235 + 220 us, 3.5 ms).
 * The per-core arch timer stops while the core is off. As in the vendor
 * kernel, every core ticks from its own MCT local timer instead (an SPI to
 * that core, in the always-on domain; patch_tree.py ranks it above the arch
 * timer), so the state needs no broadcast. The broadcast alternative (MCT
 * global comparator, then an IPI) lost wake-ups: a core stayed off for good.
 *
 * Hotplug uses the kernel's s9p_cpu_hooks (scripts/build/patch_tree.py):
 * the dying core does the "off" writes before CPU_OFF, the killing core
 * waits for the PMU status to drop (vendor: up to 2 s), the new core does the
 * "on" writes before it is marked starting.
 *
 *   cpus=<mask>   CPUs that get the C2 state (default all; bring-up: one)
 *   hotplug=1     install the hotplug hooks
 *   /sys/module/s9p_cpupm/parameters/stats   C2 entries / failures per CPU
 */

#include <linux/cpu.h>
#include <linux/cpu_pm.h>
#include <linux/cpuidle.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/psci.h>
#include <linux/reboot.h>
#include <linux/sched.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <asm/barrier.h>
#include <asm/cputype.h>

#define PMU_PHYS		0x14060000
#define INTR_PHYS		0x14070000
#define CPU_INFORM(phy)		(0x0860 + 4 * (phy))
#define EVT_EN_GRP2		0x7f08
#define GRP1_PEND		0x0108
#define GRP1_CLEAR		0x010c
#define GRP2_ENABLE		0x0200
#define GRP2_PEND		0x0208
#define GRP2_CLEAR		0x020c
#define INFORM_C2		1

#define PSCI_C2			0x10000

static unsigned int cpus = 0xff;
module_param(cpus, uint, 0444);
MODULE_PARM_DESC(cpus, "CPUs that get the C2 idle state (bit mask)");
static bool hotplug;
module_param(hotplug, bool, 0444);
MODULE_PARM_DESC(hotplug, "power cores down on CPU_OFF and back up on CPU_ON");

static void __iomem *pmu, *intr;
static DEFINE_RAW_SPINLOCK(pmu_lock);
static DEFINE_PER_CPU(unsigned long, c2_entries);
static DEFINE_PER_CPU(unsigned long, c2_errors);

/* the vendor's linear physical CPU number: (cluster << 2) | core */
static u8 phy_table[NR_CPUS];

static unsigned int phy_of(unsigned int cpu)
{
	return phy_table[cpu];
}

static void read_phy(void *unused)
{
	u64 mpidr = read_cpuid_mpidr();
	/* the A55s are multithreading-style (MT set, core in Aff1), the M3s not */
	int lvl = (mpidr & MPIDR_MT_BITMASK) ? 1 : 0;

	phy_table[smp_processor_id()] = (MPIDR_AFFINITY_LEVEL(mpidr, (lvl + 1)) << 2) |
					 MPIDR_AFFINITY_LEVEL(mpidr, lvl);
}

static u32 bit_of(unsigned int phy)
{
	return BIT(phy < 4 ? 4 + phy : phy - 4);
}

/* STATUS of a core: M3 n at 0x2004 + 8n, A55 n at 0x2024 + 8n */
static u32 status_of(unsigned int phy)
{
	return phy < 4 ? 0x2024 + 8 * phy : 0x2004 + 8 * (phy - 4);
}

static void rmw(void __iomem *base, u32 off, u32 clr, u32 set)
{
	writel_relaxed((readl_relaxed(base + off) & ~clr) | set, base + off);
}

static void core_off(unsigned int phy)
{
	u32 b = bit_of(phy);
	unsigned long flags;

	raw_spin_lock_irqsave(&pmu_lock, flags);
	writel_relaxed(INFORM_C2, pmu + CPU_INFORM(phy));
	/*
	 * GRP2 pending latches every WFI of the core, enabled or not (0xef
	 * after boot). The vendor clears it only on the way back up; a stale
	 * one hands the APM a power-down request while the core is still
	 * running, the APM drops it, and the real WFI later finds nothing
	 * pending: the core sits in EL3's WFI, powered, for good. Clear first.
	 */
	writel_relaxed(b, intr + GRP2_CLEAR);
	dsb(sy);
	rmw(intr, GRP2_ENABLE, 0, b);
	if (readl_relaxed(intr + GRP1_PEND) & b)
		writel_relaxed(b, intr + GRP1_CLEAR);
	rmw(pmu, EVT_EN_GRP2, 0, b);
	raw_spin_unlock_irqrestore(&pmu_lock, flags);
}

static void core_on(unsigned int phy)
{
	u32 b = bit_of(phy);
	unsigned long flags;

	raw_spin_lock_irqsave(&pmu_lock, flags);
	writel_relaxed(0, pmu + CPU_INFORM(phy));
	rmw(intr, GRP2_ENABLE, b, 0);
	if (readl_relaxed(intr + GRP2_PEND) & b)
		writel_relaxed(b, intr + GRP2_CLEAR);
	rmw(pmu, EVT_EN_GRP2, b, 0);
	raw_spin_unlock_irqrestore(&pmu_lock, flags);
}

/* ------------------------------------------------------------ cpuidle */

static __cpuidle int wfi_enter(struct cpuidle_device *dev, struct cpuidle_driver *drv, int idx)
{
	dsb(sy);
	wfi();
	return idx;
}

/*
 * One power-down at a time. With two cores asking the APM for C2 at the same
 * moment, one of them sometimes never came back: still in EL3's WFI with the
 * PMU showing it powered (the APM had dropped the request), or powered off
 * with its wake-up lost. A core may start its own request only when no other
 * core is between "requested" and "PMU reports it off".
 */
static unsigned int going_down = UINT_MAX;	/* phy of the core in flight */
static DEFINE_PER_CPU(unsigned long, c2_busy);
static DEFINE_PER_CPU(u64, c2_since);	/* jiffies64 of the entry, 0 outside */

static bool claim_transition(unsigned int phy)
{
	unsigned long flags;
	bool ok = false;

	raw_spin_lock_irqsave(&pmu_lock, flags);
	if (going_down != UINT_MAX &&
	    !(readl_relaxed(pmu + status_of(going_down)) & 0xf))
		going_down = UINT_MAX;		/* that one is off now */
	if (going_down == UINT_MAX) {
		going_down = phy;
		ok = true;
	}
	raw_spin_unlock_irqrestore(&pmu_lock, flags);
	return ok;
}

static void release_transition(unsigned int phy)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&pmu_lock, flags);
	if (going_down == phy)
		going_down = UINT_MAX;
	raw_spin_unlock_irqrestore(&pmu_lock, flags);
}

static __cpuidle int c2_enter(struct cpuidle_device *dev, struct cpuidle_driver *drv, int idx)
{
	unsigned int phy = phy_of(dev->cpu);
	int ret;

	if (!claim_transition(phy)) {
		this_cpu_inc(c2_busy);
		return -EBUSY;
	}
	if (cpu_pm_enter()) {
		release_transition(phy);
		return -EBUSY;
	}
	core_off(phy);
	WRITE_ONCE(*this_cpu_ptr(&c2_since), get_jiffies_64() | 1);
	ret = s9p_psci_suspend_lossy(PSCI_C2);
	WRITE_ONCE(*this_cpu_ptr(&c2_since), 0);
	core_on(phy);
	release_transition(phy);
	cpu_pm_exit();
	if (ret) {
		this_cpu_inc(c2_errors);
		return -EBUSY;
	}
	this_cpu_inc(c2_entries);
	return idx;
}

#define S9P_DRV(_name, _entry, _exit, _res) {				\
	/* no .owner: cpuidle would pin the module it is unregistered from */ \
	.name = _name,							\
	.states[0] = {							\
		.enter = wfi_enter,					\
		.exit_latency = 1,					\
		.target_residency = 1,					\
		.name = "WFI",						\
		.desc = "ARM WFI",					\
	},								\
	.states[1] = {							\
		.enter = c2_enter,					\
		.exit_latency = (_entry) + (_exit),			\
		.target_residency = (_res),				\
		.flags = CPUIDLE_FLAG_RCU_IDLE,				\
		.name = "C2",						\
		.desc = "core power-down (FlexPMU)",			\
	},								\
	.state_count = 2,						\
}

static struct cpuidle_driver drv_little = S9P_DRV("s9p_idle_a55", 70, 160, 2000);
static struct cpuidle_driver drv_big = S9P_DRV("s9p_idle_m3", 235, 220, 3500);
static bool little_on, big_on;

static int register_cluster(struct cpuidle_driver *drv, bool big)
{
	unsigned int cpu;

	drv->cpumask = kzalloc(cpumask_size(), GFP_KERNEL);
	if (!drv->cpumask)
		return -ENOMEM;
	for_each_possible_cpu(cpu)
		if ((phy_of(cpu) >= 4) == big && (cpus & BIT(cpu)))
			cpumask_set_cpu(cpu, (struct cpumask *)drv->cpumask);
	if (cpumask_empty(drv->cpumask)) {
		kfree(drv->cpumask);
		drv->cpumask = NULL;
		return 0;
	}
	return cpuidle_register(drv, NULL) ?: 1;
}

/* ------------------------------------------------------------ hotplug */

static void hp_die(unsigned int cpu)
{
	core_off(phy_of(cpu));
}

static int hp_kill(unsigned int cpu)
{
	unsigned int phy = phy_of(cpu);
	u32 v;
	int ret = readl_poll_timeout(pmu + status_of(phy), v, !(v & 0xf), 100, 2000000);

	if (ret)
		pr_warn("s9p-cpupm: CPU%u still powered (STATUS %08x)\n", cpu, v);
	return ret;
}

static void hp_starting(unsigned int cpu)
{
	core_on(phy_of(cpu));
}

/* ----------------------------------------------------------- deadman */

/*
 * Bring-up aid: a core that never leaves C2 stalls whatever is queued on it,
 * and the phone hangs without a trace for minutes. Once a second, from CPU0:
 * a core in C2 for over a second gets a wake-up; if it is still there 200 ms
 * later, the PMU, INTR_BID and FlexPMU state go to the kernel log and the
 * phone restarts 3 s later (the flight recorder writes the log to disk).
 */
static bool deadman = true;
module_param(deadman, bool, 0644);
static void __iomem *flex;

static void dump_state(unsigned int stuck)
{
	unsigned int cpu, i;

	pr_emerg("s9p-cpupm: CPU%u did not leave C2\n", stuck);
	for_each_possible_cpu(cpu)
		pr_emerg("s9p-cpupm: cpu%u phy%u inform %08x status %08x C2 %lu\n", cpu,
			 phy_of(cpu), readl_relaxed(pmu + CPU_INFORM(phy_of(cpu))),
			 readl_relaxed(pmu + status_of(phy_of(cpu))), per_cpu(c2_entries, cpu));
	pr_emerg("s9p-cpupm: GRP1 en %08x pend %08x GRP2 en %08x pend %08x GRP3 %08x/%08x GRP4 %08x/%08x evt %08x\n",
		 readl_relaxed(intr + 0x100), readl_relaxed(intr + 0x108),
		 readl_relaxed(intr + 0x200), readl_relaxed(intr + 0x208),
		 readl_relaxed(intr + 0x300), readl_relaxed(intr + 0x308),
		 readl_relaxed(intr + 0x400), readl_relaxed(intr + 0x408),
		 readl_relaxed(pmu + EVT_EN_GRP2));
	for (i = 0; flex && i < 0x100; i += 16)
		pr_emerg("s9p-cpupm: flex+%03x %08x %08x %08x %08x\n", i,
			 readl_relaxed(flex + i), readl_relaxed(flex + i + 4),
			 readl_relaxed(flex + i + 8), readl_relaxed(flex + i + 12));
}

static void deadman_nop(void *unused)
{
}

static DEFINE_PER_CPU(unsigned long, c2_rescues);

/*
 * The failure, caught by the dump above: the core's request reached the APM
 * (GRP2 pending consumed) but a wake-up was already pending for it in GRP1
 * -- an interrupt arrived while EL3 was turning the core's GIC interface off.
 * The APM correctly aborts the power-down, and the core stays in EL3's WFI,
 * powered, with no interrupt able to reach it. Power-cycle it through its PMU
 * CONFIGURATION register: back on, EL3 resumes it through cpu_resume, which
 * is exactly the normal way out of C2.
 */
static u32 config_of(unsigned int phy)
{
	return status_of(phy) - 4;
}

static bool rescue(unsigned int cpu)
{
	unsigned int phy = phy_of(cpu);
	u32 v;

	if (!(readl_relaxed(pmu + status_of(phy)) & 0xf))
		return false;			/* really off: not this case */
	pr_warn("s9p-cpupm: CPU%u held in EL3 after an aborted power-down, cycling it\n", cpu);
	writel_relaxed(0, pmu + config_of(phy));
	if (readl_poll_timeout(pmu + status_of(phy), v, !(v & 0xf), 10, 20000))
		return false;
	writel_relaxed(0xf, pmu + config_of(phy));
	if (readl_poll_timeout(pmu + status_of(phy), v, (v & 0xf) == 0xf, 10, 20000))
		return false;
	per_cpu(c2_rescues, cpu)++;
	return true;
}

static void deadman_work(struct work_struct *w);
static DECLARE_DELAYED_WORK(deadman_dwork, deadman_work);
static bool deadman_fired;

static void deadman_work(struct work_struct *w)
{
	u64 now = get_jiffies_64();
	unsigned int cpu;

	for_each_online_cpu(cpu) {
		u64 since = READ_ONCE(per_cpu(c2_since, cpu));

		if (!since || time_before64(now, since + HZ))
			continue;
		smp_call_function_single(cpu, deadman_nop, NULL, 0);
		msleep(200);
		if (READ_ONCE(per_cpu(c2_since, cpu)) != since)
			continue;
		if (rescue(cpu)) {
			msleep(200);
			if (READ_ONCE(per_cpu(c2_since, cpu)) != since)
				continue;
		}
		if (!deadman_fired) {
			deadman_fired = true;
			dump_state(cpu);
			msleep(3000);
			emergency_restart();
		}
	}
	schedule_delayed_work_on(0, &deadman_dwork, HZ);
}

/* ------------------------------------------------------------- stats */

static int stats_get(char *buf, const struct kernel_param *kp)
{
	unsigned int cpu;
	int n = 0;

	for_each_possible_cpu(cpu)
		n += sysfs_emit_at(buf, n, "cpu%u phy%u C2 %lu errors %lu busy %lu rescued %lu status %08x\n",
				   cpu, phy_of(cpu), per_cpu(c2_entries, cpu), per_cpu(c2_errors, cpu),
				   per_cpu(c2_busy, cpu), per_cpu(c2_rescues, cpu),
				   readl_relaxed(pmu + status_of(phy_of(cpu))));
	return n;
}

static const struct kernel_param_ops stats_ops = { .get = stats_get };
module_param_cb(stats, &stats_ops, NULL, 0444);

static int __init s9p_cpupm_init(void)
{
	unsigned int cpu;
	int ret;

	if (!of_machine_is_compatible("samsung,exynos9810"))
		return -ENODEV;
	pmu = ioremap(PMU_PHYS, 0x8000);
	intr = ioremap(INTR_PHYS, 0x1000);
	if (!pmu || !intr) {
		ret = -ENOMEM;
		goto err;
	}
	/* all eight are online at load (maxcpus=8); their MPIDR, from each */
	for_each_online_cpu(cpu)
		smp_call_function_single(cpu, read_phy, NULL, 1);
	if (num_online_cpus() != num_possible_cpus()) {
		pr_err("s9p-cpupm: load with all CPUs online\n");
		ret = -EBUSY;
		goto err;
	}

	/*
	 * The PMU survives a warm reboot: a core that hung in C2 leaves its
	 * CPU_INFORM at 1 (or 0x10001, bit 16 being EL3's own mark) for the
	 * next boot. Start from the vendor's running state for every core.
	 */
	for_each_possible_cpu(cpu)
		core_on(phy_of(cpu));

	if (hotplug) {
		s9p_cpu_hooks.starting = hp_starting;
		s9p_cpu_hooks.kill = hp_kill;
		smp_wmb();
		s9p_cpu_hooks.die = hp_die;
	}

	ret = register_cluster(&drv_little, false);
	if (ret < 0)
		goto err_hooks;
	little_on = ret > 0;
	ret = register_cluster(&drv_big, true);
	if (ret < 0)
		goto err_little;
	big_on = ret > 0;
	flex = ioremap(0x0204f800, 0x400);
	if (deadman)
		schedule_delayed_work_on(0, &deadman_dwork, HZ);
	pr_info("s9p-cpupm: C2 on CPUs %*pbl, hotplug hooks %s\n", 8, (unsigned long[]){ cpus },
		hotplug ? "on" : "off");
	return 0;

err_little:
	if (little_on)
		cpuidle_unregister(&drv_little);
err_hooks:
	memset(&s9p_cpu_hooks, 0, sizeof(s9p_cpu_hooks));
	synchronize_rcu();
err:
	kfree(drv_little.cpumask);
	kfree(drv_big.cpumask);
	if (intr)
		iounmap(intr);
	if (pmu)
		iounmap(pmu);
	return ret;
}

static void __exit s9p_cpupm_exit(void)
{
	cancel_delayed_work_sync(&deadman_dwork);
	if (flex)
		iounmap(flex);
	if (big_on)
		cpuidle_unregister(&drv_big);
	if (little_on)
		cpuidle_unregister(&drv_little);
	memset(&s9p_cpu_hooks, 0, sizeof(s9p_cpu_hooks));
	synchronize_rcu();
	kfree(drv_little.cpumask);
	kfree(drv_big.cpumask);
	iounmap(intr);
	iounmap(pmu);
}

module_init(s9p_cpupm_init);
module_exit(s9p_cpupm_exit);
MODULE_DESCRIPTION("Galaxy S9+ CPU power-down in idle and hotplug (vendor FlexPMU handshake)");
MODULE_LICENSE("GPL");
