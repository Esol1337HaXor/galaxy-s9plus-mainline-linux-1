# microsd

The microSD slot: cards up to SDXC, UHS-I SDR104 (200 MHz), hot-plug through
the card-detect pin, and exFAT (cards above 32 GB come formatted with it).
GNOME mounts an inserted card under `/media/$USER/` like any removable disk.

```bash
sudo extras/install.sh microsd
lsblk /dev/mmcblk0
```

The slot is DWMMC2, which the kernel's own `dw_mmc-exynos` drives. The port's
device tree has nothing of it, so `s9p-sd-board` supplies the rest: the card
clock (CMU_TOP on PLL_SHARED2, its divider registered as a real clock so the
driver can pick 200 or 800 MHz), the two supplies as regulators over ACPM
(S2MPS18 LDO31 for the card, LDO2 for the I/O so the core can switch to 1.8 V),
the pins, and the controller node. `exfat` is the kernel's own driver, built as
a module for this kernel.

Option, in `/etc/modprobe.d/s9p-sd.conf`: `uhs=0` keeps the card at high speed
(50 MHz, 3.3 V) -- the fallback for a card that misbehaves at UHS-I.

Measured on a SanDisk 256 GB SDXC: 72 MB/s read, 34 MB/s write, CRCs of 24 zip
archives and a write/read-back SHA-256 correct. Details:
[docs/14-microsd.md](../../docs/14-microsd.md).
