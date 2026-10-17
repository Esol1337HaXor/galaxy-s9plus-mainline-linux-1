// SPDX-License-Identifier: GPL-2.0
/*
 * CP (Shannon modem) power and reset control of the Exynos 9810, in place of
 * the vendor's pmu-cp.c + cal-if/pmucal_cp.c + pmucal_rae.c.
 *
 * The sequences are pmucal_cp_exynos9810.h, unchanged. As in the vendor
 * pmucal_rae, CP_CTRL_NS (PMU + 0x30) and CP_CTRL_S (PMU + 0x34) are owned by
 * EL3: they are read and written through SMC 0x82000700 (READ_CTRL returns
 * one 16-bit half per call, status in the low half). Every other register is
 * an ordinary MMIO access.
 */
#include <linux/arm-smccc.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include <soc/samsung/exynos-pmu.h>

#include "pmucal_cp.h"

#define PMU_PHYS		0x14060000
#define PMU_SIZE		0x10000
#define GICD_PHYS		0x10100000
#define GICD_SIZE		0x2000

#define CP_CTRL_NS_OFF		0x30
#define CP_CTRL_S_OFF		0x34
#define SMC_CP_CTRL		0x82000700
#define READ_CTRL		0x3
#define WRITE_CTRL		0x4
enum { SMC_REG_CP_CTRL_S, SMC_REG_CP_CTRL_NS };

#define WAIT_TIMEOUT_US		100000

#include "pmucal_cp_exynos9810.h"

static void __iomem *pmu;
static void __iomem *gicd;
static DEFINE_SPINLOCK(cp_lock);

static int cp_smc_reg(u32 off)
{
	return off == CP_CTRL_S_OFF ? SMC_REG_CP_CTRL_S : SMC_REG_CP_CTRL_NS;
}

static int cp_smc_read(u32 off, u32 *val)
{
	struct arm_smccc_res lo, hi;

	arm_smccc_smc(SMC_CP_CTRL, READ_CTRL, 0, cp_smc_reg(off), 0, 0, 0, 0, &lo);
	arm_smccc_smc(SMC_CP_CTRL, READ_CTRL, 1, cp_smc_reg(off), 0, 0, 0, 0, &hi);
	if ((lo.a0 & 0xffff) || (hi.a0 & 0xffff)) {
		pr_err("s9p-cp: SMC read of CP_CTRL %#x failed: %lx/%lx\n", off,
		       lo.a0 & 0xffff, hi.a0 & 0xffff);
		return -EIO;
	}
	*val = ((u32)(hi.a0 >> 16) << 16) | (u32)((lo.a0 >> 16) & 0xffff);
	return 0;
}

static int cp_smc_write(u32 off, u32 val)
{
	struct arm_smccc_res r;

	arm_smccc_smc(SMC_CP_CTRL, WRITE_CTRL, val, cp_smc_reg(off), 0, 0, 0, 0, &r);
	if ((long)r.a0 > 0) {
		pr_err("s9p-cp: SMC write of CP_CTRL %#x = %#x failed: %ld\n", off, val, (long)r.a0);
		return -EIO;
	}
	return 0;
}

static void __iomem *seq_va(const struct pmucal_seq *s, bool cond)
{
	u32 pa = cond ? s->cond_base_pa : s->base_pa;
	u32 off = cond ? s->cond_offset : s->offset;

	if (pa == PMU_PHYS && off < PMU_SIZE)
		return pmu + off;
	if (pa == GICD_PHYS && off < GICD_SIZE)
		return gicd + off;
	return NULL;
}

static bool is_cp_ctrl(const struct pmucal_seq *s)
{
	return s->base_pa == PMU_PHYS &&
	       (s->offset == CP_CTRL_NS_OFF || s->offset == CP_CTRL_S_OFF);
}

static int run_seq(struct pmucal_seq *seq, unsigned int n)
{
	unsigned int i;
	void __iomem *va;
	u32 v;
	int ret;

	for (i = 0; i < n; i++) {
		struct pmucal_seq *s = &seq[i];

		switch (s->access_type) {
		case PMUCAL_DELAY:
			udelay(s->value);
			break;
		case PMUCAL_READ:
			if (is_cp_ctrl(s)) {
				ret = cp_smc_read(s->offset, &v);
				if (ret)
					return ret;
			} else {
				va = seq_va(s, false);
				if (!va)
					return -EINVAL;
				v = readl(va);
			}
			s->value = v & s->mask;
			break;
		case PMUCAL_WRITE:
			if (is_cp_ctrl(s)) {
				if (s->mask == U32_MAX) {
					v = s->value;
				} else {
					ret = cp_smc_read(s->offset, &v);
					if (ret)
						return ret;
					v = (v & ~s->mask) | s->value;
				}
				ret = cp_smc_write(s->offset, v);
				if (ret)
					return ret;
			} else {
				va = seq_va(s, false);
				if (!va)
					return -EINVAL;
				writel((readl(va) & ~s->mask) | s->value, va);
			}
			pr_info("s9p-cp: %s <- %08x\n", s->sfr_name, s->value);
			break;
		case PMUCAL_WAIT:
			va = seq_va(s, false);
			if (!va)
				return -EINVAL;
			ret = readl_poll_timeout_atomic(va, v, (v & s->mask) == s->value,
							10, WAIT_TIMEOUT_US);
			if (ret) {
				pr_err("s9p-cp: %s: timeout, %08x & %08x != %08x\n",
				       s->sfr_name, v, s->mask, s->value);
				return ret;
			}
			break;
		default:
			pr_err("s9p-cp: %s: access type %d not handled\n", s->sfr_name,
			       s->access_type);
			return -EINVAL;
		}
	}
	return 0;
}

static int run(struct pmucal_seq *seq, unsigned int n, const char *what)
{
	unsigned long flags;
	int ret;

	if (!pmu || !gicd)
		return -ENODEV;
	spin_lock_irqsave(&cp_lock, flags);
	ret = run_seq(seq, n);
	spin_unlock_irqrestore(&cp_lock, flags);
	pr_info("s9p-cp: %s: %d\n", what, ret);
	return ret;
}

int cal_cp_init(void)
{
	return run(cp_init, ARRAY_SIZE(cp_init), "init");
}

/* 1: CP up (RESET_SEQUENCER_STATUS == 5, as pmucal_cp_status) */
int cal_cp_status(void)
{
	if (run_seq(cp_status, ARRAY_SIZE(cp_status)))
		return 0;
	return cp_status[0].value == 0x5;
}

int cal_cp_reset_assert(void)
{
	return run(cp_reset_assert, ARRAY_SIZE(cp_reset_assert), "reset assert");
}

int cal_cp_reset_release(void)
{
	return run(cp_reset_release, ARRAY_SIZE(cp_reset_release), "reset release");
}

void cal_cp_active_clear(void)
{
	run(cp_cp_active_clear, ARRAY_SIZE(cp_cp_active_clear), "active clear");
}

void cal_cp_reset_req_clear(void)
{
	run(cp_cp_reset_req_clear, ARRAY_SIZE(cp_cp_reset_req_clear), "reset request clear");
}

void cal_cp_enable_dump_pc_no_pg(void)
{
	run(cp_enable_dump_pc_no_pg, ARRAY_SIZE(cp_enable_dump_pc_no_pg), "dump pc on");
}

void cal_cp_disable_dump_pc_no_pg(void)
{
	run(cp_disable_dump_pc_no_pg, ARRAY_SIZE(cp_disable_dump_pc_no_pg), "dump pc off");
}

/* plain PMU access for the rest of the vendor code */
int exynos_pmu_read(unsigned int offset, unsigned int *val)
{
	if (!pmu || offset >= PMU_SIZE)
		return -EINVAL;
	if (offset == CP_CTRL_NS_OFF || offset == CP_CTRL_S_OFF)
		return cp_smc_read(offset, val);
	*val = readl(pmu + offset);
	return 0;
}

int exynos_pmu_write(unsigned int offset, unsigned int val)
{
	if (!pmu || offset >= PMU_SIZE)
		return -EINVAL;
	if (offset == CP_CTRL_NS_OFF || offset == CP_CTRL_S_OFF)
		return cp_smc_write(offset, val);
	writel(val, pmu + offset);
	return 0;
}

int exynos_pmu_update(unsigned int offset, unsigned int mask, unsigned int val)
{
	unsigned int v;
	int ret = exynos_pmu_read(offset, &v);

	return ret ?: exynos_pmu_write(offset, (v & ~mask) | (val & mask));
}

int s9p_cp_pmu_init(void)
{
	u32 ns = 0, s = 0;

	pmu = ioremap(PMU_PHYS, PMU_SIZE);
	gicd = ioremap(GICD_PHYS, GICD_SIZE);
	if (!pmu || !gicd)
		return -ENOMEM;
	cp_smc_read(CP_CTRL_NS_OFF, &ns);
	cp_smc_read(CP_CTRL_S_OFF, &s);
	pr_info("s9p-cp: CP_CTRL_NS %08x CP_CTRL_S %08x RESET_SEQUENCER_STATUS %08x\n",
		ns, s, readl(pmu + 0x504));
	return 0;
}

void s9p_cp_pmu_exit(void)
{
	if (pmu)
		iounmap(pmu);
	if (gicd)
		iounmap(gicd);
	pmu = gicd = NULL;
}
