# usb-c-display

USB-C DisplayPort output (HDMI adapters/docks), USB host on the same port, HDMI audio.

Turns the phone into a small desktop computer: plug a USB-C to HDMI adapter or
dock, and GNOME moves to the external monitor (1080p60). Keyboards, mice and
USB sticks on the adapter's hub work, and the monitor gets the sound.

- The internal panel keeps working; GNOME extends or mirrors the desktop like on a
  laptop. Phone with a broken screen? `sudo systemctl enable s9p-panel-off` leaves
  the panel out from the next boot on, so GNOME only uses the external monitor
  (and runs headless, SSH/RDP, when none is plugged).
- Hot-plug in both cable orientations; screen blanking keeps the link alive.
- USB on the hub is USB 2.0 (the two SuperSpeed lanes carry DisplayPort).
- Needs the modules from `scripts/build/build_extras.sh` in `dist/extras/`.

```bash
sudo scripts/build/build_extras.sh            # on the build host, after build_stable.sh
# copy the repository, dist/ included, to the phone, then on the phone:
sudo extras/install.sh usb-c-display
sudo s9p-dp status                            # USB-C, link and DECON state
```

How it works: [docs/08-usb-c-display.md](../../docs/08-usb-c-display.md).
