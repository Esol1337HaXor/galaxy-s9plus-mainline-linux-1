# microSD

[`extras/microsd`](../extras/microsd/) makes the slot work: `s9p-sd-board`
(src/sdcard) plus the kernel's own `dw_mmc-exynos` and `exfat`.

## Hardware (vendor DT `dwmmc2@11500000`, star2lte rev 26)

| | |
|---|---|
| controller | DWMMC2, `0x11500000`, GIC SPI 249, VERID `0x270a`, HCON `0x08e42541` (64-bit IDMAC, 64-bit FIFO, 64 deep) |
| mainline compatible | `samsung,exynos7-dw-mshc` (CLKSEL at `0xa8`); the vendor node is the generic one, no SMU/FMP |
| card clock | CMU_TOP `MUX_CLKCMU_FSYS1_MMC_CARD` `0x1a24106c` = 1 (PLL_SHARED2, 800 MHz), divider `0x1a241864` [8:0], gate `0x1a242070`; FSYS1 user mux `0x11400120` bit 4, gates `0x11402030` (ACLK) / `0x11402034` (SDCLKIN) |
| CLKSEL | divider 3, drive phase 2, sample phase 0 (DDR 1) -- vendor `sdr-timing = <3 0 2 0>` |
| card supply (vmmc) | S2MPS18 LDO31, register `0x68`, 2.8 V (`0xe8`) |
| I/O supply (vqmmc) | S2MPS18 LDO2, register `0x4a`, 1.8-3.3 V (1.8 V + 25 mV/step) |
| pins | gpf1-0 clk (drive 4), gpf1-1 cmd, gpf1-2..5 data (drive 3), function 2, no pull |
| card detect | gpa1-5 (EINT), active low: `0x14050044` bit 5 reads 0 with a card in |

sboot leaves the card mux on PLL_MMC with a divider of 16, both LDOs off and
the pins as pulled-down inputs. Nothing in the PMU needs touching: FSYS1 is
on, like UFS and PCIe.

## How the module does it

The node cannot just be added: fw_devlink would park the controller on the
clock and the supplies, which have no device of their own. So, in order:

1. CMU and pins by hand.
2. A first OF changeset: a clock node, two regulator nodes and
   `mmc@11500000` with `status = "disabled"`. Their phandles are set by hand
   after the apply (`__of_attach_node` reads `phandle` before the changeset has
   added it).
3. The providers: the divider as a clock (`determine_rate` / `set_rate` with
   the busy poll), the LDOs as regulators whose ops go through
   `s9p_acpm_pmic_*`.
4. A second changeset sets `status = "okay"`; dw_mmc probes.

The driver asks for `4 x` the card clock (CIU divider 3), so the divider
gives 200 MHz for high speed and 800 MHz for SDR104. With the I/O supply a
real regulator, the core does the 1.8 V switch and SDR104 tuning on its own.

## Measured (SanDisk SDXC 256 GB "SC256", exFAT, 2026-10-16)

| | |
|---|---|
| mode | UHS-I SDR104, 200 MHz |
| sequential read | 72 MB/s (1.6 GiB, cold cache) |
| write | 34 MB/s (16 MiB, `conv=fsync`) |
| integrity | 24 zip archives' CRCs correct; written file's SHA-256 equal after dropping caches |

`uhs=0` (`/etc/modprobe.d/s9p-sd.conf`) is the fallback for a card that
misbehaves at UHS-I: high speed, 50 MHz, 3.3 V.
