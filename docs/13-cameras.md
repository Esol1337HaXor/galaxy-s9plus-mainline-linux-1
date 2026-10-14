# Cameras (work in progress)

Goal: the cameras as ordinary V4L2 devices that libcamera, PipeWire and every
application see, without Samsung's ISP. The plan, from the vendor kernel:

| phase | what | state |
|---|---|---|
| 1 | front camera powered, sensor answering on I2C | **done** (2026-10-15) |
| 2 | CSIS v5.1 receiver + its DMA writing RAW frames to memory (test pattern first) | |
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
