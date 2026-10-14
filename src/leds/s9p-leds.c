// SPDX-License-Identifier: GPL-2.0
/*
 * Galaxy S9+ (exynos9810): notification LED.
 *
 * The RGB LED is driven by the MAX77705 PMIC. Its MFD core always registers a
 * "max77705-rgb" platform device, but the port's device tree has no "leds"
 * node under pmic@66, so the upstream driver (leds-max77705) finds no LED.
 * This module adds the node with an OF changeset, as in the upstream binding
 * (maxim,max77705.yaml), and hands it to the existing device; then the
 * upstream driver can be loaded.
 *
 * Channels as the vendor's leds-max77705-rgb.c (DT "rgb-name"): LED0 white
 * (not fitted on this phone), LED1 red, LED2 green, LED3 blue.
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define PMIC_NODE	"/soc@0/i2c@14360000/pmic@66"
#define LED_COLOR_ID_RED	1
#define LED_COLOR_ID_GREEN	2
#define LED_COLOR_ID_BLUE	3
#define LED_COLOR_ID_RGB	9

static char *trigger = "max170xx_battery-charging-orange-full-green";
module_param(trigger, charp, 0444);
MODULE_PARM_DESC(trigger, "default trigger (empty: none); default: orange charging, green full");

static struct of_changeset cs;
static struct device *rgb_dev;
static struct device_node *leds_np;

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

static int u32p(struct device_node *np, const char *name, u32 v)
{
	__be32 b = cpu_to_be32(v);

	return prop(np, name, &b, 4);
}

#define STR(np, name, s)	prop(np, name, s, sizeof(s))

static int add_nodes(struct device_node *pmic)
{
	static const struct { const char *name; u32 reg, color; } ch[] = {
		{ "led@1", 1, LED_COLOR_ID_RED },
		{ "led@2", 2, LED_COLOR_ID_GREEN },
		{ "led@3", 3, LED_COLOR_ID_BLUE },
	};
	struct device_node *multi, *led;
	int i, ret;

	of_changeset_init(&cs);
	leds_np = of_changeset_create_node(&cs, pmic, "leds");
	multi = leds_np ? of_changeset_create_node(&cs, leds_np, "multi-led@0") : NULL;
	if (!multi)
		return -ENOMEM;
	ret = STR(leds_np, "compatible", "maxim,max77705-rgb") ?:
	      u32p(leds_np, "#address-cells", 1) ?:
	      u32p(leds_np, "#size-cells", 0) ?:
	      /* the driver wants a "reg" on the RGB node too, unlike the binding example */
	      u32p(multi, "reg", 0) ?:
	      u32p(multi, "color", LED_COLOR_ID_RGB) ?:
	      STR(multi, "function", "status") ?:
	      (trigger && *trigger ? prop(multi, "linux,default-trigger", trigger,
					  strlen(trigger) + 1) : 0) ?:
	      u32p(multi, "#address-cells", 1) ?:
	      u32p(multi, "#size-cells", 0);
	/*
	 * Created in reverse: the changeset prepends children, and the
	 * multicolor triggers expect red, green, blue in this order.
	 */
	for (i = ARRAY_SIZE(ch) - 1; !ret && i >= 0; i--) {
		led = of_changeset_create_node(&cs, multi, ch[i].name);
		ret = !led ? -ENOMEM :
		      u32p(led, "reg", ch[i].reg) ?:
		      u32p(led, "color", ch[i].color);
	}
	return ret ?: of_changeset_apply(&cs);
}

static int __init s9p_leds_init(void)
{
	struct device_node *pmic = of_find_node_by_path(PMIC_NODE);
	int ret;

	if (!pmic)
		return -ENODEV;
	rgb_dev = bus_find_device_by_name(&platform_bus_type, NULL, "max77705-rgb");
	if (!rgb_dev) {
		ret = -ENODEV;
		goto out;
	}
	if (rgb_dev->driver) {
		pr_info("s9p-leds: max77705-rgb already has a driver\n");
		ret = -EBUSY;
		goto put;
	}
	ret = add_nodes(pmic);
	if (ret) {
		of_changeset_destroy(&cs);
		goto put;
	}
	device_set_node(rgb_dev, of_fwnode_handle(leds_np));
	/* the upstream driver binds once loaded; if it already is, probe now */
	if (device_attach(rgb_dev) < 0)
		pr_warn("s9p-leds: attach failed\n");
	pr_info("s9p-leds: %pOF added to max77705-rgb (trigger %s)\n", leds_np,
		trigger && *trigger ? trigger : "none");
	of_node_put(pmic);
	return 0;
put:
	put_device(rgb_dev);
	rgb_dev = NULL;
out:
	of_node_put(pmic);
	return ret;
}

static void __exit s9p_leds_exit(void)
{
	device_release_driver(rgb_dev);
	device_set_node(rgb_dev, NULL);
	of_changeset_revert(&cs);
	of_changeset_destroy(&cs);
	put_device(rgb_dev);
}

module_init(s9p_leds_init);
module_exit(s9p_leds_exit);
MODULE_DESCRIPTION("Galaxy S9+ notification LED node for the MAX77705");
MODULE_LICENSE("GPL");
