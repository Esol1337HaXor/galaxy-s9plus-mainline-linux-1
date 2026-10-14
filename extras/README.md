# Extras

Optional features for the Galaxy S9+. The normal image (`image/build-image.sh`)
installs none of them; it is the same clean Ubuntu as on the other phones of
this project, plus only what this phone's hardware needs. Install what you
want on the phone itself, from a checkout of this repository:

```bash
sudo extras/install.sh                      # list
sudo extras/install.sh power-profiles       # install one
sudo extras/install.sh power-profiles --remove
```

| Feature | What it does |
|---|---|
| [power-profiles](power-profiles/) | CPU/GPU ceilings and Wi-Fi power save as three profiles; follows GNOME's power mode switch |
| [flight-recorder](flight-recorder/) | writes every kernel message and a state line per minute to disk, fsync'ed, so a hard hang leaves its last minute behind |
| [usb-apt-proxy](usb-apt-proxy/) | lets `apt` reach the internet through a proxy on the PC the phone is cabled to |
| [usb-c-display](usb-c-display/) | external monitor over USB-C (DisplayPort to HDMI), USB host on the adapter's hub, HDMI audio. Needs `scripts/build/build_extras.sh` |
| [audio](audio/) | built-in speaker and headphone jack through the ABOX DSP (stock firmware copied from the phone). Needs `scripts/build/build_extras.sh` |
| [debug-tools](debug-tools/) | register, PMIC and clock tools used to bring the port up; test-run reboot helper; the old userspace governor |
