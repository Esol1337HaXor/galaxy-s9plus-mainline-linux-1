/* SPDX-License-Identifier: GPL-2.0 */
/* The SS310AP is on-chip: no GPIOs are wired; keep the vendor calls compiling. */
#ifndef S9P_OF_GPIO_H
#define S9P_OF_GPIO_H
#include <linux/gpio.h>
#include <linux/of.h>
static inline int of_get_named_gpio(const struct device_node *np, const char *name, int idx)
{
	return -ENOENT;
}
static inline int of_gpio_count(const struct device_node *np) { return 0; }
static inline int of_get_gpio(const struct device_node *np, int idx) { return -ENOENT; }
#endif
