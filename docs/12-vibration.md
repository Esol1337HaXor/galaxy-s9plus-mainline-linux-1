# Vibration

```
TOUT1 (PWM timer pwm@10510000, channel 1, pin gpp3-1) ── 20.48 kHz, duty 50..75 %
  └─> MAX77705 haptic driver (MCONFIG 0x10: LRA, external PWM, /128 = 160 Hz)
        └─> LRA motor
```

The MAX77705's MFD core registers a `max77705-haptic` device, and upstream
`drivers/input/misc/max77693-haptic.c` supports it, but the port's device
tree has neither the SoC's PWM block nor a `haptic` node, so the driver cannot
get its PWM. Values from the vendor DT (`haptic` node: `pwm_id` 1, period
48828 ns) and `max77705_haptic.c`.

| module | does |
|---|---|
| `s9p-haptic` | opens the PWM block's bus clock gate (CMU_PERIC0 `0x10402044`), gives `/osc-clock` (26 MHz, the PWM tick) a phandle, adds `pwm@10510000` (`samsung,s3c6400-pwm`, output 1) and `pmic@66/haptic` with an OF changeset, and hands the node to the existing device |
| `max77693-haptic` | upstream, plus `max_ratio` (duty ceiling, Samsung's 75 %) and `max_ms` (longest effect) |
| `ff-memless` | `=m` in the stable config, installed with the extras; it also emulates periodic effects with rumble |

The motor boost is not under software control on this phone (the vendor
driver's regulator hook returns 0), so there is no `haptic-supply`: the driver
uses a dummy regulator.

## feedbackd

feedbackd finds its vibrator by the udev property `FEEDBACKD_TYPE=vibra`; the
package only sets it for `platform-vibrator`. The extra's
`/etc/udev/rules.d/90-feedbackd.rules` (which replaces the package's file)
sets it for `max77693-haptic` and drops the package's LED rule, which would
take the `rgb:status` notification LED off its battery trigger.

The default theme's `phone-incoming-call` vibration is a periodic effect with
magnitude 0 in feedbackd 0.2.1 (Ubuntu 24.04), so it does not vibrate; rumble
events (`message-new-instant`, `message-new-email`, `button-pressed`...) do.

## Hurdles

| Symptom | Cause | Fix |
|---|---|---|
| `could not find phandle` for `pwms` | attaching a changeset node reads its phandle before the property is added | set the node's phandle again after `of_changeset_apply` |
| motor never ran from feedbackd | no `FEEDBACKD_TYPE` on the device | udev rule |
