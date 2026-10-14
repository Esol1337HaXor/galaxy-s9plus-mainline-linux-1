# Cameras (work in progress)

Goal: the cameras as ordinary V4L2 devices that libcamera, PipeWire and every
application see, without Samsung's ISP. The plan, from the vendor kernel:

| phase | what | state |
|---|---|---|
| 1 | front camera powered, sensor answering on I2C | **done** (2026-10-15) |
| 2 | CSIS v5.1 receiver + its DMA writing RAW frames to memory (test pattern first) | block reachable (2026-10-15), capture not started |
| 3 | V4L2 drivers: S5K3H1 sensor (v4l2-cci) and CSIS, media controller | needs `MEDIA_SUPPORT` (can be modules) |
| 4 | libcamera "simple" pipeline with the software ISP, PipeWire | |
| 5 | rear (S5K2L3) and tele (S5K3M3) cameras, autofocus | |

Samsung's hardware ISP is not used: its firmware runs inside the vendor
kernel at a fixed address against that kernel's ABI. The sensor init tables
are in the vendor's GPL sources; no Samsung firmware is needed on this path.

## Hardware (vendor DT)

| camera | sensor | bus | address | CSIS / lanes |
|---|---|---|---|---|
| rear 12 MP | S5K2L3 | `hsi2c@10860000` | 0x10 | CSIS0, 4 lanes |
| tele 12 MP | S5K3M3 | `hsi2c@10880000` | 0x2d | CSIS2, 4 lanes |
| front 8 MP | S5K3H1 | `hsi2c@108f0000` (USI08_I2C) | 0x35 | CSIS1, 2 lanes |
| front AF, EEPROM | AK7372, EEPROM | `hsi2c@10900000` (USI09) | 0x0c, 0x51 | |

Power comes from two camera PMICs without mainline drivers: S2MPB02
(`hsi2c@10920000`, USI10, 0x59) and S2MPB03 (`hsi2c@14300000`, 0x56).

## Phase 1: front camera on I2C

`src/camera/s9p-cam-board.c` adds the three buses (gates, USI protocol, pins,
OF changeset) and drives the sensor's reset (gpg2-5) and master clock
(CIS_CLK1 = OSCCLK, 26 MHz, on gpc1-5) through its `rst` and `mclk`
parameters. Power-up, in the vendor's order (`fimc-is-device-module-3h1.c`),
with the S2MPB02 on bus `P`:

```sh
i2ctransfer -y $P w2@0x59 0x2d 0xd8    # LDO16 VDDAF 2.8 V on
i2ctransfer -y $P w2@0x59 0x2a 0xd8    # LDO13 VDDA 2.8 V on
i2ctransfer -y $P w2@0x59 0x23 0xb0    # LDO6  VDDIO 1.8 V on
i2ctransfer -y $P w2@0x59 0x22 0xa4    # LDO5  VDDD 1.05 V on
echo 1 > /sys/module/s9p_cam_board/parameters/rst
echo 1 > /sys/module/s9p_cam_board/parameters/mclk
```

(BUCK2, 1.3 V, is already on from the bootloader.) Results:

| device | read | |
|---|---|---|
| S2MPB02 `BST_CTRL2` (0x1d) | 0x90 | revision 1, the vendor driver's check |
| EEPROM 0x51 @0x0030 | `W08LLKF00AM` | module header (the vendor expects `W08LL`) |
| S5K3H1 `model_id` (0x0000) | 0x3081 | revision (0x0002) 0xa3, `mode_select` 0 (standby) |
| AK7372 (0x03) | 0x0c | |

Power-down is the reverse: `mclk=0`, `rst=0`, LDO16, LDO5, LDO13, LDO6 off
(write the same values with bit 7 clear).

## Phase 2: first look at the ISPPRE block

Every access in `0x162xxxxx` goes through a TZPC that the secure world must
have opened; a rejected access is an SError (a reset). The `pd-isppre` domain
is on from the bootloader (PMU `0x14064048` = 0xf, status `0x0010000f`), and
the vendor only restores the TZPC (SMC `0x82000410`, `0x16210204`) after it
powers the domain on itself: no SMC was sent. Read-only probes, one at a time
(2026-10-15), all answered:

| address | what | value |
|---|---|---|
| `0x16200100` | CMU_ISPPRE user mux | 0x10 (bus clock from CMU_TOP) |
| `0x16201804` | DIV_BUSP | 1 |
| `0x16203024` / `0x1620305c` | QCH CSIS1 / SysMMU | 0x003f0002 |
| `0x16210400` | SYSREG_ISPPRE | 0x3222 |
| `0x16310000` | SysMMU `MMU_CTRL` | 0x4 (bit 0 clear: bypass) |
| `0x16240000` | CSIS1 `CSIS_VERSION` | 0x05010000 (v5.1) |

So the TZPC is open and the receiver is clocked. Next: the CMU_TOP bus divider
(ISPPRE at /16, 25 MHz today), the CSIS1 D-PHY (PMU isolation `0x14060730`),
and the DMA test pattern into a buffer below 4 GiB.

### Phase 2a, first attempt (2026-10-15): no frame yet

`src/camera/s9p-csis-test.c` sets the ISPPRE bus to 200 MHz (CMU_TOP
`0x1a241890` = 1, from /16), configures CSIS1 (two lanes, RAW10 on VC0, the
vendor's interrupt masks), the VC0 write DMA (`0x162c0000`, 8 addresses on
one buffer below 4 GiB, `CTRL = 3`) and the common DMA's test pattern
(`0x162f0000`: priorities, SRAM split, 800x600 at 30 fps), then polls for the
frame end. Every register took its value, but none of the four CSIS DMAs
(`0x162b0000`-`0x162e0000`) reported a frame start within a second, and VC0
reads `ACT_CTRL = 0x0001ff1c` (aborted). Ruled out the same night: the CMU_ISPPRE Q-channels (all
`0x003f0002`, clock requested; `0x1620303c` and `0x16203074-7c` are holes that
abort, as a SIGBUS from user space, which is the safe way to probe), the
common DMA's clock gating (`COMMON_DMA_CLK_CTRL` bit 0 set: no change), and
enabling the CSIS1 link (`link_en=1`: no change; its frame counter stays 0).
Next: the vendor's reset of the common DMA and the exact pattern start
sequence (`csi_hw_s_dma_common_pattern`, `csi_hw_reset`), then a capture with
the real sensor's colour bars (`0x0600 = 2`) through the link instead of the
pattern generator.
