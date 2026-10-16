# Installing

## What has to be true first

1. **Bootloader unlocked** and `KG STATE: Normal` in download mode.
2. **TWRP 3.3.1-0 for star2lte on `RECOVERY`.**
3. `adb` on the host (Android platform-tools). The flash scripts find the S9+
   by its product name; with more than one on the bus, set `S9PLUS_SERIAL`.

### KG STATE: Prenormal blocks everything

In that state the bootloader accepts an Odin session and then refuses every
partition operation. Two independent tools fail at exactly the same point, so
it is not a tool bug:

| Tool | gets to | fails with |
|---|---|---|
| Thor 1.1.0 | `Successfully began an Odin session!` | `flashTar` → `Failed to bulk read: Connection timed out (110)` |
| Heimdall 1.4.2 | `Session begun.` | `ERROR: Failed to receive PIT file size!` |

`OEM LOCK: OFF` only means the developer-options switch is set. What matters
is `KG STATE`.

**How it was cleared on this device:** boot Android, correct the device clock,
give it internet (USB tethering works), and leave it online. Knox Guard went
to `Checking` and the bootloader started accepting writes. The seven days
Samsung nominally wants were not needed.

## What you flash

Two pieces, both built from this repository ([02-building.md](02-building.md)):

| Partition | Image | Built by |
|---|---|---|
| `BOOT` (`/dev/block/sda10`) | `dist/uniloader/<name>.img` — uniLoader + kernel + device tree | `scripts/build/build_stable.sh` + `pack_uniloader.sh` |
| `USERDATA` (`/dev/block/sda25`) | `dist/image/s9plus-rootfs.img` — the Ubuntu root filesystem | `image/build-image.sh` |

`RECOVERY` keeps TWRP. **Flashing USERDATA replaces all Android user data.**

## Flashing

Everything after the initial TWRP install goes over **TWRP + adb**, not Odin.
Boot the phone into TWRP (Power + Volume-Up + Bixby), cable in:

```bash
# the root filesystem (resumable, every chunk checked, then read back whole)
python scripts/flash/flash_s9plus_rootfs_adb.py dist/image/s9plus-rootfs.img

# the kernel: the uniLoader-wrapped image (a raw m71-*/boot.img does not boot)
python scripts/flash/flash_s9plus_boot_adb.py dist/uniloader/<name>.img
```

The boot script reboots when BOOT reads back correctly.

> TWRP's `adbd` accepts `exec-in` but **discards the stream**. The rootfs
> therefore goes over `adb push` in 128 MiB chunks into `/tmp` and is written
> with `dd seek=`, each chunk compared against the partition first. That makes
> the whole run idempotent and resumable — which matters, because the Windows
> adb server reliably drops out during long transfers.

### Odin, if you ever need it

`TWRP_IMG=<twrp image> scripts/flash/mk_twrp_tar.sh` builds the smallest
possible Odin package: TWRP for `RECOVERY` only. That is the one write that
matters, because sboot keeps the recovery flag it was once given.

Odin-path traps that cost time:

* The device gives **one Odin handshake per download-mode entry**. Every test
  run uses it up; after that only a power cycle helps.
* `Thor-Windows.exe` cannot flash at all ("A USB handler wasn't written for
  your platform") — only the Linux binary can, over `usbipd-win`.
* Thor needs a real pty **with the window size set**, or Spectre.Console wraps
  after every single character.
* Blacklist `cdc_acm`; unloading it mid-session tears down the usbip link.
* Longer USB timeouts do **not** fix the PIT problem.

## First boot

* The root filesystem grows to the whole of USERDATA and the phone creates
  its own SSH host keys; then GDM logs your user in, GNOME on Wayland.
* The user and password are the ones you gave the image build; root is
  locked (use `sudo`), and root can never log in over SSH.
* **USB network:** the phone takes `172.16.42.2` (and also asks for DHCP). Give
  the PC's new USB network adapter the address `172.16.42.1/24`, then
  `ssh <user>@172.16.42.2`. The link is only between the phone and that PC.
* The same cable is also a serial console (a `getty` on `ttyGS0`, a COM port on
  the PC).
* Wi-Fi: from the GNOME menu, like on any laptop.

## What the image adds for this phone

`device/base/` is everything the image carries on top of the common base
system ([`image/common/README.md`](../image/common/README.md)), and
`device/configure.sh` enables it. Only what the hardware needs:

| | |
|---|---|
| `usb-gadget` + `usb-gadget.service` | USB serial console (ACM) and network (NCM, ECM/RNDIS as fallbacks) |
| `NetworkManager/…/usb0.nmconnection` | the phone's side of the USB network |
| `s9p-touch-power` + service | lifts the charger input limit from 375 mA (what TWRP leaves) to 500 mA. The touch rails and the GPU rail are set by the kernel (`s9p-acpm`, `s9p-g3d`) |
| `s9p-stability` + service | keeps the idle-only voltage reductions off (see [06-known-issues.md](06-known-issues.md)) |
| `s9p-wifi-guard` + service | brings Wi-Fi back after NetworkManager gave up on a failed rekey |
| `hciattach-bcm4361.service` + `brcm/bcm4361B2_semco.hcd` | old Bluetooth attach, **installed but not enabled**: it does not work with the serdev UART; use `extras/bluetooth` |
| `brcm/brcmfmac4361-pcie.*` | Wi-Fi firmware on disk too (the stable kernel has it built in) |
| `logind.conf.d`, `sleep.conf.d`, masked sleep targets | no suspend: the big cores do not come back from it |
| `system.conf.d/s9p-watchdog.conf` | systemd feeds the hardware watchdog (`RuntimeWatchdogSec=30s`) |
| `s9p-boot-ok` + timer | two minutes into a healthy boot, an unplanned reset (panic, watchdog) restarts Ubuntu instead of TWRP; see [06-known-issues.md](06-known-issues.md) |
| `gdm.service.d/10-wait-for-render-node.conf` | GDM waits for the GPU's render node |
| `PAN_MAX_AFBC_PACKING_RATIO=0` in `/etc/environment` | Mesa texture fix, see below |

Everything else — power profiles, the flight recorder, the apt proxy over
USB, the bring-up tools — is in [`extras/`](../extras/) and only installed
when you ask for it.

Two userland settings that are easy to get wrong:

* **`PAN_MAX_AFBC_PACKING_RATIO=0` must be in `/etc/environment`** (the image
  build adds it). Without it icons and glyphs show 16-pixel horizontal
  dashes. `/etc/environment.d/` does **not** reach `gnome-shell`;
  `/etc/environment` (pam_env) does.
* **`LIBGL_ALWAYS_SOFTWARE=1` must not be anywhere** (`/etc/environment`,
  `gdm3.service.d`) or every client silently gets llvmpipe. The user must be
  in the `render` group (the image build does that).

> When you change either and want to measure the result, run
> `loginctl terminate-user <user>` before restarting GDM. The `systemd --user`
> manager keeps its environment across `systemctl restart gdm`, and a stale
> value has survived three measurements that way.

## If it does not come up

1. **Black, no USB** → wrong DTB or a kernel panic. Download mode is in ROM,
   not in `boot`, so it is still reachable. Flash a known-good `boot.img`
   back, or reach TWRP with Power + VolUp + Bixby.
2. **Boot log on the panel, then nothing** → read the ramoops window from
   TWRP; the kernel's console zone is at `0x4000` of `0x8000@0xfed10000` and
   TWRP shows it as `pmsg-ramoops-0`. Read the **end** of it.
3. **Serial console, rescue shell** → kernel is fine, the rootfs is missing or
   broken. Reflash `s9plus-rootfs.img`.

The kernel's init arms Samsung's recovery magic in the PMU before it does
anything else (`INFORM2 = 0x12345678`, `INFORM3 = 0x12345674`), so a panic,
the init's own deadman timer, or any later reset comes up **in TWRP** rather
than in the same kernel again.

> The inverse layout — TWRP on `BOOT`, the test kernel on `RECOVERY` — looks
> safer and is not: the recovery key combo then lands in the *test* kernel,
> which is exactly what someone reaches for when the phone looks dead.
