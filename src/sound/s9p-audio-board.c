// SPDX-License-Identifier: GPL-2.0
/*
 * Galaxy S9+ (exynos9810): buses of the audio codec and amplifiers.
 *
 * The port's device tree has neither of them, so (as s9p-dp-board.c does for
 * the DP SBU expander) this module opens the clock gates, selects the USI
 * protocol, programs the pin mux by hand and adds the bus nodes with an OF
 * changeset:
 *
 *   spi@10470000 (USI01, PERIC0): Cirrus CS47L93 on CS0, reset gpg1-0,
 *                                 MCLK1 = 26 MHz TCXO on the PMU CLKOUT0
 *   i2c@108c0000 (USI07, PERIC1): two MAX98512 amplifiers, 0x39 and 0x3a
 *
 * Addresses and values: audio/analisis/02-codec-amp.md (from the vendor DT
 * and kernel). The bus clocks are the 200 MHz stub of the port; the real
 * block clock is set with the CMU divider, so the bus speed the drivers
 * compute is higher than the real one (slower is safe).
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/machine.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>

static bool codec_dev = true;
module_param(codec_dev, bool, 0444);
MODULE_PARM_DESC(codec_dev, "add the codec@0 node (compatible cirrus,cs47l93)");

static unsigned int spi_div = 7;
module_param(spi_div, uint, 0444);
MODULE_PARM_DESC(spi_div, "USI01 CMU divider (block clock = 200 MHz / (n+1), SPI = that / 4)");

static unsigned int spi_hz = 6000000;
module_param(spi_hz, uint, 0444);
MODULE_PARM_DESC(spi_hz, "spi-max-frequency of the codec");

static struct of_changeset *cs;

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
	pr_info("s9p-audio: %-16s %08llx: %08x -> %08x\n", what, (u64)a, old, new);
}

/* CMU gate: manual mode, clock on (as s9p-reg.py "gate") */
static void gate(phys_addr_t a)
{
	rmw(a, 0, BIT(20) | BIT(21), "gate");
}

/* exynos850-type bank: CON +0, DAT +4, PUD +8, DRV +0xc, 4-bit fields */
static void pin(phys_addr_t bank, int n, u32 func, u32 pud, u32 drv)
{
	u32 sh = n * 4;

	rmw(bank + 0x0, 0xf << sh, func << sh, "pin con");
	rmw(bank + 0x8, 0xf << sh, pud << sh, "pin pud");
	rmw(bank + 0xc, 0xf << sh, drv << sh, "pin drv");
}


/* ------------------------------------------------------------------ */
/* PMIC: the port's built-in s9p-acpm driver owns the ACPM MFD channel  */
/* (S2MPS18 on the APM's SPEEDY bus); share it through its API.         */

int s9p_acpm_pmic_read(u8 reg, u8 *val);
int s9p_acpm_pmic_update(u8 reg, u8 val, u8 mask);

#define PMIC_L34CTRL		0x6b		/* LDO34 = vdd_codec_1p2 */
#define LDO_EN			(0x3 << 6)

/* LDO34 (codec DCVDD/CPVDD2, 1.2 V): sboot leaves it off */
static int codec_power(bool on)
{
	u8 v;
	int ret = s9p_acpm_pmic_read(PMIC_L34CTRL, &v);

	if (ret)
		return ret;
	if (!!(v & LDO_EN) == on) {
		pr_info("s9p-audio: LDO34 (codec 1.2 V) already %s (%02x)\n", on ? "on" : "off", v);
		return 0;
	}
	ret = s9p_acpm_pmic_update(PMIC_L34CTRL, on ? LDO_EN : 0, LDO_EN);
	pr_info("s9p-audio: LDO34 (codec 1.2 V) %s: %d\n", on ? "on" : "off", ret);
	if (!ret && on)
		usleep_range(5000, 6000);	/* enable ramp, stock 5 ms */
	return ret;
}

static void hw_setup(void)
{
	/* PERIC0 USI01 (SPI): gates, divider, protocol, block reset */
	gate(0x10402010);
	gate(0x104020bc);
	gate(0x104020c0);
	gate(0x1040205c);
	rmw(0x10401808, 0xf, spi_div & 0xf, "USI01 div");
	rmw(0x1041100c, 0x7, 0x2, "USI01 SW_CONF");
	rmw(0x104700c4, 0x1, 0, "USI01 USI_CON");
	rmw(0x104700c8, 0, 0x2, "USI01 USI_OPT");
	/* gpp0 @ pinctrl@10430000 + 0x000: 4/5/6 SPI, 7 CS with pull-up */
	pin(0x10430000, 4, 2, 0, 0);
	pin(0x10430000, 5, 2, 0, 0);
	pin(0x10430000, 6, 2, 0, 0);
	/* gpp0-7 (CS) is driven as a GPIO by the SPI core: cs-gpios */

	/* PERIC1 USI07 (I2C): gates, protocol, block reset */
	gate(0x1080203c);
	gate(0x108020e4);
	gate(0x108020e8);
	gate(0x10802094);
	rmw(0x10811024, 0x7, 0x4, "USI07 SW_CONF");
	rmw(0x108c00c4, 0x1, 0, "USI07 USI_CON");
	rmw(0x108c00c8, 0, 0x2, "USI07 USI_OPT");
	/* gpp4 @ pinctrl@10830000 + 0x000: 4/5 = SCL/SDA */
	pin(0x10830000, 4, 2, 0, 0);
	pin(0x10830000, 5, 2, 0, 0);

	/* MCLK1: PMU CLKOUT0 = TCXO (SEL[13:8] = 1), enabled (bit0 = 0) */
	rmw(0x14060a00, 0x3f01, 0x100, "CLKOUT0 (MCLK1)");
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

/* GPIO banks of the port's DT have no phandle: give one to reference them */
static u32 bank_phandle(const char *path)
{
	struct device_node *np = of_find_node_by_path(path), *n;
	u32 max = 0, ph;

	if (!np)
		return 0;
	ph = np->phandle;
	if (!ph) {
		for (n = of_find_all_nodes(NULL); n; n = of_find_all_nodes(n))
			max = max(max, n->phandle);
		ph = max + 1;
		if (u32s(np, "phandle", &ph, 1)) {
			of_node_put(np);
			return 0;
		}
		np->phandle = ph;	/* the property alone does not set it */
	}
	of_node_put(np);
	return ph;
}

static int add_nodes(void)
{
	struct device_node *soc, *stub, *spi, *codec, *i2c;
	u32 ph, gpp0, gpg1, gpa2;
	int ret, i;

	soc = of_find_node_by_path("/soc@0");
	stub = of_find_node_by_path("/clock-hsi2c-stub");
	if (!soc || !stub) {
		ret = -ENODEV;
		goto out;
	}
	ph = stub->phandle;
	cs = kzalloc(sizeof(*cs), GFP_KERNEL);
	if (!cs) {
		ret = -ENOMEM;
		goto out;
	}
	of_changeset_init(cs);

	spi = of_changeset_create_node(cs, soc, "spi@10470000");
	i2c = of_changeset_create_node(cs, soc, "i2c@108c0000");
	if (!spi || !i2c) {
		ret = -ENOMEM;
		goto destroy;
	}
	ret = STR(spi, "compatible", "samsung,exynos850-spi") ?:
	      u32s(spi, "reg", (u32[]){ 0x10470000, 0x100 }, 2) ?:
	      u32s(spi, "interrupts", (u32[]){ 0, 400, 4 }, 3) ?:
	      u32s(spi, "clocks", (u32[]){ ph, ph }, 2) ?:
	      STR(spi, "clock-names", "spi\0spi_busclk0") ?:
	      u32s(spi, "#address-cells", (u32[]){ 1 }, 1) ?:
	      u32s(spi, "#size-cells", (u32[]){ 0 }, 1) ?:
	      STR(spi, "status", "okay");
	if (ret)
		goto destroy;
	gpp0 = bank_phandle("/soc@0/pinctrl@10430000/gpp0-gpio-bank");
	gpg1 = bank_phandle("/soc@0/pinctrl@10430000/gpg1-gpio-bank");
	gpa2 = bank_phandle("/soc@0/pinctrl@14050000/gpa2-gpio-bank");
	if (!gpp0 || !gpg1 || !gpa2) {
		ret = -ENODEV;
		goto destroy;
	}
	/* hardware CS does not reach the pin: GPIO chip select, active low */
	ret = u32s(spi, "cs-gpios", (u32[]){ gpp0, 7, 1 }, 3);
	if (ret)
		goto destroy;
	if (codec_dev) {
		/* binding cirrus,madera; supplies are fixed (LDO33 always on, LDO34 above) */
		codec = of_changeset_create_node(cs, spi, "codec@0");
		ret = !codec ? -ENOMEM :
		      STR(codec, "compatible", "cirrus,cs47l93") ?:
		      u32s(codec, "reg", (u32[]){ 0 }, 1) ?:
		      u32s(codec, "spi-max-frequency", &spi_hz, 1) ?:
		      u32s(codec, "interrupt-parent", &gpa2, 1) ?:
		      u32s(codec, "interrupts", (u32[]){ 2, 8 /* level low */ }, 2) ?:
		      prop(codec, "interrupt-controller", "", 0) ?:
		      u32s(codec, "#interrupt-cells", (u32[]){ 2 }, 1) ?:
		      prop(codec, "gpio-controller", "", 0) ?:
		      u32s(codec, "#gpio-cells", (u32[]){ 2 }, 1) ?:
		      u32s(codec, "#sound-dai-cells", (u32[]){ 1 }, 1) ?:
		      u32s(codec, "reset-gpios", (u32[]){ gpg1, 0, 0 }, 3) ?:
		      STR(codec, "status", "okay");
		if (ret)
			goto destroy;
	}
	ret = STR(i2c, "compatible", "samsung,exynosautov9-hsi2c") ?:
	      u32s(i2c, "reg", (u32[]){ 0x108c0000, 0x1000 }, 2) ?:
	      u32s(i2c, "interrupts", (u32[]){ 0, 430, 4 }, 3) ?:
	      u32s(i2c, "clocks", (u32[]){ ph, ph }, 2) ?:
	      STR(i2c, "clock-names", "hsi2c\0hsi2c_pclk") ?:
	      u32s(i2c, "clock-frequency", (u32[]){ 400000 }, 1) ?:
	      u32s(i2c, "#address-cells", (u32[]){ 1 }, 1) ?:
	      u32s(i2c, "#size-cells", (u32[]){ 0 }, 1) ?:
	      STR(i2c, "status", "okay");
	if (ret)
		goto destroy;
	for (i = 0; i < 2; i++) {
		static const char * const name[] = { "amp@39", "amp@3a" };
		static const char * const prefix[] = { "Left", "Right" };
		struct device_node *amp = of_changeset_create_node(cs, i2c, name[i]);

		ret = !amp ? -ENOMEM :
		      STR(amp, "compatible", "s9p,max98512") ?:
		      u32s(amp, "reg", (u32[]){ 0x39 + i }, 1) ?:
		      prop(amp, "sound-name-prefix", prefix[i], strlen(prefix[i]) + 1) ?:
		      u32s(amp, "s9p,channel", (u32[]){ i }, 1) ?:
		      u32s(amp, "#sound-dai-cells", (u32[]){ 0 }, 1) ?:
		      STR(amp, "status", "okay");
		if (ret)
			goto destroy;
	}
	ret = of_changeset_apply(cs);
	if (ret)
		goto destroy;
	pr_info("s9p-audio: added spi@10470000%s and i2c@108c0000 (amp@39, amp@3a)\n",
		codec_dev ? " (codec@0)" : "");
	goto out;

destroy:
	of_changeset_destroy(cs);
	kfree(cs);
	cs = NULL;
out:
	of_node_put(soc);
	of_node_put(stub);
	return ret;
}

static int __init s9p_audio_board_init(void)
{
	int ret;

	hw_setup();
	ret = codec_power(true);
	if (ret)
		pr_warn("s9p-audio: LDO34 via ACPM failed (%d): the codec will not answer\n", ret);
	return add_nodes();
}

static void __exit s9p_audio_board_exit(void)
{
	of_changeset_revert(cs);
	of_changeset_destroy(cs);
	kfree(cs);
	codec_power(false);
}

module_init(s9p_audio_board_init);
module_exit(s9p_audio_board_exit);
MODULE_DESCRIPTION("Galaxy S9+ audio buses (codec SPI, amplifier I2C)");
MODULE_LICENSE("GPL");
