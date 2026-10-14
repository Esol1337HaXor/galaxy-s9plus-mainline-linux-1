# power-button

The power button as on a phone: a short press locks the session and turns
the screen off (another short press turns it on again, on the lock screen); a
long press (0.7 s) opens GNOME's power off / restart dialog.

```bash
sudo extras/install.sh power-button    # then log out and in, or reboot
```

Suspend cannot resume on this port (see `docs/06-known-issues.md`), so the
short press does not suspend: it does what a phone's button does. A user
service (`s9p-power-button`, in the GNOME session) reads the button and talks
to GNOME over D-Bus; GNOME's own power button action is set to "nothing". The
long-press time is `S9P_POWER_LONG_MS` (a drop-in for the user unit).
