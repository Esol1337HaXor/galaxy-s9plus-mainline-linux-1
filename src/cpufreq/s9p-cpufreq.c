// SPDX-License-Identifier: GPL-2.0
/*
 * cpufreq for the Samsung Exynos 9810 (Galaxy S9+).
 *
 * sboot leaves both CPU clusters at ~1.05 GHz. The vendor kernel scales them
 * by asking the ACPM DVFS plugin (channel 5); that plugin refuses this
 * port's requests, so this driver does what the port's validated userspace
 * s9p-cpuclk tool does: set the cluster BUCK through the APM firmware
 * (channel 2), switch the cluster mux to the 26 MHz OSC, relock the PLL with
 * the vendor's own rate table entries (cmucal-node.c), and switch back. The
 * PLL configurations are the exact vendor entries; 2704 MHz (M=208, P=2,
 * S=0) and 1794 MHz (M=138, P=2, S=0) are additionally proven on this device
 * with full core validation.
 *
 * Voltage ordering matters: on the way up the BUCK is raised before the PLL,
 * on the way down the PLL drops before the BUCK.
 *
 * The clock switch runs at 26 MHz for the relock window even when the
 * calling CPU is inside the cluster being switched -- the sequence is the
 * vendor's, and the arch timer is independent of the cluster clock.
 *
 * Two policies live on top of the frequency table, both as cpufreq QoS
 * maximum constraints refreshed every 100 ms:
 *
 *  - Samsung's busy-core rule for the M3 cluster: 2704 MHz with one busy
 *    core, 2327 MHz with two, 1794 MHz above that (four cores at 2704 MHz
 *    hard-reset the SoC, measured).
 *  - a thermal cap read straight from the TMU (the stock 96/91 C hotplug
 *    limits are far too late for a phone that must never get hot): the
 *    M3 cluster steps down from 72 C, the A55 cluster from 76 C, the GPU
 *    from 80 C, each level with 8 C of hysteresis on the way back up.
 */
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm_qos.h>
#include <linux/sched.h>
#include <linux/workqueue.h>

#include <asm/cputype.h>
#include <asm/smp_plat.h>

#include <linux/soc/samsung/s9p-acpm.h>

#define PLL_CON0_ENABLE		BIT(31)
#define PLL_CON0_STABLE		BIT(29)
#define PLL_CON0_MUX_SEL		BIT(4)
#define PLL_CON0_PMS_MASK	((0x3ffu << 16) | (0x3fu << 8) | 0x7u)
#define MUX_BUSY		BIT(16)

struct s9p_pll_step {
	u32 freq_khz;
	u16 m;
	u8 p, s;
	u8 vsel;
};

/*
 * Voltage/frequency pairs. Every point is the userspace s9p-governor's
 * stress-tested value plus a small margin, except the two
 * validated PLL maxima. Two hard limits from that testing are encoded
 * below: the M3 cluster must not run 2704 MHz with more than two busy
 * cores (hard reset), and 2327 MHz needs 1075 mV (1000 mV froze).
 */
/* CPUCL0: 4x Cortex-A55. Vendor PLL table entries + the validated maximum. */
static const struct s9p_pll_step s9p_steps_little[] = {
	{ 1794000, 138, 2, 0, 0x70 },	/* 1000 mV; 900 mV reset -- boost only */
	{ 1499333, 173, 3, 0, 0x60 },	/*  900 mV, governor tested at 850  */
	{ 1150500, 354, 4, 1, 0x58 },	/*  850 mV                          */
	{  650000, 200, 4, 1, 0x50 },	/*  800 mV                          */
	{  349916, 323, 6, 2, 0x4b },	/*  768 mV, the governor's floor    */
};

/* CPUCL1: 4x Mongoose M3, Samsung's busy-core policy applied at runtime. */
static const struct s9p_pll_step s9p_steps_big[] = {
	{ 2704000, 208, 2, 0, 0x88 },	/* 1150 mV, 1 busy core -- boost only     */
	{ 2327000, 358, 4, 0, 0x7c },	/* 1075 mV, tested (1000 mV froze)        */
	{ 1794000, 138, 2, 0, 0x60 },	/*  900 mV, governor tested at 875        */
	{ 1469000, 452, 4, 1, 0x58 },	/*  850 mV, governor tested at 800        */
	{ 1066000, 246, 3, 1, 0x48 },	/*  750 mV, governor tested at 700        */
	{  400000, 200, 13, 0, 0x40 },	/*  700 mV                                */
};

static struct cpufreq_frequency_table s9p_ftab_little[] = {
	{ .frequency = 1794000 },
	{ .frequency = 1499333 },
	{ .frequency = 1150500 },
	{ .frequency = 650000 },
	{ .frequency = 349916 },
	{ .frequency = CPUFREQ_TABLE_END },
};

static struct cpufreq_frequency_table s9p_ftab_big[] = {
	{ .frequency = 2704000 },
	{ .frequency = 2327000 },
	{ .frequency = 1794000 },
	{ .frequency = 1469000 },
	{ .frequency = 1066000 },
	{ .frequency = 400000 },
	{ .frequency = CPUFREQ_TABLE_END },
};

/* thermal levels: enter level i (or higher) at up_at C, leave it again
 * only below up_at - THERMAL_HYST and after THERMAL_HOLD_MS */
struct s9p_thermal_level {
	unsigned int max_khz;
	int up_at;
};

#define THERMAL_HYST		8
#define THERMAL_HOLD_MS		2000
#define THERMAL_PERIOD_MS	100

/*
 * Idle floor.
 *
 * schedutil never took the M3 cluster all the way down: measured on a quiet
 * desktop it sat at 1066 MHz and kept jumping to 1794 MHz, which is 750 and
 * 900 mV, while its lowest step is 400 MHz on 700 mV. Something wakes those
 * cores often enough -- timers, interrupts, this very poll -- that the
 * utilisation signal never decays far enough for the governor to ask for the
 * bottom step. The A55 cluster does reach its floor on its own.
 *
 * So the floor is applied here instead, as a cpufreq QoS maximum, which is
 * the same mechanism the thermal cap and the busy-core rule already use: it
 * goes through the cpufreq core, so policy->cur stays truthful and the
 * governor is not fighting a hardware state it does not know about.
 *
 * IDLE_ENTER_POLLS of 5 means half a second of every core in the cluster
 * being idle before the floor goes on, and while it is on the poll runs at
 * IDLE_RELEASE_MS so that the first task to arrive waits at most that long
 * for full speed. The fast poll only happens while the cluster is asleep
 * anyway.
 */
#define IDLE_ENTER_POLLS	5
#define IDLE_RELEASE_MS		25

static const struct s9p_thermal_level s9p_thermal_big[] = {
	{ 2704000,  0 },
	{ 2327000, 72 },
	{ 1794000, 80 },
	/* #157/#160 repeatedly jumped 1794 -> 1066 under eight-core load.
	 * Use the existing 1469 MHz / 850 mV point before the 88 C limit. */
	{ 1469000, 84 },
	{ 1066000, 88 },
};

static const struct s9p_thermal_level s9p_thermal_little[] = {
	{ 1794000,  0 },
	{ 1499333, 76 },
	{ 1150500, 86 },
};

static const struct s9p_thermal_level s9p_thermal_gpu[] = {
	{ 572000,  0 },
	{ 455000, 80 },
	{ 260000, 90 },
};

struct s9p_cluster {
	const char *name;
	void __iomem *pll_con0;
	void __iomem *mux;
	void __iomem *locktime;		/* PLL_LOCKTIME, block base */
	u8 buck_reg;		/* S2MPS18 BUCKxOUT register via ACPM */
	struct cpufreq_frequency_table *freq_table;
	const struct s9p_pll_step *steps;
	unsigned int nsteps;
	const struct s9p_thermal_level *thermal;
	unsigned int nthermal;
	unsigned int thermal_lvl;
	ktime_t thermal_changed;
	unsigned int busy_cap;
	unsigned int safe_max;		/* top step proven under load */
	struct freq_qos_request qos_thermal;
	struct freq_qos_request qos_busy;
	struct freq_qos_request qos_boost;
	struct freq_qos_request qos_idle;
	unsigned int idle_polls;	/* consecutive polls with no busy core */
	bool idle_capped;
	bool qos_added;
	unsigned int first_cpu;
};

/*
 * boost=0 (default) holds both clusters at the steps the userspace governor
 * ran under load for hours: M3 2327 MHz at 1075 mV, A55 1499 MHz at 900 mV.
 * boost=1 releases the stock maxima (2704 MHz at 1150 mV on one busy core,
 * 1794 MHz at 1000 mV) -- those two points have never survived a full
 * validation on this device, and a failure there is a silent SoC reset, so
 * they are switched on at runtime under observation
 * (/sys/module/s9p_cpufreq/parameters/boost), not blindly at boot.
 */
static unsigned int s9p_boost;

/*
 * Register blocks: CPUCL0 (A55, BUCK3) and CPUCL1 (M3, BUCK2), from the
 * validated s9p-cpuclk tool.
 */
static struct s9p_cluster s9p_clusters[2] = {
	{ .name = "little", .buck_reg = 0x23, .freq_table = s9p_ftab_little,
	  .steps = s9p_steps_little, .nsteps = ARRAY_SIZE(s9p_steps_little),
	  .thermal = s9p_thermal_little,
	  .nthermal = ARRAY_SIZE(s9p_thermal_little), .safe_max = 1499333 },
	{ .name = "big", .buck_reg = 0x20, .freq_table = s9p_ftab_big,
	  .steps = s9p_steps_big, .nsteps = ARRAY_SIZE(s9p_steps_big),
	  .thermal = s9p_thermal_big,
	  .nthermal = ARRAY_SIZE(s9p_thermal_big), .safe_max = 2327000 },
};

static void s9p_apply_boost(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(s9p_clusters); i++) {
		struct s9p_cluster *c = &s9p_clusters[i];

		if (c->qos_added)
			freq_qos_update_request(&c->qos_boost, s9p_boost ?
						FREQ_QOS_MAX_DEFAULT_VALUE :
						c->safe_max);
	}
}

static int s9p_boost_set(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_uint(val, kp);

	if (ret)
		return ret;
	s9p_boost = !!s9p_boost;
	pr_info("s9p-cpufreq: boost %s (%s)\n", s9p_boost ? "on" : "off",
		s9p_boost ? "stock maxima 2704/1794 MHz" :
			    "validated maxima 2327/1499 MHz");
	s9p_apply_boost();
	return 0;
}

static const struct kernel_param_ops s9p_boost_ops = {
	.set = s9p_boost_set,
	.get = param_get_uint,
};
module_param_cb(boost, &s9p_boost_ops, &s9p_boost, 0644);
MODULE_PARM_DESC(boost, "1: allow the stock maximum clocks (2704/1794 MHz)");

static DEFINE_MUTEX(s9p_clk_lock);
static unsigned int s9p_pll_lock_timeouts;
module_param_named(pll_lock_timeouts, s9p_pll_lock_timeouts, uint, 0444);

static int s9p_cluster_of(unsigned int cpu)
{
	if (MPIDR_AFFINITY_LEVEL(cpu_logical_map(cpu), 1) == 1)
		return 1;
	return 0;
}

static const struct s9p_pll_step *s9p_find_step(struct s9p_cluster *c,
						 unsigned int freq_khz)
{
	unsigned int i;

	for (i = 0; i < c->nsteps; i++)
		if (c->steps[i].freq_khz == freq_khz)
			return &c->steps[i];
	return NULL;
}

static u32 s9p_current_khz(struct s9p_cluster *c)
{
	u32 con0 = readl_relaxed(c->pll_con0);
	u32 m = (con0 >> 16) & 0x3ff;
	u32 p = (con0 >> 8) & 0x3f;
	u32 s = con0 & 0x7;

	/* The external mux selects the CMU switch clock, not necessarily OSC. */
	if (readl_relaxed(c->mux) & 0x1)
		return 0;
	if (!(con0 & PLL_CON0_MUX_SEL))
		return 26000;
	if (!p)
		return 0;
	return 26000 * m / p / (1u << s);
}

/*
 * This is the proven PLL register sequence. An internal-mux/lock/rollback
 * rewrite was tried and caused boot failures, so it was reverted. This
 * sequence retains known limitations (unbounded CMU mux waits and an
 * advisory STABLE check); it is not a validated clock-driver repair. See
 * docs/07-validation.md before changing it.
 */
static int s9p_set_step(struct s9p_cluster *c, const struct s9p_pll_step *step)
{
	u32 v;
	int ret;
	bool up;

	mutex_lock(&s9p_clk_lock);
	up = s9p_current_khz(c) < step->freq_khz;

	if (up) {
		/* up: raise the rail first */
		ret = s9p_acpm_pmic_write(c->buck_reg, step->vsel);
		if (ret) {
			pr_err_ratelimited("s9p-cpufreq: %s BUCK write for %u kHz failed (%d)\n",
					   c->name, step->freq_khz, ret);
			goto err_unlock;
		}
		udelay(200);
	}

	writel_relaxed(150u * step->p, c->locktime);
	writel_relaxed(1, c->mux);
	while (readl_relaxed(c->mux) & MUX_BUSY)
		cpu_relax();
	v = readl_relaxed(c->pll_con0) &
		~(PLL_CON0_ENABLE | PLL_CON0_PMS_MASK);
	writel_relaxed(v, c->pll_con0);
	writel_relaxed(v | ((u32)step->m << 16) |
		((u32)step->p << 8) | step->s, c->pll_con0);
	writel_relaxed(v | PLL_CON0_ENABLE | ((u32)step->m << 16) |
		((u32)step->p << 8) | step->s, c->pll_con0);
	/*
	 * The lock counter (LOCKTIME = 150 * P cycles of the 26 MHz OSC, about
	 * 12 us) bounds the relock; STABLE is seldom reported on these PLLs.
	 * Waiting 3 ms for it, with the whole cluster on the 26 MHz OSC, cost
	 * every transition 3.3 ms of a stalled cluster (95 % of them timed
	 * out): poll for eight lock times, then give it a margin and go on.
	 */
	ret = readl_poll_timeout_atomic(c->pll_con0, v, v & PLL_CON0_STABLE,
					2, 100);
	if (ret) {
		s9p_pll_lock_timeouts++;
		udelay(50);
		pr_info_once("s9p-cpufreq: %s PLL without STABLE at %u kHz (con0=%08x), continuing\n",
			     c->name, step->freq_khz, v);
	}
	writel_relaxed(0, c->mux);
	while (readl_relaxed(c->mux) & MUX_BUSY)
		cpu_relax();

	if (!up) {
		/* down: lower the rail only after the clock dropped */
		ret = s9p_acpm_pmic_write(c->buck_reg, step->vsel);
		if (ret) {
			pr_err_ratelimited("s9p-cpufreq: %s BUCK write for %u kHz failed (%d)\n",
					   c->name, step->freq_khz, ret);
			/* The clock did change. A failed voltage reduction leaves
			 * the higher, safe rail; reporting a failed clock transition
			 * would make cpufreq restore a frequency we no longer run. */
		}
	}

	mutex_unlock(&s9p_clk_lock);
	return 0;

err_unlock:
	mutex_unlock(&s9p_clk_lock);
	return ret;
}

static unsigned int s9p_big_busy_cores(void)
{
	unsigned int cpu, busy = 0;

	for_each_online_cpu(cpu)
		if (s9p_cluster_of(cpu) == 1 && !idle_cpu(cpu))
			busy++;
	return busy;
}

static unsigned int s9p_big_cap_for(unsigned int busy)
{
	return (busy >= 3) ? 1794000 : (busy == 2) ? 2327000 : 2704000;
}

static int s9p_target(struct cpufreq_policy *policy, unsigned int target,
		     unsigned int relation)
{
	struct s9p_cluster *c = policy->driver_data;
	unsigned int maximum = policy->max;
	struct cpufreq_freqs freqs = { .old = policy->cur };
	const struct s9p_pll_step *step;
	int index, ret;

	/*
	 * Samsung's busy-core policy for the M3 cluster, measured by the
	 * userspace governor: 2704 MHz with one busy core, 2327 MHz with
	 * two, 1794 MHz above that. More cores at the top steps hard-reset
	 * the SoC. The target callback owns transition notifications, allowing
	 * a last-moment safety cap to be reported with the ACTUAL frequency.
	 * target_index cannot do that: silent substitution lies to the core,
	 * while rejecting ordinary races repeatedly leaves schedutil retrying.
	 */
	if (c == &s9p_clusters[1])
		maximum = min(maximum, s9p_big_cap_for(s9p_big_busy_cores()));
	if (maximum < policy->min)
		return -EAGAIN;
	index = cpufreq_frequency_table_target(policy, min(target, maximum),
					      policy->min, maximum, relation);
	if (index < 0)
		return index;
	freqs.new = policy->freq_table[index].frequency;

	step = s9p_find_step(c, freqs.new);
	if (!step)
		return -EINVAL;
	if (s9p_current_khz(c) == freqs.new && policy->cur == freqs.new)
		return 0;
	cpufreq_freq_transition_begin(policy, &freqs);
	ret = s9p_set_step(c, step);
	cpufreq_freq_transition_end(policy, &freqs, ret != 0);
	return ret;
}

static unsigned int s9p_get(unsigned int cpu)
{
	struct s9p_cluster *c = &s9p_clusters[s9p_cluster_of(cpu)];

	if (!c->pll_con0)
		return 0;
	return s9p_current_khz(c);
}

static int s9p_init(struct cpufreq_policy *policy)
{
	struct s9p_cluster *c = &s9p_clusters[s9p_cluster_of(policy->cpu)];
	unsigned int cpu;
	int ret;

	if (!c->pll_con0)
		return -EPROBE_DEFER;

	policy->driver_data = c;
	policy->freq_table = c->freq_table;
	/* a transition costs ~0.5 ms (BUCK over ACPM, PLL relock); see above */
	policy->transition_delay_us = 5 * USEC_PER_MSEC;

	cpumask_clear(policy->cpus);
	for_each_possible_cpu(cpu)
		if (s9p_cluster_of(cpu) == s9p_cluster_of(policy->cpu))
			cpumask_set_cpu(cpu, policy->cpus);
	c->first_cpu = cpumask_first(policy->cpus);

	ret = freq_qos_add_request(&policy->constraints, &c->qos_thermal,
				   FREQ_QOS_MAX, FREQ_QOS_MAX_DEFAULT_VALUE);
	if (ret < 0)
		return ret;
	ret = freq_qos_add_request(&policy->constraints, &c->qos_busy,
				   FREQ_QOS_MAX, FREQ_QOS_MAX_DEFAULT_VALUE);
	if (ret < 0) {
		freq_qos_remove_request(&c->qos_thermal);
		return ret;
	}
	ret = freq_qos_add_request(&policy->constraints, &c->qos_boost,
				   FREQ_QOS_MAX, s9p_boost ?
				   FREQ_QOS_MAX_DEFAULT_VALUE : c->safe_max);
	if (ret < 0) {
		freq_qos_remove_request(&c->qos_busy);
		freq_qos_remove_request(&c->qos_thermal);
		return ret;
	}
	ret = freq_qos_add_request(&policy->constraints, &c->qos_idle,
				   FREQ_QOS_MAX, FREQ_QOS_MAX_DEFAULT_VALUE);
	if (ret < 0) {
		freq_qos_remove_request(&c->qos_boost);
		freq_qos_remove_request(&c->qos_busy);
		freq_qos_remove_request(&c->qos_thermal);
		return ret;
	}
	c->idle_polls = 0;
	c->idle_capped = false;
	c->qos_added = true;

	return 0;
}

static void s9p_exit(struct cpufreq_policy *policy)
{
	struct s9p_cluster *c = policy->driver_data;

	if (c->qos_added) {
		c->qos_added = false;
		freq_qos_remove_request(&c->qos_idle);
		freq_qos_remove_request(&c->qos_boost);
		freq_qos_remove_request(&c->qos_busy);
		freq_qos_remove_request(&c->qos_thermal);
	}
}

static struct cpufreq_driver s9p_cpufreq_driver = {
	.name = "s9p-cpufreq",
	.flags = CPUFREQ_NEED_INITIAL_FREQ_CHECK,
	.verify = cpufreq_generic_frequency_table_verify,
	.target = s9p_target,
	.get = s9p_get,
	.init = s9p_init,
	.exit = s9p_exit,
};

/* ------------------------------------------------------- TMU + policies */

/*
 * TMU blocks (vendor DT: BIG/LITTLE sensors at 0x10070000, G3D at
 * 0x10074000), read directly. TRIMINFO(s) at s*4 holds the two-point
 * calibration (e1 = [8:0] at 25 C, e2 = [17:9] at 85 C); CURRENT_TEMP0_1 at
 * 0x40 carries sensors 0 and 1, CURRENT_TEMP2_4 at 0x44 sensors 2..4,
 * CURRENT_TEMP5_7 at 0x48 sensors 5..7, nine bits each. Sensors 1,2,3,4,6
 * of the first block sit in the M3 cluster, sensor 5 in the A55 cluster,
 * sensor 1 of the G3D block on the GPU -- the mapping the userspace
 * governor validated against the measured heat-up of single cores.
 */
#define TMU_CPU_PHYS		0x10070000
#define TMU_G3D_PHYS		0x10074000
#define TMU_SIZE		0x100

static void __iomem *tmu_cpu;
static void __iomem *tmu_g3d;

static const u8 s9p_tmu_big[] = { 1, 2, 3, 4, 6 };
static const u8 s9p_tmu_little[] = { 5 };
static const u8 s9p_tmu_gpu[] = { 1 };

static int s9p_tmu_read(void __iomem *base, unsigned int s)
{
	u32 trim = readl_relaxed(base + s * 4);
	int e1 = trim & 0x1ff, e2 = (trim >> 9) & 0x1ff;
	unsigned int off, first;
	int code, t;

	if (s < 2) {
		off = 0x40;
		first = 0;
	} else if (s < 5) {
		off = 0x44;
		first = 2;
	} else {
		off = 0x48;
		first = 5;
	}
	code = (readl_relaxed(base + off) >> (9 * (s - first))) & 0x1ff;
	if (e2 > e1)
		t = (code - e1) * 60 / (e2 - e1) + 25;
	else
		t = code - e1 + 25;
	/* a sensor that is off or uncalibrated reads nonsense; ignore it */
	if (t < -20 || t > 150)
		return INT_MIN;
	return t;
}

static int s9p_tmu_max(void __iomem *base, const u8 *sensors, unsigned int n)
{
	int best = INT_MIN;
	unsigned int i;

	for (i = 0; i < n; i++)
		best = max(best, s9p_tmu_read(base, sensors[i]));
	return best;
}

/*
 * Median of the last three readings.
 *
 * The raw maximum over the big cluster's five sensors jumps: 63 C, then
 * 83 C a tenth of a second later, then 55 C -- measured on an idle device,
 * and no silicon cools 28 degrees in two seconds. A single bad sample was
 * enough to drop the cap two steps, and every cap change relocks the
 * cluster PLL, which parks all four cores on the 26 MHz oscillator while it
 * settles. Filtering costs two polling periods of reaction time on a real
 * temperature ramp and removes the flapping entirely.
 */
struct s9p_tmu_filter {
	int sample[3];
	unsigned int next;
	bool primed;
};

static int s9p_tmu_filtered(struct s9p_tmu_filter *f, int value)
{
	int a, b, c;

	if (value == INT_MIN)
		return INT_MIN;
	if (!f->primed) {
		f->sample[0] = f->sample[1] = f->sample[2] = value;
		f->primed = true;
	} else {
		f->sample[f->next] = value;
		f->next = (f->next + 1) % 3;
	}
	a = f->sample[0];
	b = f->sample[1];
	c = f->sample[2];
	return max(min(a, b), min(max(a, b), c));
}

static unsigned int s9p_thermal_next(unsigned int cur, int temp,
				     const struct s9p_thermal_level *lv,
				     unsigned int n, ktime_t *changed)
{
	unsigned int want = 0, i;

	if (temp == INT_MIN)
		return cur;
	for (i = 1; i < n; i++)
		if (temp >= lv[i].up_at)
			want = i;
	if (want > cur) {
		*changed = ktime_get();
		return want;
	}
	if (cur > 0 && temp < lv[cur].up_at - THERMAL_HYST &&
	    ktime_ms_delta(ktime_get(), *changed) >= THERMAL_HOLD_MS) {
		*changed = ktime_get();
		return cur - 1;
	}
	return cur;
}

/*
 * Hold a cluster at its lowest step once every core in it has been idle for
 * a while, and let go the moment one is not. Switchable, because it trades a
 * few tens of milliseconds on the first task after a quiet spell for the
 * voltage drop that goes with the lower step.
 *
 * Enabled on the recovered #150 and #155 kernels. The sampler runs on A55
 * CPU0 so that checking the M3 cluster does not itself prevent M3 idleness.
 * Runtime switch: /sys/module/s9p_cpufreq/parameters/idle_floor.
 *
 * Off by default since 2026-09-23: with it (and s9p_g3d.step_volt) on, #163
 * froze about once a day on an idle desktop -- seven hard hangs in a week,
 * the M3 cluster parked at 400 MHz / 700 mV, a step no load test covered.
 * The validation behind "on" was 30-120 s long.
 */
static bool s9p_idle_floor_on;
module_param_named(idle_floor, s9p_idle_floor_on, bool, 0644);
MODULE_PARM_DESC(idle_floor, "hold an idle cluster at its lowest step (and so its lowest voltage)");

static bool s9p_cluster_is_idle(unsigned int index)
{
	unsigned int cpu;

	for_each_online_cpu(cpu)
		if (s9p_cluster_of(cpu) == (int)index && !idle_cpu(cpu))
			return false;
	return true;
}

static void s9p_idle_floor(struct s9p_cluster *c, unsigned int index)
{
	unsigned int floor = c->steps[c->nsteps - 1].freq_khz;
	bool want;

	if (!c->qos_added)
		return;

	if (s9p_idle_floor_on && s9p_cluster_is_idle(index)) {
		if (c->idle_polls < IDLE_ENTER_POLLS)
			c->idle_polls++;
	} else {
		c->idle_polls = 0;
	}

	want = c->idle_polls >= IDLE_ENTER_POLLS;
	if (want == c->idle_capped)
		return;

	c->idle_capped = want;
	freq_qos_update_request(&c->qos_idle, want ? floor :
				FREQ_QOS_MAX_DEFAULT_VALUE);
}

static unsigned int gpu_lvl;
static ktime_t gpu_changed;

static void s9p_policy_work(struct work_struct *work);
static DECLARE_DELAYED_WORK(s9p_policy_dwork, s9p_policy_work);
static int s9p_temp_big, s9p_temp_little, s9p_temp_gpu;
module_param_named(temp_big, s9p_temp_big, int, 0444);
module_param_named(temp_little, s9p_temp_little, int, 0444);
module_param_named(temp_gpu, s9p_temp_gpu, int, 0444);

static void s9p_policy_work(struct work_struct *work)
{
	int t_big = INT_MIN, t_little = INT_MIN, t_gpu = INT_MIN;
	unsigned int i, lvl, cap;

	static struct s9p_tmu_filter filter_big, filter_little, filter_gpu;

	if (tmu_cpu) {
		t_big = s9p_tmu_filtered(&filter_big,
					 s9p_tmu_max(tmu_cpu, s9p_tmu_big,
						     ARRAY_SIZE(s9p_tmu_big)));
		t_little = s9p_tmu_filtered(&filter_little,
					    s9p_tmu_max(tmu_cpu, s9p_tmu_little,
							ARRAY_SIZE(s9p_tmu_little)));
	}
	if (tmu_g3d)
		t_gpu = s9p_tmu_filtered(&filter_gpu,
					 s9p_tmu_max(tmu_g3d, s9p_tmu_gpu,
						     ARRAY_SIZE(s9p_tmu_gpu)));
	s9p_temp_big = t_big;
	s9p_temp_little = t_little;
	s9p_temp_gpu = t_gpu;

	for (i = 0; i < ARRAY_SIZE(s9p_clusters); i++) {
		struct s9p_cluster *c = &s9p_clusters[i];
		int t = (i == 1) ? t_big : t_little;

		if (!c->qos_added)
			continue;

		lvl = s9p_thermal_next(c->thermal_lvl, t, c->thermal,
				       c->nthermal, &c->thermal_changed);
		if (lvl != c->thermal_lvl) {
			pr_info("s9p-cpufreq: %s %d C -> max %u MHz\n",
				c->name, t, c->thermal[lvl].max_khz / 1000);
			c->thermal_lvl = lvl;
			freq_qos_update_request(&c->qos_thermal,
						c->thermal[lvl].max_khz);
		}

		if (i == 1) {
			cap = s9p_big_cap_for(s9p_big_busy_cores());
			if (cap != c->busy_cap) {
				c->busy_cap = cap;
				freq_qos_update_request(&c->qos_busy, cap);
			}
		}

		s9p_idle_floor(c, i);
	}

	lvl = s9p_thermal_next(gpu_lvl, t_gpu, s9p_thermal_gpu,
			       ARRAY_SIZE(s9p_thermal_gpu), &gpu_changed);
	if (lvl != gpu_lvl) {
		if (s9p_g3d_set_khz(s9p_thermal_gpu[lvl].max_khz) == 0) {
			pr_info("s9p-cpufreq: gpu %d C -> %u MHz\n", t_gpu,
				s9p_thermal_gpu[lvl].max_khz / 1000);
			gpu_lvl = lvl;
		}
	}

	/*
	 * While a cluster is held at its floor the poll is what releases it
	 * again, so it has to be quick; otherwise the slow period is plenty.
	 */
	/* CPU0 is the always-online A55 housekeeping CPU on star2lte. Running
	 * this sampler on M3 wakes that cluster and counts the sampler itself
	 * as busy, defeating the idle floor it is supposed to manage. */
	schedule_delayed_work_on(0, &s9p_policy_dwork,
			      msecs_to_jiffies(s9p_clusters[0].idle_capped ||
					       s9p_clusters[1].idle_capped ?
					       IDLE_RELEASE_MS :
					       THERMAL_PERIOD_MS));
}

/* ---------------------------------------------------------------- probe */

static const struct resource s9p_cpufreq_res[] = {
	DEFINE_RES_MEM(0x1d000140, 0x20),	/* CPUCL0 PLL_CON0 */
	DEFINE_RES_MEM(0x1d00100c, 0x20),	/* CPUCL0 mux */
	DEFINE_RES_MEM(0x1d100120, 0x20),	/* CPUCL1 PLL_CON0 */
	DEFINE_RES_MEM(0x1d101000, 0x20),	/* CPUCL1 mux */
	DEFINE_RES_MEM(0x1d000000, 0x10),	/* CPUCL0 PLL_LOCKTIME */
	DEFINE_RES_MEM(0x1d100000, 0x10),	/* CPUCL1 PLL_LOCKTIME */
};

static int s9p_cpufreq_probe(struct platform_device *pdev)
{
	int ret;

	/* Initial frequency validation can change a rail during registration. */
	if (!s9p_acpm_is_ready())
		return -EPROBE_DEFER;

	s9p_clusters[0].pll_con0 = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(s9p_clusters[0].pll_con0))
		return PTR_ERR(s9p_clusters[0].pll_con0);
	s9p_clusters[0].mux = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(s9p_clusters[0].mux))
		return PTR_ERR(s9p_clusters[0].mux);
	s9p_clusters[1].pll_con0 = devm_platform_ioremap_resource(pdev, 2);
	if (IS_ERR(s9p_clusters[1].pll_con0))
		return PTR_ERR(s9p_clusters[1].pll_con0);
	s9p_clusters[1].mux = devm_platform_ioremap_resource(pdev, 3);
	if (IS_ERR(s9p_clusters[1].mux))
		return PTR_ERR(s9p_clusters[1].mux);
	s9p_clusters[0].locktime = devm_platform_ioremap_resource(pdev, 4);
	if (IS_ERR(s9p_clusters[0].locktime))
		return PTR_ERR(s9p_clusters[0].locktime);
	s9p_clusters[1].locktime = devm_platform_ioremap_resource(pdev, 5);
	if (IS_ERR(s9p_clusters[1].locktime))
		return PTR_ERR(s9p_clusters[1].locktime);

	tmu_cpu = devm_ioremap(&pdev->dev, TMU_CPU_PHYS, TMU_SIZE);
	tmu_g3d = devm_ioremap(&pdev->dev, TMU_G3D_PHYS, TMU_SIZE);
	if (!tmu_cpu || !tmu_g3d)
		dev_warn(&pdev->dev, "TMU not mapped, no thermal cap\n");

	ret = cpufreq_register_driver(&s9p_cpufreq_driver);
	if (ret)
		return ret;

	schedule_delayed_work_on(0, &s9p_policy_dwork, HZ);
	return 0;
}

static struct platform_driver s9p_cpufreq_plat = {
	.probe = s9p_cpufreq_probe,
	.driver = { .name = "s9p-cpufreq" },
};

static struct platform_device *s9p_cpufreq_pdev;

static int __init s9p_cpufreq_modinit(void)
{
	int ret;

	if (!of_machine_is_compatible("samsung,exynos9810"))
		return -ENODEV;
	if (s9p_cpufreq_pdev)
		return 0;
	ret = platform_driver_register(&s9p_cpufreq_plat);
	if (ret)
		return ret;
	s9p_cpufreq_pdev = platform_device_register_simple("s9p-cpufreq", -1,
							   s9p_cpufreq_res,
							   ARRAY_SIZE(s9p_cpufreq_res));
	if (IS_ERR(s9p_cpufreq_pdev)) {
		platform_driver_unregister(&s9p_cpufreq_plat);
		return PTR_ERR(s9p_cpufreq_pdev);
	}
	return 0;
}
device_initcall(s9p_cpufreq_modinit);

MODULE_DESCRIPTION("cpufreq for Exynos 9810 (PLL + ACPM BUCK, vendor tables, TMU cap)");
MODULE_LICENSE("GPL v2");
