# vibration

The vibration motor, as a standard force-feedback input device
(`max77693-haptic`, `/dev/input/event*`, FF_RUMBLE), and `feedbackd` set up to
use it: anything that talks to feedbackd (libfeedback, GNOME Calls, Chatty,
`fbcli`) vibrates the phone.

```bash
sudo extras/install.sh vibration
fbcli -E message-new-instant           # a short buzz through feedbackd
```

The motor is an LRA on the MAX77705 PMIC, driven from TOUT1 of the SoC's PWM
block. `s9p-haptic` adds the PWM block and the `haptic` node the upstream
driver needs; a copy of `max77693-haptic` adds two limits, in
`/etc/modprobe.d/s9p-haptic.conf`:

| option | default | |
|---|---|---|
| `max_ratio` | 75 | PWM duty at full strength, percent (50 = no force; upstream goes to 100, Samsung stops at 75) |
| `max_ms` | 10000 | longest single vibration, so a client that dies mid-effect cannot leave it running |

`/etc/udev/rules.d/90-feedbackd.rules` replaces the package's rules: it marks
the motor as feedbackd's vibrator and leaves the notification LED to
`extras/notification-led`. Details: [docs/12-vibration.md](../../docs/12-vibration.md).
