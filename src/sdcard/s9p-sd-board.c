// SPDX-License-Identifier: GPL-2.0
/*
 * Galaxy S9+ (exynos9810): microSD slot.
 *
 * The slot is DWMMC2 (0x11500000, SPI 249), which the upstream dw_mmc-exynos
 * driver handles as "samsung,exynos7-dw-mshc" (64-bit IDMAC, CLKSEL at 0xa8).
 * The port's device tree has neither the controller nor anything it needs, so
 * this module provides all of it before adding the node with an OF changeset:
 *
 *  - the card clock: CMU_TOP MUX_CLKCMU_FSYS1_MMC_CARD on PLL_SHARED2 (800 MHz)
 *    and its 9-bit divider registered as a real clock, so that the driver can
 *    ask for 200 MHz (SD high speed) or 800 MHz (SDR104) as the vendor does;
 *    the FSYS1 user mux and the clock gates are opened by hand;
 *  - the two supplies as regulators over ACPM: S2MPS18 LDO31 "vmmc" (card,
 *    2.8 V fixed) and LDO2 "vqmmc" (I/O, 1.8-3.3 V), so that the MMC core can
 *    power-cycle the card and switch the I/O to 1.8 V for UHS-I;
 *  - the pins: gpf1-0..5 as SD2 (vendor pull and drive strength), card detect
 *    on gpa1-5 (active low, measured: reads 0 with a card in).
 *
 * Values from the vendor DT (dwmmc2@11500000, LDO2/LDO31) and CMU tables:
 * docs/14-microsd.md.
 */

#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>
#include <linux/slab.h>

int s9p_acpm_pmic_read(u8 reg, u8 *val);
int s9p_acpm_pmic_update(u8 reg, u8 val, u8 mask);
bool s9p_acpm_is_ready(void);

static bool uhs = true;
module_param(uhs, bool, 0444);
MODULE_PARM_DESC(uhs, "offer UHS-I (SDR50/SDR104 at 1.8 V); 0: high speed at 3.3 V only");

#define MUX_TOP_MMC	0x1a24106c	/* [2:0] 1 = PLL_SHARED2, busy bit 16 */
#define DIV_TOP_MMC	0x1a241864	/* [8:0] ratio - 1, busy bit 16 */
#define GAT_TOP_MMC	0x1a242070
#define MUX_USER_MMC	0x11400120	/* bit 4: 1 = CLKCMU, busy bit 7 */
#define GOUT_MMC_ACLK	0x11402030
#define GOUT_MMC_SDCLK	0x11402034
#define GPF1		0x11430000	/* pinctrl@11430000, bank at +0 */
#define PLL_SHARED2_HZ	800000000UL

#define L2CTRL1		0x4a		/* vqmmc */
#define L31CTRL		0x68		/* vmmc */
#define LDO_EN		0xc0
#define LDO_VSEL	0x3f

static void __iomem *map(phys_addr_t a)
{
	return ioremap(a & ~0xfffUL, 0x1000) + (a & 0xfff);
}

static void unmap(void __iomem *p)
{
	iounmap((void __iomem *)((unsigned long)p & ~0xfffUL));
}

static u32 rd(phys_addr_t a)
{
	void __iomem *p = map(a);
	u32 v = readl(p);

	unmap(p);
	return v;
}

static void rmw(phys_addr_t a, u32 clr, u32 set)
{
	void __iomem *p = map(a);

	writel((readl(p) & ~clr) | set, p);
	unmap(p);
}

/* wait for a CMU busy bit; the switch takes a few cycles of the slower clock */
static int busy_wait(phys_addr_t a, u32 bit)
{
	void __iomem *p = map(a);
	u32 v;
	int ret = readl_poll_timeout_atomic(p, v, !(v & bit), 1, 1000);

	unmap(p);
	return ret;
}

/* ------------------------------------------------------------ clock */

static void __iomem *div_reg;
static DEFINE_SPINLOCK(div_lock);

static unsigned long mmc_recalc(struct clk_hw *hw, unsigned long parent)
{
	return PLL_SHARED2_HZ / ((readl(div_reg) & 0x1ff) + 1);
}

static unsigned int mmc_ratio(unsigned long rate)
{
	return clamp_t(unsigned long, DIV_ROUND_UP(PLL_SHARED2_HZ, max(rate, 1UL)), 1, 512);
}

static int mmc_determine(struct clk_hw *hw, struct clk_rate_request *req)
{
	req->rate = PLL_SHARED2_HZ / mmc_ratio(req->rate);
	return 0;
}

static int mmc_set_rate(struct clk_hw *hw, unsigned long rate, unsigned long parent)
{
	unsigned long flags;
	u32 v;
	int ret;

	spin_lock_irqsave(&div_lock, flags);
	writel((readl(div_reg) & ~0x1ff) | (mmc_ratio(rate) - 1), div_reg);
	ret = readl_poll_timeout_atomic(div_reg, v, !(v & BIT(16)), 1, 1000);
	spin_unlock_irqrestore(&div_lock, flags);
	return ret;
}

static const struct clk_ops mmc_clk_ops = {
	.recalc_rate = mmc_recalc,
	.determine_rate = mmc_determine,
	.set_rate = mmc_set_rate,
};

static struct clk_hw mmc_clk = {
	.init = &(struct clk_init_data){
		.name = "s9p_mmc_card",
		.ops = &mmc_clk_ops,
	},
};

/* ------------------------------------------------------- regulators */

static int ldo_is_enabled(struct regulator_dev *rdev)
{
	u8 v;
	int ret = s9p_acpm_pmic_read(rdev->desc->enable_reg, &v);

	return ret ?: !!(v & LDO_EN);
}

static int ldo_enable(struct regulator_dev *rdev)
{
	return s9p_acpm_pmic_update(rdev->desc->enable_reg, LDO_EN, LDO_EN);
}

static int ldo_disable(struct regulator_dev *rdev)
{
	return s9p_acpm_pmic_update(rdev->desc->enable_reg, 0, LDO_EN);
}

static int ldo_get_vsel(struct regulator_dev *rdev)
{
	u8 v;
	int ret = s9p_acpm_pmic_read(rdev->desc->vsel_reg, &v);

	return ret ?: v & LDO_VSEL;
}

static int ldo_set_vsel(struct regulator_dev *rdev, unsigned int sel)
{
	return s9p_acpm_pmic_update(rdev->desc->vsel_reg, sel, LDO_VSEL);
}

static const struct regulator_ops ldo_ops = {
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
	.is_enabled = ldo_is_enabled,
	.enable = ldo_enable,
	.disable = ldo_disable,
	.get_voltage_sel = ldo_get_vsel,
	.set_voltage_sel = ldo_set_vsel,
	.set_voltage_time_sel = regulator_set_voltage_time_sel,
};

/* S2MPS18 LDO_MIN3 / LDO_STEP2: 1.8 V + 25 mV per step, 12 mV/us ramp */
#define LDO_DESC(_name, _reg) {						\
	.name = _name, .of_match = _name, .ops = &ldo_ops,		\
	.type = REGULATOR_VOLTAGE, .owner = THIS_MODULE,		\
	.min_uV = 1800000, .uV_step = 25000, .n_voltages = 64,		\
	.vsel_reg = _reg, .vsel_mask = LDO_VSEL,			\
	.enable_reg = _reg, .enable_mask = LDO_EN,			\
	.ramp_delay = 12000, .enable_time = 1000,			\
}

static const struct regulator_desc ldo_desc[] = {
	LDO_DESC("vmmc", L31CTRL),
	LDO_DESC("vqmmc", L2CTRL1),
};

/* ------------------------------------------------------ DT changeset */

static struct of_changeset cs, cs_on;
static bool cs_applied, cs_on_applied, clk_provider;
static struct device_node *clk_np, *reg_np[2], *mmc_np;
static u32 ph_base;
static struct platform_device *pdev;

static struct property *mkprop(const char *name, const void *val, int len)
{
	struct property *p = kzalloc(sizeof(*p), GFP_KERNEL);

	if (!p)
		return NULL;
	p->name = kstrdup(name, GFP_KERNEL);
	p->value = len ? kmemdup(val, len, GFP_KERNEL) : kzalloc(1, GFP_KERNEL);
	p->length = len;
	if (!p->name || !p->value) {
		kfree(p->name);
		kfree(p->value);
		kfree(p);
		return NULL;
	}
	return p;
}

static int prop(struct device_node *np, const char *name, const void *v, int len)
{
	struct property *p = mkprop(name, v, len);

	return p ? of_changeset_add_property(&cs, np, p) : -ENOMEM;
}

static int u32s(struct device_node *np, const char *name, const u32 *v, int n)
{
	__be32 b[8];
	int i;

	for (i = 0; i < n; i++)
		b[i] = cpu_to_be32(v[i]);
	return prop(np, name, b, n * 4);
}

#define STR(np, name, s)	prop(np, name, s, sizeof(s))

static int prop_update(struct device_node *np, const char *name, const void *v, int len)
{
	struct property *p = mkprop(name, v, len);

	return p ? of_changeset_update_property(&cs_on, np, p) : -ENOMEM;
}
#define FLAG(np, name)		prop(np, name, NULL, 0)

static u32 next_phandle(void)
{
	struct device_node *n;
	u32 ph = 0;

	for (n = of_find_all_nodes(NULL); n; n = of_find_all_nodes(n))
		ph = max(ph, n->phandle);
	return ph + 1;
}

static struct device_node *new_node(struct device_node *parent, const char *name, u32 ph)
{
	struct device_node *np = of_changeset_create_node(&cs, parent, name);

	if (np && ph && u32s(np, "phandle", &ph, 1))
		return NULL;
	return np;
}

static int add_nodes(void)
{
	struct device_node *root = of_find_node_by_path("/");
	struct device_node *soc = of_find_node_by_path("/soc@0");
	struct device_node *gpa1 = of_find_node_by_path("/soc@0/pinctrl@14050000/gpa1-gpio-bank");
	struct device_node *mmc;
	u32 ph = ph_base = next_phandle(), ph_clk = ph, ph_vmmc = ph + 1, ph_vqmmc = ph + 2;
	int ret = -ENODEV;

	if (!root || !soc || !gpa1 || !gpa1->phandle)
		goto out;

	ret = -ENOMEM;
	clk_np = new_node(root, "clock-mmc-card", ph_clk);
	reg_np[0] = new_node(root, "regulator-sd-vmmc", ph_vmmc);
	reg_np[1] = new_node(root, "regulator-sd-vqmmc", ph_vqmmc);
	mmc = new_node(soc, "mmc@11500000", 0);
	if (!clk_np || !reg_np[0] || !reg_np[1] || !mmc)
		goto out;

	ret = u32s(clk_np, "#clock-cells", (u32[]){ 0 }, 1) ?:
	      STR(reg_np[0], "regulator-name", "vmmc") ?:
	      u32s(reg_np[0], "regulator-min-microvolt", (u32[]){ 2800000 }, 1) ?:
	      u32s(reg_np[0], "regulator-max-microvolt", (u32[]){ 2800000 }, 1) ?:
	      STR(reg_np[1], "regulator-name", "vqmmc") ?:
	      u32s(reg_np[1], "regulator-min-microvolt", (u32[]){ 1800000 }, 1) ?:
	      u32s(reg_np[1], "regulator-max-microvolt", (u32[]){ 3300000 }, 1) ?:
	      STR(mmc, "compatible", "samsung,exynos7-dw-mshc") ?:
	      u32s(mmc, "reg", (u32[]){ 0x11500000, 0x2000 }, 2) ?:
	      u32s(mmc, "interrupts", (u32[]){ 0, 249, 4 }, 3) ?:
	      u32s(mmc, "#address-cells", (u32[]){ 1 }, 1) ?:
	      u32s(mmc, "#size-cells", (u32[]){ 0 }, 1) ?:
	      u32s(mmc, "clocks", (u32[]){ ph_clk, ph_clk }, 2) ?:
	      STR(mmc, "clock-names", "biu\0ciu") ?:
	      u32s(mmc, "fifo-depth", (u32[]){ 64 }, 1) ?:
	      u32s(mmc, "card-detect-delay", (u32[]){ 200 }, 1) ?:
	      /* CLKSEL: divider 3, drive phase 2, sample phase 0 (DDR: 1) */
	      u32s(mmc, "samsung,dw-mshc-ciu-div", (u32[]){ 3 }, 1) ?:
	      u32s(mmc, "samsung,dw-mshc-sdr-timing", (u32[]){ 0, 2 }, 2) ?:
	      u32s(mmc, "samsung,dw-mshc-ddr-timing", (u32[]){ 1, 2 }, 2) ?:
	      u32s(mmc, "bus-width", (u32[]){ 4 }, 1) ?:
	      u32s(mmc, "max-frequency", (u32[]){ 200000000 }, 1) ?:
	      FLAG(mmc, "cap-sd-highspeed") ?:
	      FLAG(mmc, "cap-cmd23") ?:
	      (uhs ? FLAG(mmc, "sd-uhs-sdr50") ?: FLAG(mmc, "sd-uhs-sdr104") : 0) ?:
	      FLAG(mmc, "disable-wp") ?:
	      FLAG(mmc, "no-sdio") ?:
	      FLAG(mmc, "no-mmc") ?:
	      u32s(mmc, "cd-gpios", (u32[]){ gpa1->phandle, 5, 1 /* active low */ }, 3) ?:
	      u32s(mmc, "vmmc-supply", &ph_vmmc, 1) ?:
	      u32s(mmc, "vqmmc-supply", &ph_vqmmc, 1) ?:
	      STR(mmc, "status", "disabled");
	if (ret)
		goto out;
	mmc_np = mmc;
out:
	of_node_put(root);
	of_node_put(soc);
	of_node_put(gpa1);
	return ret;
}

/* ---------------------------------------------------------- hardware */

static int hw_setup(void)
{
	int i, ret;

	/* card clock source: PLL_SHARED2, 200 MHz until the driver asks */
	rmw(DIV_TOP_MMC, 0x1ff, 3);
	ret = busy_wait(DIV_TOP_MMC, BIT(16));
	rmw(MUX_TOP_MMC, 0x7, 1);
	ret = ret ?: busy_wait(MUX_TOP_MMC, BIT(16));
	rmw(GAT_TOP_MMC, 0, BIT(20) | BIT(21));
	rmw(MUX_USER_MMC, 0, BIT(4));
	ret = ret ?: busy_wait(MUX_USER_MMC, BIT(7));
	rmw(GOUT_MMC_ACLK, 0, BIT(20) | BIT(21));
	rmw(GOUT_MMC_SDCLK, 0, BIT(20) | BIT(21));
	if (ret) {
		pr_err("s9p-sd: CMU switch did not settle\n");
		return ret;
	}

	/* gpf1-0 clk (drive 4), -1 cmd, -2..5 data (drive 3): function 2, no pull */
	for (i = 0; i < 6; i++) {
		rmw(GPF1 + 0x0, 0xf << (4 * i), 2 << (4 * i));
		rmw(GPF1 + 0x8, 0xf << (4 * i), 0);
		rmw(GPF1 + 0xc, 0xf << (4 * i), (i ? 3 : 4) << (4 * i));
	}
	return 0;
}

static int s9p_sd_probe(struct platform_device *p)
{
	struct regulator_config cfg = { .dev = &p->dev };
	struct regulator_dev *rdev;
	int i, ret;

	if (!s9p_acpm_is_ready())
		return -EPROBE_DEFER;

	div_reg = devm_ioremap(&p->dev, DIV_TOP_MMC, 4);
	if (!div_reg)
		return -ENOMEM;
	ret = hw_setup();
	if (ret)
		return ret;

	/*
	 * Two steps: the clock and regulator nodes (and the mmc node, still
	 * disabled) first, so that their providers exist before dw_mmc ever
	 * sees the node -- fw_devlink would otherwise park the controller on
	 * suppliers that never become devices.
	 */
	of_changeset_init(&cs);
	ret = add_nodes();
	if (ret)
		goto err_cs;
	ret = of_changeset_apply(&cs);
	if (ret)
		goto err_cs;
	cs_applied = true;
	/* attaching read "phandle" before the changeset added it */
	clk_np->phandle = ph_base;
	reg_np[0]->phandle = ph_base + 1;
	reg_np[1]->phandle = ph_base + 2;

	ret = devm_clk_hw_register(&p->dev, &mmc_clk) ?:
	      of_clk_add_hw_provider(clk_np, of_clk_hw_simple_get, &mmc_clk);
	if (ret)
		goto err_cs;
	clk_provider = true;

	for (i = 0; i < ARRAY_SIZE(ldo_desc); i++) {
		cfg.of_node = reg_np[i];
		cfg.init_data = of_get_regulator_init_data(&p->dev, reg_np[i], &ldo_desc[i]);
		if (!cfg.init_data) {
			ret = -ENOMEM;
			goto err_cs;
		}
		rdev = devm_regulator_register(&p->dev, &ldo_desc[i], &cfg);
		if (IS_ERR(rdev)) {
			ret = PTR_ERR(rdev);
			goto err_cs;
		}
		fwnode_dev_initialized(of_fwnode_handle(reg_np[i]), true);
	}

	/* status = "okay" creates the platform device: dw_mmc probes */
	of_changeset_init(&cs_on);
	ret = prop_update(mmc_np, "status", "okay", sizeof("okay")) ?:
	      of_changeset_apply(&cs_on);
	if (ret) {
		of_changeset_destroy(&cs_on);
		goto err_cs;
	}
	cs_on_applied = true;
	pr_info("s9p-sd: mmc@11500000 enabled (card %s, %s)\n",
		rd(0x14050044) & BIT(5) ? "absent" : "present",
		uhs ? "UHS-I" : "high speed only");
	return 0;

err_cs:
	if (clk_provider)
		of_clk_del_provider(clk_np);
	clk_provider = false;
	if (cs_applied)
		of_changeset_revert(&cs);
	of_changeset_destroy(&cs);
	cs_applied = false;
	return ret;
}

static void s9p_sd_remove(struct platform_device *p)
{
	if (cs_on_applied) {
		of_changeset_revert(&cs_on);
		of_changeset_destroy(&cs_on);
		cs_on_applied = false;
	}
	if (clk_provider)
		of_clk_del_provider(clk_np);
	clk_provider = false;
	if (cs_applied)
		of_changeset_revert(&cs);
	of_changeset_destroy(&cs);
	cs_applied = false;
}

static struct platform_driver s9p_sd_driver = {
	.probe = s9p_sd_probe,
	.remove = s9p_sd_remove,
	.driver = { .name = "s9p-sd-board" },
};

static int __init s9p_sd_init(void)
{
	int ret;

	if (!of_machine_is_compatible("samsung,exynos9810"))
		return -ENODEV;
	ret = platform_driver_register(&s9p_sd_driver);
	if (ret)
		return ret;
	pdev = platform_device_register_simple("s9p-sd-board", -1, NULL, 0);
	if (IS_ERR(pdev)) {
		platform_driver_unregister(&s9p_sd_driver);
		return PTR_ERR(pdev);
	}
	return 0;
}

static void __exit s9p_sd_exit(void)
{
	platform_device_unregister(pdev);
	platform_driver_unregister(&s9p_sd_driver);
}

module_init(s9p_sd_init);
module_exit(s9p_sd_exit);
MODULE_DESCRIPTION("Galaxy S9+ microSD slot: clock, supplies, pins and the DWMMC2 node");
MODULE_LICENSE("GPL");
