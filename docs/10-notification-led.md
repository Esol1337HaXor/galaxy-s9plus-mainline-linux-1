# Notification LED

The RGB LED above the screen is driven by the **MAX77705** PMIC (I2C bus 1,
0x66), the same chip as the charger and the Type-C controller. Mainline has a
driver for it (`drivers/leds/leds-max77705.c`) and the MFD core always
registers its `max77705-rgb` platform device, but the port's device tree has
no `leds` node under `pmic@66`, so nothing binds.

`src/leds/` fixes that without touching the kernel image:

| module | does |
|---|---|
| `s9p-leds` | adds `pmic@66/leds` with an OF changeset, as in the upstream binding (`maxim,max77705.yaml`), and gives it to the existing `max77705-rgb` device |
| `leds-max77705` | the upstream driver plus a current ceiling (`max_current`) |
| `led-class-multicolor` | `=m` in the stable config; installed with the extras |

Build with `scripts/build/build_extras.sh`, install with
`sudo extras/install.sh notification-led`.

## Channels and current

From the vendor's `leds-max77705-rgb.c` and its DT (`rgb-name = "led_w",
"led_r", "led_g", "led_b"`): LED0 is white (not fitted on the SM-G965F), LED1
red, LED2 green, LED3 blue. Registers: `LEDEN` 0x30 (2 bits per channel),
`LED0BRT..LED3BRT` 0x31-0x34, `LEDRMP` 0x36, `LEDBLNK` 0x38.

The brightness registers set the LED **current**. Upstream writes the LED
class brightness (0-255) straight into them, and at 0xff the LED is
blinding. Samsung never goes above `led_dynamic_current = 0x14`, so here full
brightness maps to `max_current = 20`.

## Using it

```bash
L=/sys/class/leds/rgb:status
cat $L/multi_index                                   # red green blue
echo none > $L/trigger
echo 0 0 255 > $L/multi_intensity; echo 255 > $L/brightness   # blue
echo timer > $L/trigger                               # blinking
```

The default trigger is `max170xx_battery-charging-orange-full-green`.
Options in `/etc/modprobe.d/s9p-leds.conf`:

| parameter | default | |
|---|---|---|
| `s9p_leds trigger` | battery, orange/green | default trigger; empty for none |
| `leds_max77705 max_current` | 20 | current register at full brightness, 1..255 |

## Hurdles

| Symptom | Cause | Fix |
|---|---|---|
| probe fails with -EINVAL | the driver reads `reg` on the RGB node too; the binding example has none | `multi-led@0` with `reg = <0>` |
| charging showed purple instead of orange | the changeset prepends children, so the colours came out blue, green, red | children created in reverse order |
| LED blinding | brightness written as the current register | `max_current` ceiling |
