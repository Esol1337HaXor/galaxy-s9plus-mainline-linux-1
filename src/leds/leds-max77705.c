// SPDX-License-Identifier: GPL-2.0
/*
 * Based on leds-max77650 driver
 *
 * LED driver for MAXIM 77705 PMIC.
 * Copyright (C) 2025 Dzmitry Sankouski <dsankouski@gmail.org>
 *
 * Galaxy S9+ port: upstream drivers/leds/leds-max77705.c (Linux 7.1) plus a
 * current ceiling. The brightness registers are the LED current; at 0xff
 * the LED is blinding. Full brightness maps to max_current (the vendor's
 * led_dynamic_current, 0x14).
 *
 * Hardware blinking is fixed here. Upstream's blink_set found its LED with
 * container_of(cdev, ..., cdev), but an RGB LED registers mcdev.led_cdev:
 * the regmap it read was NULL and the timer trigger oopsed with the LED's
 * trigger lock held, so every later write to the LED hung. Probe turned the
 * LED off through the same wrong pointer. It also never put
 * the channels in blink mode (LEDEN 0b10, as the vendor driver does), and
 * its delay_off steps from 5 s on came out negative. And the mainline header
 * numbers the registers without gaps (LEDRMP 0x35, LEDBLNK 0x36), while the
 * chip, as Samsung's driver for this phone has it, keeps LEDRMP at 0x36 and
 * LEDBLNK at 0x38: blink times landed in the ramp register.
 *
 * The PMIC runs from the battery, so whatever the LED shows outlives Linux:
 * after a power off it stayed lit until the next boot reached this driver.
 * Shutdown and reboot now put it out.
 */

#include <linux/i2c.h>
#include <linux/led-class-multicolor.h>
#include <linux/leds.h>
#include <linux/mfd/max77705-private.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define MAX77705_LED_NUM_LEDS			4
#define MAX77705_LED_EN_MASK			GENMASK(1, 0)
#define MAX77705_LED_MAX_BRIGHTNESS		0xff
#define MAX77705_LED_EN_SHIFT(reg)		(reg * MAX77705_RGBLED_EN_WIDTH)
#define MAX77705_LED_REG_BRIGHTNESS(reg)	(reg + MAX77705_RGBLED_REG_LED0BRT)
/* Offsets from MAX77705_RGBLED_REG_BASE (0x30), as the vendor driver has them */
#define MAX77705_LED_REG_LEDRMP			0x06
#define MAX77705_LED_REG_LEDBLNK		0x08
/* LEDEN, two bits per channel */
#define MAX77705_LED_MODE_ON			1
#define MAX77705_LED_MODE_BLINK			2

static unsigned int max_current = 0x14;
module_param(max_current, uint, 0644);
MODULE_PARM_DESC(max_current, "LED current register at full brightness (1..255)");

struct max77705_led {
	struct led_classdev cdev;
	struct led_classdev_mc mcdev;
	struct regmap *regmap;

	struct mc_subled *subled_info;
	/* hardware blinking is set: lit channels blink instead of glowing */
	bool blinking;
};

static const struct regmap_config max77705_leds_regmap_config = {
	.reg_base = MAX77705_RGBLED_REG_BASE,
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = MAX77705_LED_REG_LEDBLNK,
};

/* LEDBLNK: on time in the high nibble, off time in the low one (vendor table). */
static unsigned int max77705_blink_on(unsigned long ms)
{
	if (ms < MAX77705_RGB_DELAY_100_STEP)
		return 0;
	if (ms < MAX77705_RGB_DELAY_100_STEP_LIM)
		return ms / MAX77705_RGB_DELAY_100_STEP - 1;
	if (ms < MAX77705_RGB_DELAY_250_STEP_LIM)
		return (ms - MAX77705_RGB_DELAY_100_STEP_LIM) / MAX77705_RGB_DELAY_250_STEP +
			MAX77705_RGB_DELAY_100_STEP_COUNT;
	return 15;
}

static unsigned int max77705_blink_off(unsigned long ms)
{
	if (ms < MAX77705_RGB_DELAY_500_STEP)
		return 1;
	if (ms < MAX77705_RGB_DELAY_500_STEP_LIM)
		return ms / MAX77705_RGB_DELAY_500_STEP;
	if (ms < MAX77705_RGB_DELAY_1000_STEP_LIM)
		return (ms - MAX77705_RGB_DELAY_500_STEP_LIM) / MAX77705_RGB_DELAY_1000_STEP +
			MAX77705_RGB_DELAY_500_STEP_COUNT;
	if (ms < MAX77705_RGB_DELAY_2000_STEP_LIM)
		return (ms - MAX77705_RGB_DELAY_1000_STEP_LIM) / MAX77705_RGB_DELAY_2000_STEP +
			MAX77705_RGB_DELAY_1000_STEP_COUNT;
	return 15;
}

static int max77705_led_blink(struct max77705_led *led, struct led_classdev *cdev,
			      unsigned long *delay_on, unsigned long *delay_off)
{
	int ret;

	if (!*delay_on && !*delay_off)
		*delay_on = *delay_off = 500;
	/* Always on or always off is not blinking: the core does those. */
	if (!*delay_on || !*delay_off)
		return -EINVAL;

	ret = regmap_write(led->regmap, MAX77705_LED_REG_LEDBLNK,
			   max77705_blink_on(*delay_on) << 4 | max77705_blink_off(*delay_off));
	if (!ret)	/* sharp edges: no ramp up or down */
		ret = regmap_write(led->regmap, MAX77705_LED_REG_LEDRMP, 0);
	if (ret)
		return ret;

	/* Relight in blink mode; a dark LED blinks at full brightness. */
	led->blinking = true;
	return cdev->brightness_set_blocking(cdev, cdev->brightness ?: cdev->max_brightness);
}

static int max77705_rgb_blink_single(struct led_classdev *cdev,
				     unsigned long *delay_on, unsigned long *delay_off)
{
	return max77705_led_blink(container_of(cdev, struct max77705_led, cdev), cdev,
				  delay_on, delay_off);
}

static int max77705_rgb_blink_multi(struct led_classdev *cdev,
				    unsigned long *delay_on, unsigned long *delay_off)
{
	struct led_classdev_mc *mcdev = lcdev_to_mccdev(cdev);

	return max77705_led_blink(container_of(mcdev, struct max77705_led, mcdev), cdev,
				  delay_on, delay_off);
}

static int max77705_led_brightness_set(struct max77705_led *led, struct mc_subled *subled,
				int num_colors)
{
	unsigned int mode = led->blinking ? MAX77705_LED_MODE_BLINK : MAX77705_LED_MODE_ON;
	unsigned int mask = 0, enable = 0;
	int ret;

	for (int i = 0; i < num_colors; i++) {
		unsigned int channel, brightness;

		channel = subled[i].channel;
		brightness = subled[i].brightness;
		mask |= MAX77705_LED_EN_MASK << MAX77705_LED_EN_SHIFT(channel);
		if (brightness == LED_OFF)
			continue;

		/* Set current */
		brightness = max(1U, DIV_ROUND_UP(brightness * clamp(max_current, 1U, 255U),
						   MAX77705_LED_MAX_BRIGHTNESS));
		ret = regmap_write(led->regmap, MAX77705_LED_REG_BRIGHTNESS(channel), brightness);
		if (ret < 0)
			return ret;
		enable |= mode << MAX77705_LED_EN_SHIFT(channel);
	}

	/* All channels in one write, so blinking ones start in step. */
	return regmap_update_bits(led->regmap, MAX77705_RGBLED_REG_LEDEN, mask, enable);
}

static int max77705_led_brightness_set_single(struct led_classdev *cdev,
					enum led_brightness brightness)
{
	struct max77705_led *led = container_of(cdev, struct max77705_led, cdev);

	led->subled_info->brightness = brightness;
	/* Turning the LED off also ends hardware blinking. */
	if (brightness == LED_OFF)
		led->blinking = false;

	return max77705_led_brightness_set(led, led->subled_info, 1);
}

static int max77705_led_brightness_set_multi(struct led_classdev *cdev,
					enum led_brightness brightness)
{
	struct led_classdev_mc *mcdev = lcdev_to_mccdev(cdev);
	struct max77705_led *led = container_of(mcdev, struct max77705_led, mcdev);

	led_mc_calc_color_components(mcdev, brightness);
	/* Turning the LED off also ends hardware blinking. */
	if (brightness == LED_OFF)
		led->blinking = false;

	return max77705_led_brightness_set(led, led->mcdev.subled_info, mcdev->num_colors);
}

static int max77705_parse_subled(struct device *dev, struct fwnode_handle *np,
				struct mc_subled *info)
{
	u32 color = LED_COLOR_ID_GREEN;
	u32 reg;
	int ret;

	ret = fwnode_property_read_u32(np, "reg", &reg);
	if (ret || !reg || reg >= MAX77705_LED_NUM_LEDS)
		return dev_err_probe(dev, -EINVAL, "invalid \"reg\" of %pOFn\n", np);

	info->channel = reg;

	ret = fwnode_property_read_u32(np, "color", &color);
	if (ret < 0 && ret != -EINVAL)
		return dev_err_probe(dev, ret,
				     "failed to parse \"color\" of %pOF\n", np);

	info->color_index = color;

	return 0;
}

static int max77705_add_led(struct device *dev, struct regmap *regmap, struct fwnode_handle *np)
{
	int ret, i = 0;
	unsigned int color, reg;
	struct max77705_led *led;
	struct led_classdev *cdev;
	struct mc_subled *info;
	struct fwnode_handle *child;
	struct led_init_data init_data = {};

	led = devm_kzalloc(dev, sizeof(*led), GFP_KERNEL);
	if (!led)
		return -ENOMEM;

	ret = fwnode_property_read_u32(np, "color", &color);
	if (ret < 0 && ret != -EINVAL)
		return dev_err_probe(dev, ret,
			      "failed to parse \"color\" of %pOF\n", np);

	led->regmap = regmap;
	init_data.fwnode = np;

	if (color == LED_COLOR_ID_RGB) {
		int num_channels = of_get_available_child_count(to_of_node(np));

		ret = fwnode_property_read_u32(np, "reg", &reg);
		if (ret || reg >= MAX77705_LED_NUM_LEDS)
			return -EINVAL;

		info = devm_kcalloc(dev, num_channels, sizeof(*info), GFP_KERNEL);
		if (!info)
			return -ENOMEM;

		cdev = &led->mcdev.led_cdev;
		cdev->max_brightness = MAX77705_LED_MAX_BRIGHTNESS;
		cdev->brightness_set_blocking = max77705_led_brightness_set_multi;
		cdev->blink_set = max77705_rgb_blink_multi;

		fwnode_for_each_child_node(np, child) {
			ret = max77705_parse_subled(dev, child, &info[i]);
			if (ret < 0)
				return ret;

			info[i].intensity = 0;
			i++;
		}

		led->mcdev.subled_info = info;
		led->mcdev.num_colors = num_channels;

		ret = devm_led_classdev_multicolor_register_ext(dev, &led->mcdev, &init_data);
		if (ret)
			return ret;

		/* The registered classdev, not the unused single-colour one. */
		ret = max77705_led_brightness_set_multi(cdev, LED_OFF);
		if (ret)
			return ret;
	} else {
		info = devm_kzalloc(dev, sizeof(*info), GFP_KERNEL);
		if (!info)
			return -ENOMEM;

		max77705_parse_subled(dev, np, info);

		led->subled_info = info;
		led->cdev.brightness_set_blocking = max77705_led_brightness_set_single;
		led->cdev.blink_set = max77705_rgb_blink_single;
		led->cdev.max_brightness = MAX77705_LED_MAX_BRIGHTNESS;

		ret = devm_led_classdev_register_ext(dev, &led->cdev, &init_data);
		if (ret)
			return ret;

		ret = max77705_led_brightness_set_single(&led->cdev, LED_OFF);
		if (ret)
			return ret;
	}

	return 0;
}

static int max77705_led_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct i2c_client *i2c = to_i2c_client(pdev->dev.parent);
	struct regmap *regmap;
	int ret;

	regmap = devm_regmap_init_i2c(i2c, &max77705_leds_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap), "Failed to register LEDs regmap\n");

	platform_set_drvdata(pdev, regmap);

	device_for_each_child_node_scoped(dev, child) {
		ret = max77705_add_led(dev, regmap, child);
		if (ret)
			return ret;
	}

	return 0;
}

/* every channel off and no blinking: the LED is dark until Linux lights it */
static void max77705_led_shutdown(struct platform_device *pdev)
{
	struct regmap *regmap = platform_get_drvdata(pdev);

	if (!regmap)
		return;
	regmap_write(regmap, MAX77705_RGBLED_REG_LEDEN, 0);
	regmap_write(regmap, MAX77705_LED_REG_LEDBLNK, 0);
}

static const struct of_device_id max77705_led_of_match[] = {
	{ .compatible = "maxim,max77705-rgb" },
	{ }
};
MODULE_DEVICE_TABLE(of, max77705_led_of_match);

static struct platform_driver max77705_led_driver = {
	.driver = {
		.name = "max77705-led",
		.of_match_table = max77705_led_of_match,
	},
	.probe = max77705_led_probe,
	.shutdown = max77705_led_shutdown,
};
module_platform_driver(max77705_led_driver);

MODULE_DESCRIPTION("Maxim MAX77705 LED driver");
MODULE_AUTHOR("Dzmitry Sankouski <dsankouski@gmail.com>");
MODULE_LICENSE("GPL");
