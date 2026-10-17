# flashlight

The rear flash LED as a torch: `/sys/class/leds/white:flash`, brightness
0-255, the same interface as other ports' flash LEDs, so Galaxy Companion's
flashlight (Quick Settings, button actions) drives it.

The LED is on the S2MPB02 camera PMIC (0x59 on `hsi2c@10920000`), which the
port's device tree does not have. `s9p-flash` opens that bus (as the camera
bring-up module does) and drives the torch over I2C, the way Samsung's
`leds-s2mpb02` does: brightness 1-255 is torch step 1-9, Samsung's torch
range. The camera flash (strobe) and the iris IR LED are not touched.

```bash
sudo extras/install.sh flashlight
echo 128 | sudo tee /sys/class/leds/white:flash/brightness   # on
echo 0 | sudo tee /sys/class/leds/white:flash/brightness     # off
```

Needs `scripts/build/build_extras.sh`.
