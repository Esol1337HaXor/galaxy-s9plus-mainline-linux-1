# Third-party components

| Component | Where | Origin | Licence |
|---|---|---|---|
| uniLoader | `bootloader/uniLoader/` | https://github.com/ivoszbg/uniLoader, plus this port's `configs/star2lte_defconfig` | see `bootloader/uniLoader/LICENSE` |
| Samsung UFS PHY calibration | `src/ufs/ufs-cal-9810.[ch]` | Samsung Exynos9810 vendor kernel, unchanged | GPL-2.0 |
| MAX77705 charger driver | `src/soc/max77705_charger.c` | mainline Linux v7.1 (Dzmitry Sankouski), with this port's `charge_behaviour` addition | GPL-2.0 |
| Pinned upstream copy of that driver | `src/reference/max77705_charger.c` | torvalds/linux v7.1, unmodified | GPL-2.0 |
| `exynos-bootfb.c` | generated at build time | `drivers/gpu/drm/sysfb/simpledrm.c` from the kernel tree being built, with `DRIVER_NAME` renamed | GPL-2.0 |
| BCM4361 firmware, NVRAM, CLM blob, Bluetooth patchram | `firmware/`, `device/base/lib/firmware/brcm/` | Samsung SM-G965F stock firmware | Broadcom, redistributed as shipped on the device; not GPL |
| `regulatory.db`, `regulatory.db.p7s` | `firmware/` | `wireless-regdb` | ISC |
| ModemManager D-Bus introspection | `src/modem/userspace/mm/` | https://gitlab.freedesktop.org/mobile-broadband/ModemManager, `introspection/` at 1.22.0, unmodified | ModemManager's (daemon GPL-2.0-or-later; libmm-glib, generated from these files, LGPL-2.0-or-later) |
| TWRP 3.3.1-0 for star2lte | not in this repository | https://twrp.me | its own terms |
| Boot logo | `branding/shero_logo.ppm` | this project (`scripts/build/make_logo.py`) | GPL-2.0 |

The firmware blobs are device firmware, not part of the kernel's own source.
They are included so `WLAN=1` builds do not race a firmware loader at boot.
