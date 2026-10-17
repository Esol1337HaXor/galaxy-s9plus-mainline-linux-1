// SPDX-License-Identifier: GPL-2.0
/*
 * Galaxy S9+ (exynos9810): the rear flash LED as a torch, "white:flash".
 *
 * The LED hangs off the S2MPB02 camera PMIC, 0x59 on hsi2c@10920000 (USI10),
 * which the port's device tree does not have. Like s9p-cam-board.c, this
 * opens the bus's clock gates, selects the I2C protocol and its pins, and
 * adds the bus with an OF changeset, plus a node for the PMIC that the I2C
 * driver below binds to. With s9p-cam-board loaded first, the bus is there
 * and only the PMIC node is added.
 *
 * The torch is the vendor's I2C way (VK drivers/leds/leds-s2mpb02.c,
 * include/linux/leds-s2mpb02.h; registers from s2mpb02-private.h, enum from
 * 0): FLED_CUR1 (0x14) low nibble = torch current step, FLED_CTRL1 (0x12)
 * bits 7:6 = enable + torch mode, bit 3 (low-voltage mode) clear. Off clears
 * bits 7:6. The flash half (high nibble, flash mode), FLED2 (the iris IR LED)
 * and the GPIO-driven strobe are left alone.
 *
 * Brightness 1-255 maps to torch steps 1..TORCH_MAX: Samsung's torch levels
 * go up to step 9 (torch_table of the vendor DT).
 */

#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>

#define BUS_PATH	"/soc@0/i2c@10920000"
#define COMPATIBLE	"s9p,s2mpb02-torch"
#define REG_FLED_CTRL1	0x12
#define REG_FLED_CUR1	0x14
#define CTRL1_ON_TORCH	0xc0	/* enable, torch mode */
#define CTRL1_MODE	0xc0
#define CTRL1_LV	0x08
#define CUR1_TORCH	0x0f
#define TORCH_MAX	9

/* ---- the bus (as s9p-cam-board.c) ---- */

static struct of_changeset *cs;

static void rmw(phys_addr_t a, u32 clr, u32 set)
{
	void __iomem *p = ioremap(a & ~0xfffUL, 0x1000);

	if (!p)
		return;
	writel((readl(p + (a & 0xfff)) & ~clr) | set, p + (a & 0xfff));
	iounmap(p);
}

/* CMU gate: manual mode, clock on */
static void gate(phys_addr_t a)
{
	rmw(a, 0, BIT(20) | BIT(21));
}

#define GPP6	0x10830040	/* pinctrl@10830000, exynos850-type bank */

static void bus_setup(void)
{
	gate(0x1a2420c0);		/* CMU_TOP PERIC1 */
	gate(0x1a2420c4);
	gate(0x10802048);		/* USI10 */
	gate(0x10802114);
	gate(0x10802118);
	gate(0x108020ac);
	rmw(0x1081103c, 0x7, 0x4);	/* USI10 SW_CONF: I2C */
	rmw(0x109200c4, 0x1, 0);	/* USI_CON: out of reset */
	rmw(0x109200c8, 0, 0x2);	/* USI_OPTION: CLKREQ_ON */
	rmw(GPP6 + 0x0, 0xff, 0x22);	/* gpp6-0/1: hsi2c function */
	rmw(GPP6 + 0x8, 0xff, 0);	/* no pulls */
}

static int prop(struct device_node *np, const char *name, const void *v, int len)
{
	struct property *p = kzalloc(sizeof(*p), GFP_KERNEL);

	if (!p)
		return -ENOMEM;
	p->name = kstrdup(name, GFP_KERNEL);
	p->value = kmemdup(v, len, GFP_KERNEL);
	p->length = len;
	if (!p->name || !p->value) {
		kfree(p->name);
		kfree(p->value);
		kfree(p);
		return -ENOMEM;
	}
	return of_changeset_add_property(cs, np, p);
}

static int u32s(struct device_node *np, const char *name, const u32 *v, int n)
{
	__be32 b[4];
	int i;

	for (i = 0; i < n; i++)
		b[i] = cpu_to_be32(v[i]);
	return prop(np, name, b, n * 4);
}

#define STR(np, name, s)	prop(np, name, s, sizeof(s))

static int add_nodes(void)
{
	struct device_node *soc = NULL, *stub = NULL, *found, *bus, *pmic;
	int ret = 0;

	cs = kzalloc(sizeof(*cs), GFP_KERNEL);
	if (!cs)
		return -ENOMEM;
	of_changeset_init(cs);
	bus = found = of_find_node_by_path(BUS_PATH);
	if (!bus) {
		soc = of_find_node_by_path("/soc@0");
		stub = of_find_node_by_path("/clock-hsi2c-stub");
		if (!soc || !stub) {
			ret = -ENODEV;
			goto out;
		}
		bus = of_changeset_create_node(cs, soc, "i2c@10920000");
		ret = !bus ? -ENOMEM :
		      STR(bus, "compatible", "samsung,exynosautov9-hsi2c") ?:
		      u32s(bus, "reg", (u32[]){ 0x10920000, 0x1000 }, 2) ?:
		      u32s(bus, "interrupts", (u32[]){ 0, 436, 4 }, 3) ?:
		      u32s(bus, "clocks", (u32[]){ stub->phandle, stub->phandle }, 2) ?:
		      STR(bus, "clock-names", "hsi2c\0hsi2c_pclk") ?:
		      u32s(bus, "clock-frequency", (u32[]){ 400000 }, 1) ?:
		      u32s(bus, "#address-cells", (u32[]){ 1 }, 1) ?:
		      u32s(bus, "#size-cells", (u32[]){ 0 }, 1) ?:
		      STR(bus, "status", "okay");
		if (ret)
			goto out;
	}
	pmic = of_changeset_create_node(cs, bus, "pmic@59");
	ret = !pmic ? -ENOMEM :
	      STR(pmic, "compatible", COMPATIBLE) ?:
	      u32s(pmic, "reg", (u32[]){ 0x59 }, 1) ?:
	      of_changeset_apply(cs);
out:
	of_node_put(found);
	of_node_put(soc);
	of_node_put(stub);
	if (ret) {
		of_changeset_destroy(cs);
		kfree(cs);
		cs = NULL;
	}
	return ret;
}

/* ---- the torch ---- */

struct torch {
	struct led_classdev led;
	struct i2c_client *client;
};

static int torch_set(struct led_classdev *led, enum led_brightness value)
{
	struct torch *t = container_of(led, struct torch, led);
	struct i2c_client *c = t->client;
	int ctrl, cur, ret;

	ctrl = i2c_smbus_read_byte_data(c, REG_FLED_CTRL1);
	if (ctrl < 0)
		return ctrl;
	if (value == LED_OFF)
		return i2c_smbus_write_byte_data(c, REG_FLED_CTRL1, ctrl & ~CTRL1_MODE);

	cur = i2c_smbus_read_byte_data(c, REG_FLED_CUR1);
	if (cur < 0)
		return cur;
	cur = (cur & ~CUR1_TORCH) | (1 + (value - 1) * (TORCH_MAX - 1) / (LED_FULL - 1));
	ret = i2c_smbus_write_byte_data(c, REG_FLED_CUR1, cur);
	if (ret)
		return ret;
	return i2c_smbus_write_byte_data(c, REG_FLED_CTRL1,
					 (ctrl & ~(CTRL1_MODE | CTRL1_LV)) | CTRL1_ON_TORCH);
}

static int torch_probe(struct i2c_client *client)
{
	struct torch *t;
	int ret;

	/* the PMIC answers, or this is not the bus it should be */
	ret = i2c_smbus_read_byte_data(client, REG_FLED_CTRL1);
	if (ret < 0)
		return dev_err_probe(&client->dev, ret, "S2MPB02 does not answer\n");
	t = devm_kzalloc(&client->dev, sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;
	t->client = client;
	t->led.name = "white:flash";
	t->led.max_brightness = LED_FULL;
	t->led.brightness_set_blocking = torch_set;
	/* whatever the bootloader or a previous load left: off */
	torch_set(&t->led, LED_OFF);
	i2c_set_clientdata(client, t);
	return devm_led_classdev_register(&client->dev, &t->led);
}

static void torch_shutdown(struct i2c_client *client)
{
	struct torch *t = i2c_get_clientdata(client);

	if (t)
		torch_set(&t->led, LED_OFF);
}

static void torch_remove(struct i2c_client *client)
{
	torch_shutdown(client);
}

static const struct of_device_id torch_of_match[] = {
	{ .compatible = COMPATIBLE },
	{ }
};
MODULE_DEVICE_TABLE(of, torch_of_match);

static struct i2c_driver torch_driver = {
	.driver = {
		.name = "s9p-flash",
		.of_match_table = torch_of_match,
	},
	.probe = torch_probe,
	.remove = torch_remove,
	.shutdown = torch_shutdown,
};

static int __init s9p_flash_init(void)
{
	int ret;

	bus_setup();
	ret = i2c_add_driver(&torch_driver);
	if (ret)
		return ret;
	ret = add_nodes();
	if (ret)
		i2c_del_driver(&torch_driver);
	return ret;
}

static void __exit s9p_flash_exit(void)
{
	i2c_del_driver(&torch_driver);
	if (cs) {
		of_changeset_revert(cs);
		of_changeset_destroy(cs);
		kfree(cs);
	}
}

module_init(s9p_flash_init);
module_exit(s9p_flash_exit);
MODULE_DESCRIPTION("Galaxy S9+ rear flash LED as a torch (S2MPB02)");
MODULE_LICENSE("GPL");
