// SPDX-License-Identifier: GPL-2.0
/*
 * Galaxy S9+ (exynos9810): Bluetooth of the BCM4361 combo on UART1.
 *
 * The port's device tree has no UART for it, so (as s9p-audio-board.c does
 * for the audio buses) this module opens the clock gates, takes the block's
 * USI out of reset, programs the pin mux by hand and adds the UART with an
 * OF changeset. Its "bluetooth" child makes samsung_tty hand the port to
 * serdev, where upstream hci_bcm powers the chip and loads its firmware.
 *
 *   uart@10840000 (UART_BT, PERIC1), GIC SPI 422, 256-byte FIFO
 *   gpq0-0..3     RXD, TXD, RTS, CTS (function 2, pull-up)
 *   BT_REG_ON     PCAL6524 @0x22 line 7 (the expander s9p-dp adds)
 *   BT_WAKE       gpg2-0, active high
 *   HOST_WAKE     gpa2-3, active high
 *
 * The chip has no address of its own (43:61:B2:00:1F:AC on every phone);
 * Samsung keeps it in EFS. extras/bluetooth passes it as bd_addr, which goes
 * to "local-bd-address" (hci_bcm sets HCI_QUIRK_USE_BDADDR_PROPERTY).
 *
 * Addresses and values: bluetooth/analisis/01-bt.md (from the vendor DT,
 * drivers/bluetooth/bcm43xx.c and drivers/tty/serial/samsung.c). The UART
 * clock is the port's 200 MHz stub, which is also the real block clock
 * (CMU divider at /1), so the baud rates samsung_tty computes are exact.
 */

#include <linux/delay.h>
#include <linux/gpio/driver.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>

static bool serdev = true;
module_param(serdev, bool, 0444);
MODULE_PARM_DESC(serdev, "add the bluetooth child (hci_bcm); off: plain /dev/ttySAC* for btattach");

static unsigned int max_speed = 3000000;
module_param(max_speed, uint, 0444);
MODULE_PARM_DESC(max_speed, "UART speed after the firmware is loaded");

static char *bd_addr;
module_param(bd_addr, charp, 0444);
MODULE_PARM_DESC(bd_addr, "the phone's Bluetooth address (EFS bluetooth/bt_addr); the chip boots with 43:61:B2:..");

static bool host_wake = true;
module_param(host_wake, bool, 0444);
MODULE_PARM_DESC(host_wake, "host wake-up line: lets the chip sleep (runtime PM)");

#define EXPANDER	"/soc@0/i2c@10930000/gpio@22"

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
	pr_debug("s9p-bt: %-16s %08llx: %08x -> %08x\n", what, (u64)a, old, new);
}

/* CMU gate: manual mode, clock on */
static void gate(phys_addr_t a)
{
	rmw(a, 0, BIT(20) | BIT(21), "gate");
}

/* exynos850-type bank: CON +0, DAT +4, PUD +8, 4-bit fields */
static void pin(phys_addr_t bank, int n, u32 func, u32 pud)
{
	u32 sh = n * 4;

	rmw(bank + 0x0, 0xf << sh, func << sh, "pin con");
	rmw(bank + 0x8, 0xf << sh, pud << sh, "pin pud");
}

static void hw_setup(void)
{
	u32 div;
	int i;

	/* CMU top PERIC1 (s9p-dp opens them too), then UART_BT in CMU_PERIC1 */
	gate(0x1a2420c4);
	gate(0x1a2420c0);
	gate(0x10802034);
	gate(0x108020c4);
	gate(0x108020c8);
	gate(0x1080201c);
	div = rd(0x10801814) & 0xf;
	if (div)
		pr_warn("s9p-bt: UART_BT divider is /%u, the baud rates will be off\n", div + 1);
	/* the block's USI: reset pulse, clock request on (vendor exynos_usi_init) */
	wr(0x108400c4, 0x1);
	udelay(1);
	wr(0x108400c4, 0x0);
	udelay(1);
	wr(0x108400c8, 0x2);
	/* gpq0 @ pinctrl@14050000 + 0xa0: 0..3 = RXD TXD RTS CTS, pull-up */
	for (i = 0; i < 4; i++)
		pin(0x140500a0, i, 2, 3);
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

/* "aa:bb:cc:dd:ee:ff" -> local-bd-address, little endian */
static int bdaddr_prop(struct device_node *np)
{
	unsigned int b[6];
	u8 le[6];
	int i;

	if (!bd_addr || !*bd_addr)
		return 0;
	if (sscanf(bd_addr, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
		pr_warn("s9p-bt: bd_addr \"%s\" is not aa:bb:cc:dd:ee:ff, ignored\n", bd_addr);
		return 0;
	}
	for (i = 0; i < 6; i++)
		le[i] = b[5 - i];
	return prop(np, "local-bd-address", le, sizeof(le));
}

/* GPIO banks of the port's DT have no phandle: give one to reference them */
static u32 node_phandle(const char *path)
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

/* BT_REG_ON sits on the expander s9p-dp creates: wait for its gpiochip */
static int wait_expander(void)
{
	struct device_node *np;
	struct gpio_device *gdev = NULL;
	int i;

	for (i = 0; i < 100 && !gdev; i++) {
		np = of_find_node_by_path(EXPANDER);
		if (np) {
			gdev = gpio_device_find_by_fwnode(of_fwnode_handle(np));
			of_node_put(np);
		}
		if (!gdev)
			msleep(100);
	}
	if (!gdev) {
		pr_err("s9p-bt: no PCAL6524 (%s): load s9p-dp first\n", EXPANDER);
		return -ENODEV;
	}
	gpio_device_put(gdev);
	return 0;
}

static int add_nodes(void)
{
	struct device_node *soc, *stub, *uart, *bt;
	u32 ph, exp, gpg2, gpa2;
	int ret;

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

	uart = of_changeset_create_node(cs, soc, "serial@10840000");
	ret = !uart ? -ENOMEM :
	      STR(uart, "compatible", "samsung,exynos8895-uart") ?:
	      u32s(uart, "reg", (u32[]){ 0x10840000, 0x100 }, 2) ?:
	      u32s(uart, "interrupts", (u32[]){ 0, 422, 4 }, 3) ?:
	      u32s(uart, "clocks", (u32[]){ ph, ph }, 2) ?:
	      STR(uart, "clock-names", "uart\0clk_uart_baud0") ?:
	      u32s(uart, "samsung,uart-fifosize", (u32[]){ 256 }, 1) ?:
	      STR(uart, "status", "okay");
	if (ret)
		goto destroy;
	if (serdev) {
		exp = node_phandle(EXPANDER);
		gpg2 = node_phandle("/soc@0/pinctrl@10430000/gpg2-gpio-bank");
		gpa2 = node_phandle("/soc@0/pinctrl@14050000/gpa2-gpio-bank");
		if (!exp || !gpg2 || !gpa2) {
			ret = -ENODEV;
			goto destroy;
		}
		/* no compatible for the 4361 upstream; bcm4345c5 sets no quirks */
		bt = of_changeset_create_node(cs, uart, "bluetooth");
		ret = !bt ? -ENOMEM :
		      STR(bt, "compatible", "brcm,bcm4345c5") ?:
		      u32s(bt, "shutdown-gpios", (u32[]){ exp, 7, 0 }, 3) ?:
		      u32s(bt, "device-wakeup-gpios", (u32[]){ gpg2, 0, 0 }, 3) ?:
		      (host_wake ? u32s(bt, "host-wakeup-gpios", (u32[]){ gpa2, 3, 0 }, 3) : 0) ?:
		      u32s(bt, "max-speed", &max_speed, 1) ?:
		      bdaddr_prop(bt);
		if (ret)
			goto destroy;
	}
	ret = of_changeset_apply(cs);
	if (ret)
		goto destroy;
	pr_info("s9p-bt: added serial@10840000%s\n", serdev ? " + bluetooth (hci_bcm)" : "");
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

static int __init s9p_bt_board_init(void)
{
	int ret = serdev ? wait_expander() : 0;

	if (ret)
		return ret;
	hw_setup();
	return add_nodes();
}

static void __exit s9p_bt_board_exit(void)
{
	of_changeset_revert(cs);
	of_changeset_destroy(cs);
	kfree(cs);
}

module_init(s9p_bt_board_init);
module_exit(s9p_bt_board_exit);
MODULE_DESCRIPTION("Galaxy S9+ Bluetooth UART (BCM4361)");
MODULE_LICENSE("GPL");
