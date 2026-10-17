# Ubuntu 24.04 LTS for Samsung Galaxy S9+

Ubuntu 24.04 LTS arm64 for the Exynos Samsung Galaxy S9+ (`SM-G965F`,
`star2lte`), running GNOME 46 on Wayland and mainline Linux 7.1.

This is a fork of [SheroAbi/galaxy-s9plus-mainline-linux](https://github.com/SheroAbi/galaxy-s9plus-mainline-linux),
the port that brought mainline Linux and Ubuntu to this phone. On top of it,
this fork adds an **external monitor over USB-C**, **USB host** on hubs and
docks, **HDMI audio**, the **built-in speaker and headphone jack**, Bluetooth,
the microSD slot, a kernel ready for **Docker and USB Ethernet**, and resets
that come back on their own: enough to use the phone as a small desktop
computer or a home server, even one with a broken screen.

> [!NOTE]
> Only for the **Exynos** Galaxy S9+ (**SM-G965F**). The Snapdragon model
> (SM-G965U) is a different device and nothing here applies to it.

## Hardware compatibility

| Component | Status | Summary |
|---|---:|---|
| Display | ✅ | Native 1440×2960 at 60 fps |
| Desktop | ✅ | GNOME 46, Wayland and GDM |
| GPU | ✅ | Mali-G72 MP18 (Panfrost), up to 572 MHz |
| Storage | ✅ | Internal UFS 2.1, Ubuntu boots from it |
| microSD | ✅ | Up to SDXC, UHS-I SDR104, exFAT, hot-plug |
| Touchscreen | ✅ | S6SY761 multitouch |
| Wi-Fi | ✅ | BCM4361 (`brcmfmac`) |
| Bluetooth | ✅ | BCM4361 over UART, in GNOME Settings |
| USB-C DisplayPort | ✅ | External monitor up to 1080p60, hot-plug |
| USB host | ✅ | Keyboards, mice and storage on the hub; Ethernet adapters untested |
| HDMI audio | ✅ | Sound on the external monitor |
| Speaker | ✅ | Both amplifiers via the ABOX DSP; volume capped |
| Headphone jack | ✅ | CS47L93 codec; no jack detection yet |
| Microphones | ✅ | Both internal mics; headset mic not yet |
| USB device mode | ✅ | Serial console and network to a PC over one cable |
| Battery telemetry | ✅ | Charge, voltage, current and temperature |
| Charging | ✅ | MAX77705, up to 3 A; reaches 100 % |
| Vibration | ✅ | Force-feedback device, used by feedbackd |
| Notification LED | ✅ | Orange while charging, green when full |
| Buttons | ✅ | Power and volume keys; short press locks, long press power menu |
| CPU scaling | ✅ | Both clusters follow the load; thermal and battery caps |
| Power management | 🟡 | Memory DVFS, unused blocks off; no deep CPU idle or suspend yet |
| Cameras | 🟡 | Front sensor answers on I2C; no image yet |
| Mobile network | 🟡 | LTE mobile data out of the box; calls and SMS not yet |
| Fingerprint reader | ❓ | Not tested |
| Motion sensors | ❓ | Not tested |

✅ working on the physical phone · 🟡 experimental or partially validated ·
❌ unavailable · ❓ not tested

The USB-C display, audio, Bluetooth, microSD and other pieces are optional
[extras](extras/README.md), mostly built as kernel modules. How every driver works,
and what does not yet, is documented in [docs/](docs/).

## Installation

Everything is built from this repository and public sources: no image is
downloaded from anywhere.

> [!WARNING]
> **Everything Android keeps in `USERDATA` is erased**: its apps, its settings
> and its files. `RECOVERY` keeps TWRP as the way back. Back up anything you
> want to keep first. On another note, the project seems really stable so far,
> but as always: I'm not responsible for bricked devices, missing recovery
> partitions, nuclear wars or you getting fired because you forgot to boot
> back into Android for the alarm.

### Before you start

1. **An unlocked bootloader.**
2. **TWRP 3.3.1-0** (`star2lte`) flashed to `RECOVERY`, and its image file at
   hand: the kernel image is built on top of it.
3. A **Linux** build host: Ubuntu 24.04 on a PC or in a VM (WSL2 works too,
   but neither Windows nor WSL is required), with `adb`.

### Build

```bash
git clone https://github.com/agcarbajo/galaxy-s9plus-mainline-linux.git
cd galaxy-s9plus-mainline-linux

# 1. one-time setup: build directory, cross toolchain + static busybox, kernel tree
sudo scripts/build/setup_build.sh
sudo scripts/build/toolchain.sh
sudo scripts/build/fetch_mainline.sh

# 2. the kernel, wrapped in uniLoader (the official TWRP image is the template)
sudo scripts/build/build_stable.sh
sudo TWRP_IMG=/path/to/twrp-3.3.1-0-star2lte.img \
    scripts/build/pack_uniloader.sh dist/m71-<stamp> boot-s9plus.img

# 3. the Ubuntu root filesystem (asks for your password)
sudo image/build-image.sh

# 4. optional: the modules of the extras (display, audio, Bluetooth, microSD...)
sudo scripts/build/build_extras.sh
```

- The kernel tree lives in `BUILD` (default `/mnt/build`; `BUILD=/some/dir`
  moves it). See [docs/02-building.md](docs/02-building.md).
- Flash the **uniLoader** image from `dist/uniloader/`. The raw
  `dist/m71-<stamp>/boot.img` does not boot on its own.

### Flash

Boot the phone into TWRP (Power + Volume-Up + Bixby) and connect it by USB:

```bash
# the root filesystem: resumable, every chunk checked, then read back whole
python3 scripts/flash/flash_s9plus_rootfs_adb.py dist/image/s9plus-rootfs.img
# the kernel: reboots only after BOOT reads back correctly
python3 scripts/flash/flash_s9plus_boot_adb.py dist/uniloader/boot-s9plus.img
```

### First boot

The root filesystem grows to the whole partition, the phone makes its own SSH
keys, and GNOME logs your user in. Over the USB cable the phone is
`172.16.42.2`: give the PC's new USB network adapter `172.16.42.1/24` and run
`ssh <user>@172.16.42.2`. Wi-Fi is set up from the GNOME menu. Details:
[docs/03-installing.md](docs/03-installing.md).

### Extras

Nothing extra is preinstalled. Copy the repository (with `dist/`, if you built
the extras) to the phone and install what you want there:

```bash
sudo extras/install.sh                    # list them
sudo extras/install.sh usb-c-display      # external monitor, USB host, HDMI audio
sudo extras/install.sh audio              # speaker and headphones (reboot after)
sudo extras/install.sh notification-led   # RGB LED: orange charging, green full
sudo extras/install.sh bluetooth          # Bluetooth (needs usb-c-display)
sudo extras/install.sh vibration          # vibration motor + feedbackd
sudo extras/install.sh microsd            # the microSD slot
sudo extras/install.sh low-power          # lower idle power
sudo extras/install.sh power-profiles     # --remove takes any of them out again
```

| Extra | What it does |
|---|---|
| [usb-c-display](extras/usb-c-display/) | External monitor, USB host on the adapter's hub, HDMI audio. Broken screen? `sudo systemctl enable s9p-panel-off` |
| [audio](extras/audio/) | Speaker and headphone jack; DSP firmware copied from the phone's `VENDOR` |
| [bluetooth](extras/bluetooth/) | Bluetooth; patchram from `VENDOR`, address from `EFS`. Needs usb-c-display |
| [microsd](extras/microsd/) | The microSD slot, UHS-I and exFAT |
| [low-power](extras/low-power/) | Memory clock follows the load; unused power domains off |
| [notification-led](extras/notification-led/) | The RGB LED, following the battery; any LED trigger |
| [vibration](extras/vibration/) | The vibration motor, with feedbackd |
| [power-button](extras/power-button/) | Short press locks and blanks, long press opens the power menu |
| [power-profiles](extras/power-profiles/) | Three CPU/GPU/Wi-Fi profiles, following GNOME's power mode |
| [flight-recorder](extras/flight-recorder/) | Kernel log and a state line per minute, fsync'ed, for hunting hangs |
| [usb-apt-proxy](extras/usb-apt-proxy/) | apt through a proxy on the PC the phone is cabled to |
| [debug-tools](extras/debug-tools/) | Register, PMIC and clock tools; `s9p-cpu-validate` for CPU load tests |

Liked the project? you might want to buy me a coffee, I'll be very grateful!

<a href='https://ko-fi.com/V7W426ZJIP' target='_blank'><img height='36' style='border:0px;height:36px;' src='https://storage.ko-fi.com/cdn/kofi5.png?v=6' border='0' alt='Buy Me a Coffee at ko-fi.com' /></a>

## Known issues

- A crash in the first two minutes after boot lands in TWRP on purpose (a bad
  kernel must not loop); `adb reboot` brings Ubuntu back. Later crashes and
  watchdog resets restart Ubuntu on their own.
- No deep CPU idle on every core and no suspend, so the battery drains faster
  than on Android.
- Sustained CPU load is limited by heat: the clusters step down from 76-80 C.
- The USB-C display draws its power from the phone: with the battery nearly
  empty (around 5 %) the adapter no longer negotiates power and the monitor and
  hub stay dark. Charge the phone, or use an adapter with a USB-C PD input.
- The speaker runs without Samsung's DSM protection, so its volume is capped.
- No headphone jack detection and no headset microphone yet.
- The audio DSP boots once per phone boot; reloading its module needs a reboot.

More in [docs/06-known-issues.md](docs/06-known-issues.md).

## Troubleshooting

| Problem | Fix |
|---|---|
| `qemu-aarch64-static: Could not open '/lib/ld-linux-aarch64.so.1'` | The busybox in `$BUILD/out` is dynamically linked. Get the static one: `python3 scripts/build/fetch_busybox.py $BUILD/out/busybox-arm64` |
| `no build directory: /mnt/build` | Run `sudo scripts/build/setup_build.sh` first, or point `BUILD` at an existing directory. |
| Odin/Heimdall start a session, then every write fails | `KG STATE: Prenormal` (Knox Guard). Boot Android online with a correct clock until it clears. See [docs/03-installing.md](docs/03-installing.md). |
| Phone "hangs at the logo" or falls back to recovery | S-Boot rejected the image. Use the uniLoader image, never the raw `boot.img`. |
| Black screen, no USB | Wrong device tree or a kernel panic. Reach TWRP with Power + Volume-Up + Bixby and flash a known-good image back. |
| Phone sits in TWRP after a crash | It crashed during boot. `adb reboot`; `s9p-boot-ok status` tells where the next reset goes. |
| Wi-Fi drops after a failed rekey | `s9p-wifi-guard` reconnects after a minute; on weak 5 GHz pin the connection to 2.4 GHz. |
| External monitor stays black | `sudo s9p-dp status`: no alt mode usually means a low battery or an adapter that needs a replug. See [docs/08-usb-c-display.md](docs/08-usb-c-display.md). |
| No sound | `sudo cat /sys/kernel/debug/s9p-abox/log` should show `BOOT_DONE`; pick the output in Settings → Sound. See [docs/09-audio.md](docs/09-audio.md). |

## Documentation

| Document | Contents |
|---|---|
| [Hardware](docs/01-hardware.md) | The SoC, what sits on which bus, and the addresses that matter |
| [Building](docs/02-building.md) | Build host, toolchain, every build switch, the image |
| [Installing](docs/03-installing.md) | Partitions, TWRP, flashing, first boot |
| [Drivers](docs/04-drivers.md) | Each driver, why it exists, how it works |
| [Performance](docs/05-performance.md) | The display path, the GPU, thermals, and how each was measured |
| [Known issues](docs/06-known-issues.md) | What does not work, and what was tried |
| [Validation](docs/07-validation.md) | The CPU/GPU validation runs and their limits |
| [USB-C display](docs/08-usb-c-display.md) | DisplayPort alt mode, link training, DECON2, USB host and HDMI audio |
| [Audio](docs/09-audio.md) | The ABOX DSP and its firmware, the codec, the amplifiers, PipeWire |
| [Notification LED](docs/10-notification-led.md) | The MAX77705 RGB LED and its triggers |
| [Bluetooth](docs/11-bluetooth.md) | The BCM4361 UART, patchram and address |
| [Vibration](docs/12-vibration.md) | PWM timer and the MAX77705 haptic driver |
| [Cameras](docs/13-cameras.md) | Work in progress: sensors, CSIS and the plan |
| [microSD](docs/14-microsd.md) | The DWMMC2 slot, its rails and UHS-I |
| [Modem](docs/15-modem.md) | Booting the Shannon CP with Samsung's cbd |

## Repository layout

```text
src/                 the drivers this port adds to the mainline tree
  ufs/               UFS 2.1 host controller + Samsung's PHY calibration
  gpu/drm/s9p/       DECON scanout driver (KMS)
  soc/               ACPM mailbox, G3D clock and power domain, clock gates, MUIC, charger
  cpufreq/  pci/  usb/   CPU clusters, PCIe root complex for Wi-Fi, USB 2.0 PHY
  cpupm/             CPU idle (C2) driver, work in progress
  power/             memory DVFS and unused power domains (modules)
  usbc-dp/           USB-C alt mode, DisplayPort, USB host and HDMI audio (modules)
  sound/             ABOX DSP, codec and amplifier drivers (modules)
  bluetooth/ sdcard/ leds/ haptic/ camera/   board glue for those blocks (modules)
  mainline-dts/      the board device tree
bootloader/uniLoader/  the shim that starts a mainline kernel on this S-Boot
image/               build the Ubuntu root filesystem
device/              initramfs init scripts, and the hardware layer of the image (base/)
extras/              optional features, installed on request
scripts/build/       kernel, boot image, DTBH table, busybox, extra modules
scripts/flash/       write boot and rootfs to the phone from TWRP
firmware/            BCM4361 firmware and the wireless regulatory database
docs/                everything above in detail
dist/                generated build output; not versioned
```

## Firmware and licensing

Keep in mind the work this fork adds (USB-C display, audio, Bluetooth,
microSD, power and the rest) has been made with the help of agentic AIs
(Claude Code), so some things might not be done in the
best possible way.

Proprietary Samsung firmware is not stored in Git. The audio extra copies the
ABOX DSP firmware from the phone's own Android `VENDOR` partition at install
time; the Wi-Fi firmware and regulatory database in `firmware/` are listed in
[THIRD-PARTY.md](THIRD-PARTY.md).

GPL-2.0-only, the same as the kernel these drivers are built into. See
[LICENSE](LICENSE), and [THIRD-PARTY.md](THIRD-PARTY.md) for the vendored and
redistributed components. `bootloader/uniLoader/` is a third-party project and
carries its own licence.

This is a hobby project, not affiliated with Samsung, and comes without any
warranty. Flashing a phone can go wrong; you do it at your own risk.

## Credits

- **[SheroAbi](https://github.com/SheroAbi)** for
  [galaxy-s9plus-mainline-linux](https://github.com/SheroAbi/galaxy-s9plus-mainline-linux),
  the port this fork is built on: the UFS, display, GPU clock, CPU frequency,
  PCIe Wi-Fi, USB and charger drivers, the uniLoader boot chain, the Ubuntu
  image, the build and flash tooling and its documentation. Same idea on other
  phones: [Xiaomi Mi 9T](https://github.com/SheroAbi/mi9t-mainline-linux) ·
  [Xiaomi Redmi 8](https://github.com/SheroAbi/redmi8-mainline-linux).
- [uniLoader](https://github.com/ivoszbg/uniLoader) and the mainline Exynos 9810
  community, whose SoC and Galaxy S9 board support the port builds on.
  `mkdtbh.py` follows [dtbtool-exynos](https://github.com/dsankouski/dtbtool-exynos).
- Samsung's GPL kernel sources for the SM-G965F (as published by
  [LineageOS](https://github.com/LineageOS/android_kernel_samsung_exynos9810)):
  the register-level reference for the Type-C, DisplayPort, ABOX, codec and
  amplifier work.
- The upstream Linux drivers this fork relies on: Cirrus Logic madera/CS47L92,
  ARM PL330 DMA, Synopsys DWC3 and the DRM/ALSA/HID frameworks.
