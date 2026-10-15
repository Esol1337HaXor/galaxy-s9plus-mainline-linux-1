// SPDX-License-Identifier: GPL-2.0
/*
 * Galaxy S9+ (exynos9810): memory (MIF) frequency scaling.
 *
 * sboot leaves the memory interface at its boot level, 1794 MHz (LPDDR4X at
 * 3588 Mbps), and the port never changed it. The vendor kernel scales it
 * through the APM firmware: exynos_acpm_set_rate(dvfs_mif = 0, kHz) with
 * cmd = { domain, kHz, FREQ_REQ, 0 }. Its acpm_dvfs device is the CMU node,
 * which has no "acpm-ipc-channel", so acpm_dvfs.ch_num stays 0 and every
 * DVFS request goes out on ACPM channel 0 -- the channel this module uses.
 * The APM does the whole DRAM switch itself (PLL_MIF, or the shared PLLs
 * below 1014 MHz, and the DREX/PHY timing). Measured on the device: 3917
 * random level changes under three memory-copy and one CPU stress jobs, no
 * wrong digest, no lost answer.
 *
 * Policy, every 50 ms: the level the busiest client asks for, up at once,
 * down only after a second of lower demand.
 *
 *   CPU busy time (all CPUs): 2 CPUs' worth -> 1794, 1 -> 1539, 1/4 -> 1014
 *   GPU: 572 MHz -> 1794, 455 -> 1352
 *   USB-C monitor running (DECON2) -> at least display_khz
 *   otherwise min_khz
 *
 * Idle saving measured at the battery: 165 mW at 421 MHz against 1794 MHz.
 * Before a reboot the memory goes back to 1794 MHz, the level sboot and TWRP
 * expect to find.
 *
 *   /sys/module/s9p_mif/parameters/enable      0: hold max_khz
 *   /sys/module/s9p_mif/parameters/min_khz     floor (default 421000)
 *   /sys/module/s9p_mif/parameters/max_khz     ceiling (default 1794000)
 *   /sys/module/s9p_mif/parameters/cur_khz     last level set
 *   /sys/module/s9p_mif/parameters/changes     level changes so far
 */

#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/reboot.h>
#include <linux/tick.h>
#include <linux/workqueue.h>

unsigned int s9p_g3d_get_khz(void);

#define MBOX_PHYS	0x14100000
#define SRAM_PHYS	0x02039000
#define SRAM_SIZE	0x30000
#define SRAM_MAP_OFF	0x3000		/* the first pages fault */
#define INITDATA	0x7f00
#define INTGR0		0x08
#define INTCR1		0x20
#define SEQ_SHIFT	16
#define DVFS_CHANNEL	0
#define DVFS_MIF	0
#define FREQ_REQ	0
#define DECON2_PHYS	0x16050000
#define DECON_RUN	BIT(4)

#define MIF_MAX		1794000

static const unsigned int levels[] = {
	1794000, 1539000, 1352000, 1014000, 845000, 676000, 546000, 421000,
};

static bool enable = true;
static unsigned int min_khz = 421000, max_khz = MIF_MAX, display_khz = 676000;
static unsigned int cur_khz, changes;
static char why[64];
module_param_string(why, why, sizeof(why), 0444);
MODULE_PARM_DESC(why, "inputs of the last decision");
module_param(enable, bool, 0644);
module_param(min_khz, uint, 0644);
module_param(max_khz, uint, 0644);
module_param(display_khz, uint, 0644);
MODULE_PARM_DESC(display_khz, "floor while the USB-C monitor is scanned out");
module_param(cur_khz, uint, 0444);
module_param(changes, uint, 0444);

static void __iomem *mbox, *sram, *decon2;
static DEFINE_MUTEX(lock);
static u32 tx_rear, tx_front, tx_base, rx_rear, rx_front, rx_base, q_len, q_elem;
static u32 seq = 1;
static bool stopped;

static u32 sr(u32 off)
{
	return readl_relaxed(sram + off - SRAM_MAP_OFF);
}

static void sw(u32 off, u32 v)
{
	writel_relaxed(v, sram + off - SRAM_MAP_OFF);
}

/* struct ipc_channel: id, field, owner, type, then channel_info (see s9p-acpm) */
static int find_channel(void)
{
	u32 table = sr(INITDATA + 2 * 4), n = sr(INITDATA + 6 * 4), i;

	for (i = 0; i < n; i++) {
		u32 b = table + i * 18 * 4;

		if ((sr(b) & 0xff) != DVFS_CHANNEL)
			continue;
		/* the AP sends into the APM's rx ring and reads its tx ring */
		tx_rear = sr(b + 4 * 4);
		tx_front = sr(b + 5 * 4);
		tx_base = sr(b + 6 * 4);
		rx_rear = sr(b + 10 * 4);
		rx_front = sr(b + 11 * 4);
		rx_base = sr(b + 12 * 4);
		q_len = sr(b + 13 * 4);
		q_elem = sr(b + 14 * 4);
		return q_len >= 2 && q_elem >= 16 ? 0 : -EINVAL;
	}
	return -ENODEV;
}

/* empty the reply ring; return the answer to @want if it is there */
static bool drain(u32 want, u32 *out)
{
	u32 front = sr(rx_front), i, k;
	bool found = false;

	for (i = sr(rx_rear); i != front; i = (i + 1) % q_len) {
		if (((sr(rx_base + q_elem * i) >> SEQ_SHIFT) & 0x3f) == want) {
			for (k = 0; k < 4; k++)
				out[k] = sr(rx_base + q_elem * i + 4 * k);
			found = true;
		}
	}
	sw(rx_rear, front);
	writel_relaxed(1 << DVFS_CHANNEL, mbox + INTCR1);
	return found;
}

static int acpm_xfer(u32 *cmd)
{
	u32 front, nxt, reply[4], dummy[4], i;
	ktime_t t0;

	drain(0, dummy);
	seq = seq % 63 + 1;
	cmd[0] = (cmd[0] & ~(0x3f << SEQ_SHIFT)) | (seq << SEQ_SHIFT);

	front = sr(tx_front);
	nxt = (front + 1) % q_len;
	t0 = ktime_get();
	while (nxt == sr(tx_rear)) {
		if (ktime_ms_delta(ktime_get(), t0) > 20)
			return -EBUSY;
		usleep_range(50, 100);
	}
	for (i = 0; i < 4; i++)
		sw(tx_base + q_elem * front + 4 * i, cmd[i]);
	wmb();
	sw(tx_front, nxt);
	wmb();
	writel_relaxed((1 << DVFS_CHANNEL) << 16, mbox + INTGR0);

	t0 = ktime_get();
	while (ktime_ms_delta(ktime_get(), t0) < 100) {
		if (sr(rx_rear) != sr(rx_front) && drain(seq, reply)) {
			memcpy(cmd, reply, sizeof(reply));
			return 0;
		}
		usleep_range(20, 50);
	}
	return -ETIMEDOUT;
}

static int mif_set(unsigned int khz)
{
	u32 cmd[4] = { DVFS_MIF, khz, FREQ_REQ, 0 };
	int ret;

	mutex_lock(&lock);
	if (stopped && khz != MIF_MAX) {
		mutex_unlock(&lock);
		return -ESHUTDOWN;
	}
	ret = acpm_xfer(cmd);
	if (!ret && cmd[1] != khz)
		ret = -EIO;
	if (!ret) {
		cur_khz = khz;
		changes++;
	}
	mutex_unlock(&lock);
	if (ret)
		pr_warn_ratelimited("s9p-mif: %u kHz: %d\n", khz, ret);
	return ret;
}

/* the lowest table level at or above @khz */
static unsigned int level_for(unsigned int khz)
{
	int i;

	for (i = ARRAY_SIZE(levels) - 1; i >= 0; i--)
		if (levels[i] >= khz)
			return levels[i];
	return levels[0];
}

/*
 * CPU demand is the busy time of all CPUs over the last two periods, not their
 * clock: schedutil lifts the clusters for a few milliseconds whenever
 * anything runs (on an idle desktop the M3 cluster touches 2327 MHz several
 * times a second, the A55 cluster sits at 1150-1499 MHz), and following the
 * clock kept the memory near its top level for nothing. In CPUs' worth of
 * busy time: under 0.25 nothing, then 1014, 1539 and from 2 CPUs 1794 MHz.
 */
static u64 last_idle[NR_CPUS], last_wall[NR_CPUS];

static unsigned int busy_milli(void)
{
	unsigned int cpu, busy = 0;

	for_each_online_cpu(cpu) {
		u64 wall, idle = get_cpu_idle_time_us(cpu, &wall);
		u64 dw = wall - last_wall[cpu], di = idle - last_idle[cpu];

		if (last_wall[cpu] && dw)
			busy += 1000 - min_t(u64, 1000, div64_u64(di * 1000, dw));
		last_wall[cpu] = wall;
		last_idle[cpu] = idle;
	}
	return busy;
}

static unsigned int demand(void)
{
	static unsigned int prev_busy;
	unsigned int now = busy_milli(), gpu = s9p_g3d_get_khz(), want = min_khz;
	/* two periods (100 ms): a process start alone does not move the memory */
	unsigned int busy = min(now, prev_busy);
	bool disp = readl_relaxed(decon2) & DECON_RUN;

	if (busy >= 2000)
		want = max(want, 1794000u);
	else if (busy >= 1000)
		want = max(want, 1539000u);
	else if (busy >= 250)
		want = max(want, 1014000u);
	if (gpu >= 572000)
		want = max(want, 1794000u);
	else if (gpu >= 455000)
		want = max(want, 1352000u);
	if (disp)
		want = max(want, display_khz);
	prev_busy = now;
	snprintf(why, sizeof(why), "busy %u gpu %u disp %d", busy, gpu, disp);
	return level_for(min(want, max_khz));
}

#define PERIOD_MS	50
#define DOWN_HOLD_MS	1000

static void mif_work(struct work_struct *w);
static DECLARE_DELAYED_WORK(mif_dwork, mif_work);
static ktime_t last_up;

static void mif_work(struct work_struct *w)
{
	unsigned int want = enable ? demand() : level_for(max_khz);

	if (want > cur_khz) {
		mif_set(want);
		last_up = ktime_get();
	} else if (want < cur_khz &&
		   ktime_ms_delta(ktime_get(), last_up) >= DOWN_HOLD_MS) {
		mif_set(want);
	} else if (want == cur_khz) {
		last_up = ktime_get();
	}
	if (!stopped)
		schedule_delayed_work_on(0, &mif_dwork, msecs_to_jiffies(PERIOD_MS));
}

static int mif_reboot(struct notifier_block *nb, unsigned long action, void *data)
{
	stopped = true;
	cancel_delayed_work_sync(&mif_dwork);
	mif_set(MIF_MAX);
	return NOTIFY_DONE;
}

static struct notifier_block reboot_nb = {
	.notifier_call = mif_reboot,
	.priority = 128,
};

static int __init s9p_mif_init(void)
{
	int ret;

	if (!of_machine_is_compatible("samsung,exynos9810"))
		return -ENODEV;
	mbox = ioremap(MBOX_PHYS, 0x1000);
	sram = ioremap(SRAM_PHYS + SRAM_MAP_OFF, SRAM_SIZE - SRAM_MAP_OFF);
	decon2 = ioremap(DECON2_PHYS, 0x1000);
	ret = -ENOMEM;
	if (!mbox || !sram || !decon2)
		goto err;
	ret = find_channel();
	if (ret) {
		pr_err("s9p-mif: no ACPM channel %d\n", DVFS_CHANNEL);
		goto err;
	}
	/* sboot's level; the first answer confirms the firmware listens */
	cur_khz = MIF_MAX;
	ret = mif_set(MIF_MAX);
	if (ret)
		goto err;
	register_reboot_notifier(&reboot_nb);
	last_up = ktime_get();
	schedule_delayed_work_on(0, &mif_dwork, HZ);
	pr_info("s9p-mif: ACPM channel %d (q_len %u), %u..%u kHz\n",
		DVFS_CHANNEL, q_len, min_khz, max_khz);
	return 0;
err:
	if (decon2)
		iounmap(decon2);
	if (sram)
		iounmap(sram);
	if (mbox)
		iounmap(mbox);
	return ret;
}

static void __exit s9p_mif_exit(void)
{
	unregister_reboot_notifier(&reboot_nb);
	stopped = true;
	cancel_delayed_work_sync(&mif_dwork);
	mif_set(MIF_MAX);
	iounmap(decon2);
	iounmap(sram);
	iounmap(mbox);
}

module_init(s9p_mif_init);
module_exit(s9p_mif_exit);
MODULE_DESCRIPTION("Galaxy S9+ memory (MIF) frequency scaling through the APM firmware");
MODULE_LICENSE("GPL");
