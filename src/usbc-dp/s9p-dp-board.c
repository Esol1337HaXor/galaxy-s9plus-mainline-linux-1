// SPDX-License-Identifier: GPL-2.0
/*
 * Galaxy S9+ (star2lte): the board side of USB-C DisplayPort.
 *
 * The DP AUX channel reaches the connector's SBU pins through an analog
 * switch whose enable (P0_4, active low) and direction select (P0_1) sit
 * on a PCAL6524 GPIO expander at 0x22 on hsi2c@10930000 (PERIC1 USI10's
 * I2C block). The mainline board DT has neither the bus nor the expander,
 * so this file adds them at runtime with an OF changeset (the kernel has
 * OF_DYNAMIC; the board DTB carries no __symbols__, so an overlay could
 * not reference the base nodes) after opening the bus's clock chain:
 *
 *   CMU top  0x1a240000: GATE_CLKCMU_PERIC1_IP +0x20c4, _BUS +0x20c0
 *   PERIC1   0x10800000: GATE_CLK_PERIC1_USI_I2C +0x2050,
 *                        USI10_I2C IPCLK +0x210c, PCLK +0x2110,
 *                        RSTnSYNC USI10_I2C +0x20a8
 *   USI10_I2C 0x10930000: USI_CON +0xc4 = 0 (reset off),
 *                         USI_OPTION +0xc8 |= CLKREQ_ON
 *
 * (vendor cmucal-sfr.c / cmucal-node.c for exynos9810; stock DT
 * hsi2c@10930000 with clocks GATE_USI10_I2C / PERIC1_USI_I2C.) The
 * expander's reset is gpg3-6, held high (released) by the stock pinctrl.
 *
 * Orientation (stock displayport_drv.c displayport_aux_sel/aux_onoff):
 *   CC1: sbu_sw_sel = 1, CC2: sbu_sw_sel = 0; then aux_sw_oe = 0 and
 *   100 ms before the first AUX transaction; aux_sw_oe = 1 when done.
 */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/slab.h>

#include "s9p-dp.h"

#define CLK_GATE_MANUAL		BIT(20)
#define CLK_GATE_CG_VAL		BIT(21)

static const u32 sbu_gates[] = {
	0x1a2420c4,	/* GATE_CLKCMU_PERIC1_IP (CMU top) */
	0x1a2420c0,	/* GATE_CLKCMU_PERIC1_BUS (CMU top) */
	0x10802050,	/* GATE_CLK_PERIC1_USI_I2C */
	0x1080210c,	/* USI10_I2C IPCLK */
	0x10802110,	/* USI10_I2C PCLK */
	0x108020a8,	/* RSTnSYNC_CLK_PERIC1_USI10_I2C */
};

/* read-only, for the log: the clock chain as TWRP/sboot left it */
static const u32 sbu_dump[] = {
	0x1a2410b8,	/* MUX_CLKCMU_PERIC1_IP */
	0x1a2418bc,	/* DIV_CLKCMU_PERIC1_IP */
	0x1a2410b4,	/* MUX_CLKCMU_PERIC1_BUS */
	0x1a2418b8,	/* DIV_CLKCMU_PERIC1_BUS */
	0x10800100,	/* PLL_CON0_MUX_CLKCMU_PERIC1_BUS_USER */
	0x10800120,	/* PLL_CON0_MUX_CLKCMU_PERIC1_IP_USER */
	0x10801830,	/* DIV_CLK_PERIC1_USI_I2C */
	0x1080304c,	/* QCH_CON_USI10_I2C_QCH */
};

#define USI10_I2C_BASE		0x10930000
#define USI_CON			0xc4
#define USI_OPTION		0xc8
#define  USI_OPTION_CLKREQ_ON	BIT(1)

static struct gpio_desc *expander_reset;

/*
 * gpp6-2/3 = hsi2c25 SCL/SDA (stock pinctrl "hsi2c25-bus": function 3, no
 * pull). Programmed by hand: a pin group added to the DT after
 * pinctrl-samsung probed never becomes a selectable function, and the
 * changeset would attach the node before its phandle property exists.
 * Bank gpp6 at pinctrl@10830000 + 0x40, 4-bit CON/PUD/DRV fields
 * (exynos850_bank_type_off).
 */
#define GPP6_BASE		0x10830040
#define GPP6_CON		0x0
#define GPP6_PUD		0x8
#define GPP6_DRV		0xc

static void set_bus_pins(void)
{
	void __iomem *b = ioremap(GPP6_BASE, 0x10);
	u32 con, pud, drv;

	if (!b)
		return;
	con = readl(b + GPP6_CON);
	pud = readl(b + GPP6_PUD);
	drv = readl(b + GPP6_DRV);
	writel((con & ~0xff00) | 0x3300, b + GPP6_CON);
	writel(pud & ~0xff00, b + GPP6_PUD);
	writel(drv & ~0xff00, b + GPP6_DRV);
	pr_info("s9p-dp: gpp6 con %08x->%08x pud %08x->%08x drv %08x->%08x\n",
		con, readl(b + GPP6_CON), pud, readl(b + GPP6_PUD), drv,
		readl(b + GPP6_DRV));
	iounmap(b);
}

#define EXP_ADDR		0x22
#define EXP_PIN_SBU_SEL		1	/* P0_1 */
#define EXP_PIN_AUX_OE		4	/* P0_4, active low */

static u32 rd32(u32 addr)
{
	void __iomem *r = ioremap(addr, 4);
	u32 v;

	if (!r)
		return 0xdeadbeef;
	v = readl(r);
	iounmap(r);
	return v;
}

static void or32(u32 addr, u32 bits, const char *what)
{
	void __iomem *r = ioremap(addr, 4);
	u32 v;

	if (!r)
		return;
	v = readl(r);
	writel(v | bits, r);
	pr_info("s9p-dp: %08x %s: %08x -> %08x\n", addr, what, v, readl(r));
	iounmap(r);
}

static void wr32(u32 addr, u32 val, const char *what)
{
	void __iomem *r = ioremap(addr, 4);
	u32 v;

	if (!r)
		return;
	v = readl(r);
	writel(val, r);
	pr_info("s9p-dp: %08x %s: %08x -> %08x\n", addr, what, v, readl(r));
	iounmap(r);
}

static struct property *mkprop(const char *name, const void *val, int len)
{
	struct property *p = kzalloc(sizeof(*p), GFP_KERNEL);

	if (!p)
		return NULL;
	p->name = kstrdup(name, GFP_KERNEL);
	p->value = kmemdup(val, len, GFP_KERNEL);
	p->length = len;
	if (!p->name || !p->value) {
		kfree(p->name);
		kfree(p->value);
		kfree(p);
		return NULL;
	}
	return p;
}

static int add_u32s(struct of_changeset *cs, struct device_node *np,
		    const char *name, const u32 *v, int n)
{
	__be32 *b = kcalloc(n, sizeof(*b), GFP_KERNEL);
	struct property *p;
	int i;

	if (!b)
		return -ENOMEM;
	for (i = 0; i < n; i++)
		b[i] = cpu_to_be32(v[i]);
	p = mkprop(name, b, n * sizeof(*b));
	kfree(b);
	return p ? of_changeset_add_property(cs, np, p) : -ENOMEM;
}

static int add_str(struct of_changeset *cs, struct device_node *np,
		   const char *name, const char *s)
{
	struct property *p = mkprop(name, s, strlen(s) + 1);

	return p ? of_changeset_add_property(cs, np, p) : -ENOMEM;
}

/* "a\0b\0" string lists */
static int add_strs(struct of_changeset *cs, struct device_node *np,
		    const char *name, const char *s, int len)
{
	struct property *p = mkprop(name, s, len);

	return p ? of_changeset_add_property(cs, np, p) : -ENOMEM;
}

static int add_bool(struct of_changeset *cs, struct device_node *np,
		    const char *name)
{
	struct property *p = mkprop(name, "", 0);

	return p ? of_changeset_add_property(cs, np, p) : -ENOMEM;
}

static struct device_node *new_node(struct of_changeset *cs,
				    struct device_node *parent,
				    const char *full_name)
{
	struct device_node *np = of_changeset_create_node(cs, parent, full_name);

	return np;
}

/* gpg3-6 high: the stock "expander_reset_active" pinctrl state */
static void release_expander_reset(void)
{
	struct gpio_device *gdev;
	struct gpio_desc *d;

	gdev = gpio_device_find_by_label("gpg3");
	if (!gdev)
		gdev = gpio_device_find_by_label("gpg3-gpio-bank");
	if (!gdev) {
		pr_warn("s9p-dp: gpg3 bank not found, expander reset left alone\n");
		return;
	}
	d = s9p_grab_gpio(gdev, 6, "pcal6524-reset", GPIOD_ASIS);
	if (IS_ERR(d)) {
		pr_warn("s9p-dp: cannot claim gpg3-6: %ld\n", PTR_ERR(d));
		gpio_device_put(gdev);
		return;
	}
	pr_info("s9p-dp: gpg3-6 (expander reset) was %d, driving 1\n",
		gpiod_get_raw_value(d));
	gpiod_direction_output_raw(d, 1);
	expander_reset = d;
	/* keep it requested: nobody else may toggle the reset */
	gpio_device_put(gdev);
}

static int create_sbu_bus(struct s9p_dp *dp)
{
	struct device_node *soc, *bus, *exp, *stub;
	struct of_changeset *cs;
	u32 stub_ph;
	static const char clknames[] = "hsi2c\0hsi2c_pclk";
	int ret;

	bus = of_find_node_by_path("/soc@0/i2c@10930000");
	if (bus) {
		of_node_put(bus);
		pr_info("s9p-dp: hsi2c@10930000 already in the DT\n");
		return 0;
	}
	soc = of_find_node_by_path("/soc@0");
	stub = of_find_node_by_path("/clock-hsi2c-stub");
	if (!soc || !stub) {
		ret = -ENODEV;
		goto out_put;
	}
	stub_ph = stub->phandle;

	cs = kzalloc(sizeof(*cs), GFP_KERNEL);
	if (!cs) {
		ret = -ENOMEM;
		goto out_put;
	}
	of_changeset_init(cs);

	bus = new_node(cs, soc, "i2c@10930000");
	if (!bus) {
		ret = -ENOMEM;
		goto out_cs;
	}
	ret = add_str(cs, bus, "compatible", "samsung,exynosautov9-hsi2c") ?:
	      add_u32s(cs, bus, "reg", (u32[]){ USI10_I2C_BASE, 0x1000 }, 2) ?:
	      add_u32s(cs, bus, "interrupts", (u32[]){ 0, 437, 4 }, 3) ?:
	      add_u32s(cs, bus, "#address-cells", (u32[]){ 1 }, 1) ?:
	      add_u32s(cs, bus, "#size-cells", (u32[]){ 0 }, 1) ?:
	      add_u32s(cs, bus, "clocks", (u32[]){ stub_ph, stub_ph }, 2) ?:
	      add_strs(cs, bus, "clock-names", clknames, sizeof(clknames)) ?:
	      add_u32s(cs, bus, "clock-frequency", (u32[]){ 200000 }, 1) ?:
	      add_str(cs, bus, "status", "okay");
	if (ret)
		goto out_cs;

	exp = new_node(cs, bus, "gpio@22");
	if (!exp) {
		ret = -ENOMEM;
		goto out_cs;
	}
	ret = add_str(cs, exp, "compatible", "nxp,pcal6524") ?:
	      add_u32s(cs, exp, "reg", (u32[]){ EXP_ADDR }, 1) ?:
	      add_bool(cs, exp, "gpio-controller") ?:
	      add_u32s(cs, exp, "#gpio-cells", (u32[]){ 2 }, 1) ?:
	      add_str(cs, exp, "status", "okay");
	if (ret)
		goto out_cs;

	ret = of_changeset_apply(cs);
	if (ret) {
		pr_err("s9p-dp: OF changeset failed: %d\n", ret);
		goto out_cs;
	}
	dp->sbu_cs = cs;
	pr_info("s9p-dp: added hsi2c@10930000 + pcal6524@22\n");
	goto out_put;

out_cs:
	of_changeset_destroy(cs);
	kfree(cs);
out_put:
	of_node_put(stub);
	of_node_put(soc);
	return ret;
}

int s9p_dp_board_init(struct s9p_dp *dp)
{
	struct gpio_device *gdev = NULL;
	struct device_node *np;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(sbu_dump); i++)
		pr_info("s9p-dp: clock chain %08x = %08x\n", sbu_dump[i],
			rd32(sbu_dump[i]));
	for (i = 0; i < ARRAY_SIZE(sbu_gates); i++)
		or32(sbu_gates[i], CLK_GATE_MANUAL | CLK_GATE_CG_VAL, "gate");
	wr32(USI10_I2C_BASE + USI_CON, 0, "USI_CON");
	or32(USI10_I2C_BASE + USI_OPTION, USI_OPTION_CLKREQ_ON, "USI_OPTION");

	set_bus_pins();
	release_expander_reset();
	msleep(5);

	ret = create_sbu_bus(dp);
	if (ret)
		return ret;

	/* wait for the bus and the pca953x driver to probe */
	np = of_find_node_by_path("/soc@0/i2c@10930000/gpio@22");
	for (i = 0; i < 50 && np; i++) {
		gdev = gpio_device_find_by_fwnode(of_fwnode_handle(np));
		if (gdev)
			break;
		msleep(100);
	}
	of_node_put(np);
	if (!gdev) {
		pr_err("s9p-dp: PCAL6524 did not appear (bus or expander dead?)\n");
		return -ENODEV;
	}
	/* AUX switch off until a sink is there (stock probe: aux_sw_oe = 1) */
	dp->aux_oe = s9p_grab_gpio(gdev, EXP_PIN_AUX_OE, "dp-aux-oe", GPIOD_OUT_HIGH);
	dp->sbu_sel = s9p_grab_gpio(gdev, EXP_PIN_SBU_SEL, "dp-sbu-sel", GPIOD_OUT_LOW);
	if (IS_ERR(dp->sbu_sel) || IS_ERR(dp->aux_oe)) {
		if (!IS_ERR(dp->aux_oe))
			gpiochip_free_own_desc(dp->aux_oe);
		if (!IS_ERR(dp->sbu_sel))
			gpiochip_free_own_desc(dp->sbu_sel);
		dp->aux_oe = dp->sbu_sel = NULL;
		gpio_device_put(gdev);
		return -EBUSY;
	}
	dp->exp_gdev = gdev;
	pr_info("s9p-dp: SBU/AUX switch ready on the PCAL6524\n");
	return 0;
}

void s9p_dp_board_aux(struct s9p_dp *dp, bool on, u8 orientation)
{
	if (!dp->aux_oe)
		return;
	if (on) {
		if (orientation != 1 && orientation != 2)
			pr_warn("s9p-dp: unknown cable orientation, assuming CC1\n");
		/* CC1 -> sbu_sw_sel 1, CC2 -> 0 (stock: sbu_sw_sel = !usb_con_sel) */
		gpiod_set_raw_value_cansleep(dp->sbu_sel, orientation == 2 ? 0 : 1);
		gpiod_set_raw_value_cansleep(dp->aux_oe, 0);
		msleep(100);
	} else {
		gpiod_set_raw_value_cansleep(dp->aux_oe, 1);
	}
	pr_info("s9p-dp: AUX switch %s (CC%u, sbu_sel=%d)\n", on ? "on" : "off",
		orientation, gpiod_get_raw_value_cansleep(dp->sbu_sel));
}

void s9p_dp_board_exit(struct s9p_dp *dp)
{
	if (dp->aux_oe) {
		gpiod_set_raw_value_cansleep(dp->aux_oe, 1);
		gpiochip_free_own_desc(dp->aux_oe);
		gpiochip_free_own_desc(dp->sbu_sel);
		dp->aux_oe = NULL;
	}
	if (dp->exp_gdev)
		gpio_device_put(dp->exp_gdev);
	if (dp->sbu_cs) {
		of_changeset_revert(dp->sbu_cs);
		of_changeset_destroy(dp->sbu_cs);
		kfree(dp->sbu_cs);
		dp->sbu_cs = NULL;
	}
	if (expander_reset) {
		/* leave the expander running: just give the line back */
		gpiochip_free_own_desc(expander_reset);
		expander_reset = NULL;
	}
}
