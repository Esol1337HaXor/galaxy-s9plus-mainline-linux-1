# Building

## Build host

Any Linux with an aarch64 cross toolchain, run as root (the scripts mount
loop images and chroot): a plain Ubuntu 24.04 machine or VM is the simplest.
Neither Windows nor WSL is needed. WSL2 works too, and was the setup this
port was developed on.

The kernel tree does not live in the project folder. `scripts/build/env.sh`
defines where it does, and every value can be overridden:

```
BUILD=/mnt/build                    the build directory (any directory on Linux)
  $BUILD/src/mainline               the mainline 7.1 tree
  $BUILD/out                        initramfs staging, busybox, empty ramdisk
  $BUILD/rootfs/noble               the staged Ubuntu rootfs
BUILD_IMG=                          only under WSL: an ext4 loop image mounted at
                                    $BUILD (default /mnt/e/s9plus-build.img there)
```

On a plain Linux host `BUILD` is an ordinary directory and `BUILD_IMG` stays
empty. Under WSL the project sits on a 9p/DrvFs mount, which is unusably slow
for a kernel tree and cannot hold its symlinks and permissions, hence the
70 GB loop image; set `BUILD_IMG` to wherever it should go.

Nothing is downloaded as a prebuilt binary from this project. The one input
from outside the toolchain is a static arm64 **busybox** for the initramfs:
`toolchain.sh` takes it from Ubuntu's `busybox-static` package
(`scripts/build/fetch_busybox.py`, checked against the archive's SHA-256). It
has to be static: the initramfs has no C library, and with a dynamic busybox
the build stops with `qemu-aarch64-static: Could not open
'/lib/ld-linux-aarch64.so.1'`. The boot image and the `dt.img` table are
written by `mkboot.py` and `mkdtbh.py`, Python versions of Samsung's
`mkbootimg` and of `dtbTool-exynos` that produce the same bytes.

> Under WSL, the loop image is unmounted between `wsl.exe` invocations, and
> anything still in the page cache is lost with it. Every script therefore
> mounts it itself and ends with `sync`. A finished `Image` that was 54 MB
> before the call has come back as 0 bytes without that.

### One-time setup

```bash
sudo scripts/build/setup_build.sh    # create BUILD (and, under WSL, the loop image)
sudo scripts/build/toolchain.sh      # gcc-aarch64-linux-gnu, bison, flex, qemu-user-static, ..., busybox
sudo scripts/build/fetch_mainline.sh # clone the mainline tree into $BUILD/src/mainline
```

## The build

```bash
sudo scripts/build/build_stable.sh
```

That is the configuration the device runs: `INIT=boot UFS=1 GPU=1 DECON=1
WLAN=1 GADGET=configfs`, the boot command line with the watchdog armed, no
Tux logo, no plymouth. It calls `build71.sh` and prints `BUILD EXIT 0` when
the image is ready.

Output lands in `dist/m71-<UTC timestamp>/`:

| File | |
|---|---|
| `boot.img` | the flashable Samsung boot image (`SEANDROIDENFORCE` appended) |
| `Image`, `vmlinux`, `System.map` | the kernel |
| `dt.img` | the DTBH table sboot expects |
| `exynos9810-star2lte.dts/.dtb` | the device tree as it was actually built |
| `kernel.config` | the full config |
| `kernel-source.patch` | the in-tree patches this build applied |
| `port-sources/` | the generated sources archived with the build |
| `SHA256SUMS` | |

Build logs go to `build-logs/build71-<stamp>.log`.

## build71.sh in detail

`build71.sh` is the end-to-end build: device tree, config, initramfs, kernel,
boot image. It reads everything it needs out of the project folder and locates
that folder from its own path, so the project can be moved without editing
anything.

### Switches

All of them are environment variables.

| | |
|---|---|
| `UFS=1` | enable the UFS node. Off by default: reading an unpowered block is a bus hang, not an error |
| `GPU=1` | enable the Mali node, same reasoning |
| `DECON=1` | let the DECON scanout driver take the display over from the bootloader framebuffer |
| `WLAN=1` | build `brcmfmac` in and embed the BCM4361 firmware and the regulatory database |
| `GADGET=configfs` | USB configfs gadget: ACM console plus NCM network |
| `INIT=boot` | build the image that hands over to Ubuntu on `USERDATA`. Without it you get the probe image, which reports and then panics on purpose |
| `NOFB=1` | drop the bootloader framebuffer node, for bisecting the display stack |
| `FRESH_CONFIG=1` | start from `defconfig` again |
| `POKE=ufs,gpu` | let the probe image read those blocks' registers directly |
| `BOOT_CMDLINE=...` | replace the kernel command line |
| `DIST=...` | write the build somewhere other than `dist/` |

### What it does, in order

1. Copies `src/mainline-dts/exynos9810-star2lte.dts` into the tree and applies
   the `*-STATUS` switches above.
2. Registers the board in `arch/arm64/boot/dts/exynos/Makefile` if it is not
   there yet.
3. `purge_fbband.py` removes the early colour bands (a debugging aid that is
   only stripes across the boot logo on a working device).
4. `apply_drivers.sh` installs every driver from `src/` into the tree and runs
   `patch_tree.py` for the in-tree edits.
5. Builds the initramfs from a static busybox plus `device/mainline-init*`.
6. Configures, builds `Image` and `dtbs`, and modules if any are `=m`.
7. **Writes `0x00080000` back into the arm64 image header's `text_offset`.**
   See below.
8. Packs `dt.img` with `mkdtbh.py` and the boot image with `mkboot.py`,
   which also appends `SEANDROIDENFORCE`.

### The stable config and the guard

The strip dance in `build71.sh` only names what it turns off. This phone's
platform drivers (`SERIAL_SAMSUNG`, `I2C_S3C2410`, `SPI_S3C64XX`,
`MFD_EXYNOS_LPASS`, `MFD_SEC_ACPM`, `EXYNOS_ACPM_PROTOCOL`,
`ARM_SCMI_PROTOCOL`, `EXYNOS_THERMAL` and about 120 more) are in no loop: on
a shared `BUILD` tree they were added to `out/.config` by hand across earlier
sessions. A foreign `.config` (another port's build over the same tree)
silently loses them — no serial (the Bluetooth UART), no I2C codec, no LPASS,
no PMIC rails: audio lands on the dummy device and Bluetooth disappears, with
no build error to show for it.

`scripts/build/kernel.config.stable` pins the config of a known-good build.
`build71.sh` installs it over `out/.config` before the loops run (the loops
then re-apply this port's own options on top, so `FRESH_CONFIG` is rarely
needed), and a guard after `olddefconfig` stops the build if any of eight
critical symbols is lost:

```
config guard: CONFIG_SERIAL_SAMSUNG lost
```

If the guard trips, do not disable it: capture `out/.config` of a good build
into `scripts/build/kernel.config.stable` instead.

### The panel sleep workaround

`build_stable.sh` sets `s9p_decon.panel_sleep=0` on the boot command line.
With it the DECON driver only sends `set_display_off` when the display blanks
and never puts the panel into MIPI sleep mode. On this panel entering sleep
loses the panel's initialization: the first wake-up after the first lock
latches stripes and blotches over everything, immediately, whatever delay the
driver waits before or after `set_display_on`. With the panel kept out of
sleep the locked screen still goes fully dark. The parameter is a module
option, so it can also be flipped at runtime through
`/sys/module/s9p_decon/parameters/panel_sleep`.

### text_offset — why a mainline kernel does nothing at all here

| | Samsung 4.9 | mainline 7.1 |
|---|---|---|
| `text_offset` (image header byte 0x08) | `0x00080000` | `0` |

Mainline has set that field to 0 since 5.8, because the kernel relocates
itself. sboot on this phone is from 2018 and still places the image by it, so
a mainline kernel lands at `0x80000000` instead of `0x80080000` — and the
bootloader's own log says what lives there:

```
sec_debug_magic   reg = <0x0 0x80000000 0x1000>
kaslr_region=0x1000@0x80001000
```

The bootloader writes its own structures over the first 8 KB of the kernel
before jumping to it. The result is a kernel that dies silently: no console,
no panic, no reset, not one byte in the ramoops window.

The build writes the old value back after linking. A relocatable kernel never
reads its own header, so this costs nothing and puts the image where this
bootloader expects it.

## The uniLoader image

`build_stable.sh` produces a raw `boot.img`. The **validated** image for
running Ubuntu wraps the same kernel in uniLoader, a small shim that fixes two
things sboot gets wrong before the kernel starts — most importantly DECON's
`HW_SW_TRIG_CONTROL`, which sboot leaves unset, so without the shim nothing
written to the framebuffer ever reaches the panel.

```bash
# from an existing build71 output
TWRP_IMG=/path/to/twrp-3.3.1-0-star2lte.img \
    scripts/build/pack_uniloader.sh dist/m71-<stamp> <image-name>.img
# or build and pack in one go
TWRP_IMG=/path/to/twrp-3.3.1-0-star2lte.img \
    scripts/build/build71_ubuntu.sh <image-name>.img GPU=1 GADGET=configfs
```

The image lands in `dist/uniloader/<image-name>`. Payload address `0x92000000`.

The boot image copies its header layout and its DTBH table from the official
**TWRP 3.3.1-0 image for star2lte** (twrp.me), because that is an image the
bootloader is known to accept; the kernel never sees that DT, uniLoader hands
over its own. The TWRP image this port was validated with has SHA-256
`5b5d2290940203316024a94581107ffad17670bb2f596fd27ef483d3d2685994`.

## Size

arm64 `defconfig` produces a 54 MB `Image` — a distro kernel for four dozen
SoC families. The config here strips it to about 21 MB by turning off every
other `ARCH_*` and the subsystems this phone does not have.

## Building the root filesystem

```bash
sudo image/build-image.sh
```

Builds the Ubuntu 24.04 root filesystem from scratch: debootstrap, the common
package set and settings of all three phone projects
([`image/common/README.md`](../image/common/README.md)), then this phone's
hardware layer (`device/base/`, `device/configure.sh`). It asks for the
password of the user it creates; user name, time zone, locale, keyboard and
an SSH key are environment variables described there.

Output: `dist/image/s9plus-rootfs.img` (raw ext4, grows to the whole of
USERDATA on the first boot) and `SHA256SUMS`. The image needs no kernel
modules — the stable kernel has every driver built in — so kernel and root
filesystem can be rebuilt independently.

Build host: Ubuntu 24.04 as root (a VM or WSL2 works too), with
`debootstrap qemu-user-static binfmt-support e2fsprogs openssl python3 curl
git`. The work directory (`WORK`, default `/var/tmp/s9plus-image`) must be
on a Linux filesystem.
