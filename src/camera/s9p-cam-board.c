// SPDX-License-Identifier: GPL-2.0
/*
 * Galaxy S9+ (exynos9810): buses and pins of the front camera (bring-up).
 *
 * The port's device tree has none of the camera buses, so (as
 * s9p-audio-board.c does for the audio buses) this module opens the clock
 * gates, selects the USI protocol, programs the pin mux by hand and adds the
 * buses with an OF changeset:
 *
 *   i2c@10920000 (USI10)      S2MPB02 camera PMIC, 0x59
 *   i2c@108f0000 (USI08_I2C)  S5K3H1 front sensor, 0x35
 *   i2c@10900000 (USI09)      front AF (AK7372) 0x0c, EEPROM 0x51
 *
 * plus the sensor's reset (gpg2-5) and master clock (CIS_CLK1, 26 MHz on
 * gpc1-5), driven through the "rst" and "mclk" parameters so the vendor's
 * power-up order can be followed: regulators, reset high, MCLK on the pin.
 *
 * Addresses and values: camaras/analisis/02-fase1-frontal.md (from the
 * vendor DT, CMU tables and fimc-is-device-module-3h1.c). The bus clocks are
 * the 200 MHz stub of the port; the real block clocks are equal or lower, so
 * the bus speeds the driver computes are upper bounds (slower is safe).
 */

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

static struct of_changeset *cs;
static DEFINE_MUTEX(pin_lock);

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

static void wr(phys_addr_t a, u32 v)
{
	void __iomem *p = map(a);

	writel(v, p);
	unmap(p);
}

static void rmw(phys_addr_t a, u32 clr, u32 set, const char *what)
{
	u32 old = rd(a), new = (old & ~clr) | set;

	wr(a, new);
	pr_debug("s9p-cam: %-16s %08llx: %08x -> %08x\n", what, (u64)a, old, new);
}

/* CMU gate: manual mode, clock on */
static void gate(phys_addr_t a)
{
	rmw(a, 0, BIT(20) | BIT(21), "gate");
}

/* exynos850-type bank: CON +0, DAT +4, PUD +8, DRV +0xc, 4-bit fields */
static void pin(phys_addr_t bank, int n, u32 func, u32 pud)
{
	u32 sh = n * 4;

	rmw(bank + 0x0, 0xf << sh, func << sh, "pin con");
	rmw(bank + 0x8, 0xf << sh, pud << sh, "pin pud");
}

#define GPG2		0x104300c0	/* pinctrl@10430000 */
#define GPC1		0x10830080	/* pinctrl@10830000 */
#define GPP5		0x10830020
#define GPP6		0x10830040
#define RST_PIN		5		/* gpg2-5 */
#define MCLK_PIN	5		/* gpc1-5 */
#define CIS_CLK1_GATE	0x1a24201c

static void mclk_pin(bool on)
{
	if (on) {
		pin(GPC1, MCLK_PIN, 2, 0);			/* fimc_is_mclk1_fn */
		usleep_range(9000, 10000);
	} else {
		rmw(GPC1 + 4, BIT(MCLK_PIN), 0, "gpc1 dat");
		pin(GPC1, MCLK_PIN, 1, 1);			/* output low, pull-down */
	}
}

static void reset_pin(bool high)
{
	rmw(GPG2 + 4, BIT(RST_PIN), high ? BIT(RST_PIN) : 0, "gpg2 dat");
}

static int set_rst(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_bool(val, kp);

	if (!ret && cs) {
		mutex_lock(&pin_lock);
		reset_pin(*(bool *)kp->arg);
		mutex_unlock(&pin_lock);
	}
	return ret;
}

static int set_mclk(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_bool(val, kp);

	if (!ret && cs) {
		mutex_lock(&pin_lock);
		mclk_pin(*(bool *)kp->arg);
		mutex_unlock(&pin_lock);
	}
	return ret;
}

static bool rst, mclk;
static const struct kernel_param_ops rst_ops = { .set = set_rst, .get = param_get_bool };
static const struct kernel_param_ops mclk_ops = { .set = set_mclk, .get = param_get_bool };
module_param_cb(rst, &rst_ops, &rst, 0644);
MODULE_PARM_DESC(rst, "front sensor reset (gpg2-5): 1 = running");
module_param_cb(mclk, &mclk_ops, &mclk, 0644);
MODULE_PARM_DESC(mclk, "front sensor master clock on gpc1-5 (26 MHz)");

/* Register dump for bring-up diagnosis: are the pins and clocks really
 * where the module left them? */
static int regs_show(struct seq_file *m, void *v)
{
	seq_printf(m, "gpg2       con %08x dat %08x (rst pin 5, bits 20-23)\n",
		   rd(GPG2), rd(GPG2 + 4));
	seq_printf(m, "gpc1       con %08x dat %08x (mclk pin 5, bits 20-23)\n",
		   rd(GPC1), rd(GPC1 + 4));
	seq_printf(m, "gpp5       con %08x (usi08 sda/scl pins 2-3, usi09 4-5)\n",
		   rd(GPP5));
	seq_printf(m, "gpp6       con %08x (usi10 pins 0-1)\n", rd(GPP6));
	seq_printf(m, "us08       con %08x opt %08x int_en %08x conf %08x\n",
		   rd(0x108f00c4), rd(0x108f00c8), rd(0x108f00e4), rd(0x1081103c));
	seq_printf(m, "cisclk1    mux %08x div %08x gate %08x\n",
		   rd(0x1a24101c), rd(0x1a241818), rd(CIS_CLK1_GATE));
	seq_printf(m, "rst=%d mclk=%d\n", rst, mclk);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(regs);

static void hw_setup(void)
{
	/* CMU_TOP PERIC1 (s9p-dp opens them too) */
	gate(0x1a2420c0);
	gate(0x1a2420c4);

	/* USI10: S2MPB02 */
	gate(0x10802048);
	gate(0x10802114);
	gate(0x10802118);
	gate(0x108020ac);
	rmw(0x1081103c, 0x7, 0x4, "USI10 SW_CONF");
	rmw(0x109200c4, 0x1, 0, "USI10 USI_CON");
	rmw(0x109200c8, 0, 0x2, "USI10 USI_OPT");
	pin(GPP6, 0, 2, 0);
	pin(GPP6, 1, 2, 0);

	/* USI08_I2C: front sensor (SW_CONF already I2C) */
	gate(0x10802050);
	gate(0x108020ec);
	gate(0x108020f0);
	gate(0x10802098);
	rmw(0x108f00c4, 0x1, 0, "USI08 USI_CON");
	rmw(0x108f00c8, 0, 0x2, "USI08 USI_OPT");
	pin(GPP5, 2, 3, 0);
	pin(GPP5, 3, 3, 0);

	/* USI09: front AF + EEPROM */
	gate(0x10802044);
	gate(0x10802104);
	gate(0x10802108);
	gate(0x108020a4);
	rmw(0x10811034, 0x7, 0x4, "USI09 SW_CONF");
	rmw(0x109000c4, 0x1, 0, "USI09 USI_CON");
	rmw(0x109000c8, 0, 0x2, "USI09 USI_OPT");
	pin(GPP5, 4, 2, 0);
	pin(GPP5, 5, 2, 0);

	/* sensor pins off: reset low, MCLK pin low (data before direction) */
	reset_pin(false);
	rmw(GPG2, 0xf << (RST_PIN * 4), 0x1 << (RST_PIN * 4), "gpg2-5 out");
	mclk_pin(false);

	/* CIS_CLK1 = OSCCLK / 1 = 26 MHz in the CMU; the pin comes later */
	rmw(0x1a24101c, 0x1, 0, "CIS_CLK1 mux");
	rmw(0x1a241818, 0x1f, 0, "CIS_CLK1 div");
	gate(CIS_CLK1_GATE);
}

/* ------------------------------------------------------------------ */

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

	return p ? of_changeset_add_property(cs, np, p) : -ENOMEM;
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

static int add_bus(struct device_node *soc, const char *name, u32 base, u32 spi, u32 ph)
{
	struct device_node *np = of_changeset_create_node(cs, soc, name);

	return !np ? -ENOMEM :
	       STR(np, "compatible", "samsung,exynosautov9-hsi2c") ?:
	       u32s(np, "reg", (u32[]){ base, 0x1000 }, 2) ?:
	       u32s(np, "interrupts", (u32[]){ 0, spi, 4 }, 3) ?:
	       u32s(np, "clocks", (u32[]){ ph, ph }, 2) ?:
	       STR(np, "clock-names", "hsi2c\0hsi2c_pclk") ?:
	       u32s(np, "clock-frequency", (u32[]){ 400000 }, 1) ?:
	       u32s(np, "#address-cells", (u32[]){ 1 }, 1) ?:
	       u32s(np, "#size-cells", (u32[]){ 0 }, 1) ?:
	       STR(np, "status", "okay");
}

static int add_nodes(void)
{
	struct device_node *soc, *stub, *bus = NULL;
	int ret;

	soc = of_find_node_by_path("/soc@0");
	stub = of_find_node_by_path("/clock-hsi2c-stub");
	if (!soc || !stub) {
		ret = -ENODEV;
		goto out;
	}
	cs = kzalloc(sizeof(*cs), GFP_KERNEL);
	if (!cs) {
		ret = -ENOMEM;
		goto out;
	}
	of_changeset_init(cs);
	/* s9p-flash (extras/flashlight) adds the S2MPB02's bus at boot */
	bus = of_find_node_by_path("/soc@0/i2c@10920000");
	ret = (bus ? 0 : add_bus(soc, "i2c@10920000", 0x10920000, 436, stub->phandle)) ?:
	      add_bus(soc, "i2c@108f0000", 0x108f0000, 433, stub->phandle) ?:
	      add_bus(soc, "i2c@10900000", 0x10900000, 434, stub->phandle) ?:
	      of_changeset_apply(cs);
	if (ret) {
		of_changeset_destroy(cs);
		kfree(cs);
		cs = NULL;
		goto out;
	}
	pr_info("s9p-cam: added i2c@10920000 (S2MPB02), i2c@108f0000 (front sensor), i2c@10900000 (front AF/EEPROM)\n");
out:
	of_node_put(bus);
	of_node_put(soc);
	of_node_put(stub);
	return ret;
}

static struct dentry *dir;

static int __init s9p_cam_board_init(void)
{
	hw_setup();
	dir = debugfs_create_dir("s9p-cam", NULL);
	debugfs_create_file("regs", 0400, dir, NULL, &regs_fops);
	return add_nodes();
}

static void __exit s9p_cam_board_exit(void)
{
	debugfs_remove_recursive(dir);
	mutex_lock(&pin_lock);
	mclk_pin(false);
	reset_pin(false);
	mutex_unlock(&pin_lock);
	rmw(CIS_CLK1_GATE, BIT(21), 0, "CIS_CLK1 gate off");
	of_changeset_revert(cs);
	of_changeset_destroy(cs);
	kfree(cs);
}

module_init(s9p_cam_board_init);
module_exit(s9p_cam_board_exit);
MODULE_DESCRIPTION("Galaxy S9+ front camera buses and pins (bring-up)");
MODULE_LICENSE("GPL");
