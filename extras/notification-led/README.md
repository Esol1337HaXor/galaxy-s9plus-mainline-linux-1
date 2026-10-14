# notification-led

The RGB notification LED: orange while charging, green when full.

The LED sits on the MAX77705 PMIC. The port's device tree does not describe it,
so `s9p-leds` adds the node the upstream driver expects and loads a copy of
upstream `leds-max77705` with a current ceiling (full brightness = the current
Samsung uses; at the register's maximum the LED is blinding).

It shows up as `/sys/class/leds/rgb:status` (a multicolor LED, channels red,
green, blue). The default trigger follows the battery; change it like any LED:

```bash
sudo extras/install.sh notification-led
L=/sys/class/leds/rgb:status
echo none > $L/trigger; echo 0 0 255 > $L/multi_intensity; echo 255 > $L/brightness  # blue
echo timer > $L/trigger                                                             # blinking
```

Options in `/etc/modprobe.d/s9p-leds.conf`: the default trigger
(`s9p_leds trigger=...`, empty for none) and the current at full brightness
(`leds_max77705 max_current=20`, 1..255). Needs `scripts/build/build_extras.sh`.
