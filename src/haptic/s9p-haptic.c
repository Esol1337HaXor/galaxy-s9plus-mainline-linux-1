// SPDX-License-Identifier: GPL-2.0
/*
 * Galaxy S9+ (exynos9810): vibration motor.
 *
 * The motor is an LRA driven by the MAX77705 PMIC from an external PWM, TOUT1
 * of the SoC's PWM timer block (pwm@10510000, pin gpp3-1). The MFD core
 * always registers a "max77705-haptic" platform device, but the port's device
 * tree has neither the PWM block nor a "haptic" node, so the upstream driver
 * (max77693-haptic) cannot get its PWM. This module opens the PWM block's bus
 * clock, adds both nodes with an OF changeset and hands the haptic node to the
 * existing device; then the driver can be loaded.
 *
 * Values from the vendor DT (pwm_id 1, period 48828 ns: 20.48 kHz, divided by
 * 128 in the PMIC = 160 Hz, the LRA's resonance) and max77705_haptic.c:
 * vibration/analisis/01-haptic.md. The PWM tick is OSCCLK (26 MHz).
 */

#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define PMIC_NODE	"/soc@0/i2c@14360000/pmic@66"
#define PWM_BASE	0x10510000
#define PWM_GATE	0x10402044	/* CMU_PERIC0 PWM bus clock gate */
#define PWM_PERIOD_NS	48828

static struct of_changeset cs;
static struct device *hap_dev;
static struct device_node *hap_np;

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

/* nodes of the port's DT without a phandle: give one to reference them */
static u32 node_phandle(const char *path)
{
	struct device_node *np = of_find_node_by_path(path), *n;
	struct property *p;
	u32 max = 0, ph;
	__be32 b;

	if (!np)
		return 0;
	ph = np->phandle;
	if (!ph) {
		for (n = of_find_all_nodes(NULL); n; n = of_find_all_nodes(n))
			max = max(max, n->phandle);
		ph = max + 1;
		b = cpu_to_be32(ph);
		p = mkprop("phandle", &b, 4);
		if (!p || of_add_property(np, p)) {
			of_node_put(np);
			return 0;
		}
		np->phandle = ph;	/* the property alone does not set it */
	}
	of_node_put(np);
	return ph;
}

/* manual mode, clock on: pwm-samsung reads TCON in its probe */
static void open_gate(void)
{
	void __iomem *g = ioremap(PWM_GATE & ~0xfffUL, 0x1000);

	if (!g)
		return;
	writel(readl(g + (PWM_GATE & 0xfff)) | BIT(20) | BIT(21), g + (PWM_GATE & 0xfff));
	iounmap(g);
}

static int add_nodes(struct device_node *soc, struct device_node *pmic, u32 osc)
{
	struct device_node *pwm;
	int ret;
	u32 ph;

	of_changeset_init(&cs);
	pwm = of_changeset_create_node(&cs, soc, "pwm@10510000");
	if (!pwm)
		return -ENOMEM;
	/* a fresh phandle for the PWM, above any in the tree */
	ph = 0;
	for (struct device_node *n = of_find_all_nodes(NULL); n; n = of_find_all_nodes(n))
		ph = max(ph, n->phandle + 1);
	ret = STR(pwm, "compatible", "samsung,s3c6400-pwm") ?:
	      u32s(pwm, "reg", (u32[]){ PWM_BASE, 0x1000 }, 2) ?:
	      u32s(pwm, "clocks", &osc, 1) ?:
	      STR(pwm, "clock-names", "timers") ?:
	      u32s(pwm, "samsung,pwm-outputs", (u32[]){ 1 }, 1) ?:
	      u32s(pwm, "#pwm-cells", (u32[]){ 3 }, 1) ?:
	      u32s(pwm, "phandle", &ph, 1) ?:
	      STR(pwm, "status", "okay");
	if (ret)
		return ret;

	hap_np = of_changeset_create_node(&cs, pmic, "haptic");
	if (!hap_np)
		return -ENOMEM;
	/* no haptic-supply: the motor boost is not under software control */
	ret = STR(hap_np, "compatible", "maxim,max77705-haptic") ?:
	      u32s(hap_np, "pwms", (u32[]){ ph, 1, PWM_PERIOD_NS, 0 }, 4) ?:
	      of_changeset_apply(&cs);
	/* attaching the node read its phandle before the property was added */
	if (!ret)
		pwm->phandle = ph;
	return ret;
}

static int __init s9p_haptic_init(void)
{
	struct device_node *soc = of_find_node_by_path("/soc@0");
	struct device_node *pmic = of_find_node_by_path(PMIC_NODE);
	u32 osc = node_phandle("/osc-clock");
	int ret;

	if (!soc || !pmic || !osc) {
		ret = -ENODEV;
		goto out;
	}
	hap_dev = bus_find_device_by_name(&platform_bus_type, NULL, "max77705-haptic");
	if (!hap_dev) {
		ret = -ENODEV;
		goto out;
	}
	if (hap_dev->driver) {
		pr_info("s9p-haptic: max77705-haptic already has a driver\n");
		ret = -EBUSY;
		goto put;
	}
	open_gate();
	ret = add_nodes(soc, pmic, osc);
	if (ret) {
		of_changeset_destroy(&cs);
		goto put;
	}
	device_set_node(hap_dev, of_fwnode_handle(hap_np));
	/* the upstream driver binds once loaded; if it already is, probe now */
	if (device_attach(hap_dev) < 0)
		pr_warn("s9p-haptic: attach failed\n");
	pr_info("s9p-haptic: pwm@10510000 and %pOF added\n", hap_np);
	goto out;
put:
	put_device(hap_dev);
	hap_dev = NULL;
out:
	of_node_put(soc);
	of_node_put(pmic);
	return ret;
}

static void __exit s9p_haptic_exit(void)
{
	device_release_driver(hap_dev);
	device_set_node(hap_dev, NULL);
	of_changeset_revert(&cs);
	of_changeset_destroy(&cs);
	put_device(hap_dev);
}

module_init(s9p_haptic_init);
module_exit(s9p_haptic_exit);
MODULE_DESCRIPTION("Galaxy S9+ vibration motor: PWM and haptic nodes for the MAX77705");
MODULE_LICENSE("GPL");
