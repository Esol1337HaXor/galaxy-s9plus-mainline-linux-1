// SPDX-License-Identifier: GPL-2.0
/*
 * Galaxy S9+ (exynos9810): power off the blocks nothing in this port uses.
 *
 * sboot leaves every PMU power domain on. The vendor kernel switches the
 * unused ones off at boot: pmucal_lpm_init powers down DCRD, DCPOST and DCF,
 * and its power-domain driver drops the rest (camera ISP, IVA, SCORE DSP,
 * G2D, MFC) through runtime PM once no device holds them. This port has no
 * driver for any of those blocks, so they stayed on for nothing -- a powered
 * domain leaks even with its clocks gated.
 *
 * The sequence is the vendor's (flexpmu_cal_local_exynos9810.h, xxx_off):
 * MEMORY_xxx_OPTION = 2 from pmucal_lpm_init, the TrustZone protection save
 * through EL3 for the domains whose vendor node has "need_smc"
 * (SMC 0x82000410, GET_IN_PD_DOWN, the TZPC address), then CONFIGURATION = 0
 * and STATUS polled to 0. Children before parents (ISPLP -> ISPHQ -> ISPPRE,
 * DSPS -> DSPM, DCPOST -> DCF). A domain that does not reach 0 is put back on
 * (and its protection restored) rather than left half off.
 *
 *   /sys/module/s9p_pd_off/parameters/domains   which ones (at load)
 *   /sys/module/s9p_pd_off/parameters/power_on  write a name: switch it back
 *                                                on, e.g. for camera bring-up
 *   /sys/module/s9p_pd_off/parameters/state     the PMU status of each one
 */

#include <linux/arm-smccc.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/string.h>

#define PMU_BASE		0x14060000
#define PMU_SIZE		0x5000
#define SMC_CMD_PREPARE_PD_ONOFF 0x82000410
#define EXYNOS_GET_IN_PD_DOWN	0
#define EXYNOS_WAKEUP_PD_DOWN	1

struct s9p_pd {
	const char *name;
	u16 cfg;		/* CONFIGURATION; STATUS at +4 */
	u16 mem_opt;		/* MEMORY_xxx_OPTION */
	u32 tzpc;		/* vendor "need_smc"; 0: the vendor never saves it */
	const char *parent;	/* vendor "parent", within this list */
	bool off;
};

/* in power-off order */
static struct s9p_pd pds[] = {
	{ "isplp",  0x4058, 0x4870, 0x16410204, "isphq" },
	{ "isphq",  0x4050, 0x47d0, 0x16610204, "isppre" },
	{ "isppre", 0x4048, 0x4730, 0x16210204 },
	{ "dsps",   0x4030, 0x4544, 0x16f10204, "dspm" },
	{ "dspm",   0x4028, 0x44a4, 0x16c10204 },
	/* lpm_init powers these three down without a protection save */
	{ "dcrd",   0x4018, 0x4364, 0 },
	{ "dcpost", 0x4010, 0x42c4, 0, "dcf" },
	{ "dcf",    0x4008, 0x4224, 0 },
	{ "g2d",    0x4038, 0x45e4, 0x17610204 },
	{ "iva",    0x4060, 0x4910, 0x17010204 },
	{ "mfc",    0x4068, 0x49b0, 0x17810204 },
};

static char domains[128] = "isplp,isphq,isppre,dsps,dspm,dcrd,dcpost,dcf,g2d,iva,mfc";
module_param_string(domains, domains, sizeof(domains), 0444);
MODULE_PARM_DESC(domains, "domains to power off at load, comma separated");

static void __iomem *pmu;
static DEFINE_MUTEX(pd_lock);

static long tz(u32 op, u32 addr)
{
	struct arm_smccc_res res = {};

	arm_smccc_smc(SMC_CMD_PREPARE_PD_ONOFF, op, addr, 0, 0, 0, 0, 0, &res);
	return res.a0;
}

static int pd_set(struct s9p_pd *d, bool on)
{
	u32 want = on ? 0xf : 0, v;
	int ret;

	if (((readl(pmu + d->cfg + 4) & 0xf) == want))
		return 0;
	if (!on) {
		writel((readl(pmu + d->mem_opt) & ~0x3) | 0x2, pmu + d->mem_opt);
		if (d->tzpc && tz(EXYNOS_GET_IN_PD_DOWN, d->tzpc)) {
			pr_err("s9p-pd-off: %s: protection save refused\n", d->name);
			return -EIO;
		}
	}
	writel((readl(pmu + d->cfg) & ~0xf) | want, pmu + d->cfg);
	ret = readl_poll_timeout(pmu + d->cfg + 4, v, (v & 0xf) == want, 10, 10000);
	if (ret && !on) {
		pr_err("s9p-pd-off: %s: power-down stopped at %08x, back on\n", d->name, v);
		on = true;
		writel((readl(pmu + d->cfg) & ~0xf) | 0xf, pmu + d->cfg);
		ret = readl_poll_timeout(pmu + d->cfg + 4, v, (v & 0xf) == 0xf, 10, 10000);
		if (ret)
			panic("s9p-pd-off: %s left partially powered (%08x)", d->name, v);
		ret = -ETIMEDOUT;
	}
	if (on && d->tzpc && tz(EXYNOS_WAKEUP_PD_DOWN, d->tzpc))
		pr_err("s9p-pd-off: %s: protection restore refused\n", d->name);
	d->off = !on;
	return ret;
}

static struct s9p_pd *pd_find(const char *name, size_t len)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(pds); i++)
		if (strlen(pds[i].name) == len && !strncmp(pds[i].name, name, len))
			return &pds[i];
	return NULL;
}

/* switch a domain on, its parents first */
static int pd_on(struct s9p_pd *d)
{
	struct s9p_pd *parent = d->parent ? pd_find(d->parent, strlen(d->parent)) : NULL;
	int ret = parent ? pd_on(parent) : 0;

	return ret ?: pd_set(d, true);
}

static int power_on_set(const char *val, const struct kernel_param *kp)
{
	struct s9p_pd *d = pd_find(val, strcspn(val, "\n"));
	int ret;

	if (!d)
		return -EINVAL;
	mutex_lock(&pd_lock);
	ret = pd_on(d);
	mutex_unlock(&pd_lock);
	pr_info("s9p-pd-off: %s on: %d\n", d->name, ret);
	return ret;
}

static int state_get(char *buf, const struct kernel_param *kp)
{
	int i, n = 0;

	for (i = 0; i < ARRAY_SIZE(pds); i++)
		n += sysfs_emit_at(buf, n, "%s %s (%08x)\n", pds[i].name,
				   (readl(pmu + pds[i].cfg + 4) & 0xf) ? "on" : "off",
				   readl(pmu + pds[i].cfg + 4));
	return n;
}

static const struct kernel_param_ops power_on_ops = { .set = power_on_set };
static const struct kernel_param_ops state_ops = { .get = state_get };
module_param_cb(power_on, &power_on_ops, NULL, 0200);
module_param_cb(state, &state_ops, NULL, 0444);

static int __init s9p_pd_off_init(void)
{
	int i, n = 0;

	if (!of_machine_is_compatible("samsung,exynos9810"))
		return -ENODEV;
	pmu = ioremap(PMU_BASE, PMU_SIZE);
	if (!pmu)
		return -ENOMEM;

	mutex_lock(&pd_lock);
	for (i = 0; i < ARRAY_SIZE(pds); i++) {
		const char *p = domains;
		size_t len = strlen(pds[i].name);
		bool want = false;

		while (*p) {
			size_t l = strcspn(p, ",\n");

			if (l == len && !strncmp(p, pds[i].name, len))
				want = true;
			p += l;
			p += strspn(p, ",\n ");
		}
		if (want && !pd_set(&pds[i], false))
			n++;
	}
	mutex_unlock(&pd_lock);
	pr_info("s9p-pd-off: %d unused power domains off\n", n);
	return 0;
}

static void __exit s9p_pd_off_exit(void)
{
	int i;

	/* back as sboot left them: parents first */
	mutex_lock(&pd_lock);
	for (i = ARRAY_SIZE(pds) - 1; i >= 0; i--)
		if (pds[i].off)
			pd_set(&pds[i], true);
	mutex_unlock(&pd_lock);
	iounmap(pmu);
}

module_init(s9p_pd_off_init);
module_exit(s9p_pd_off_exit);
MODULE_DESCRIPTION("Galaxy S9+: power off the PMU domains no driver of this port uses");
MODULE_LICENSE("GPL");
