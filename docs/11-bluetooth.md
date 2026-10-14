# Bluetooth

The BCM4361 is a Wi-Fi + Bluetooth combo. Wi-Fi goes over PCIe
(`brcmfmac`); Bluetooth over a UART the port's device tree does not describe.
`src/bluetooth/` adds it, written against Samsung's 4.9 kernel
(`drivers/bluetooth/bcm43xx.c`, `drivers/tty/serial/samsung.c`) and DT.

```
BCM4361 (BT part)
 ├─ UART   uart@10840000 (UART_BT, PERIC1), GIC SPI 422, FIFO 256, RTS/CTS
 │          pins gpq0-0..3 RXD TXD RTS CTS, function 2, pull-up
 ├─ BT_REG_ON  PCAL6524 @0x22 line 7 (the expander s9p-dp adds)
 ├─ BT_WAKE    gpg2-0, active high
 └─ HOST_WAKE  gpa2-3, active high (wakeup EINT)
```

## The pieces

| piece | does |
|---|---|
| `s9p-bt-board` | opens the UART_BT clock gates, pulses the block's USI reset (`USI_CON`, `USI_OPTION = CLKREQ_ON`, as the vendor does on every clock enable), muxes gpq0, and adds `serial@10840000` (`samsung,exynos8895-uart`, 256-byte FIFO, the 200 MHz clock stub) with a `bluetooth` child |
| upstream `hci_bcm` | `brcm,bcm4345c5` (there is no 4361 compatible; this one sets no quirks): `shutdown-gpios` = BT_REG_ON, `device-wakeup-gpios`, `host-wakeup-gpios`, `max-speed` 3 Mbaud, `local-bd-address` |
| `btbcm` + `src/bluetooth/btbcm-bcm4361.patch` | the BCM4361B2's default address is marked invalid, so the address from the DT is used; LE Read Transmit Power is marked broken on this board (advertised, fails, and aborted the init) |
| Kconfig `src/bluetooth/extras.config` | `BT=m` and friends, as modules. `RFKILL` is built into the stable kernel (`build71.sh`): GNOME only shows Bluetooth through rfkill, and as a module it would turn the built-in cfg80211 into one (`depends on RFKILL \|\| !RFKILL`) |

The UART block clock is 200 MHz (CMU divider at /1), the same as the port's
clock stub, so the baud rates samsung_tty computes are exact.

## Firmware and address

`hci_bcm` asks for `brcm/BCM.samsung,star2lte.hcd` (the board compatible):
the installer copies Android's `/vendor/firmware/bcm4361B2_semco.hcd` there
(the module is SEMCO, as the Wi-Fi NVRAM). The log then reads
`BCM4361B2 Crown E32A ANT1 [Baseline: 0111]`.

The chip boots with `43:61:B2:00:1F:AC` on every phone. Samsung keeps the real
address in the `EFS` partition (`bluetooth/bt_addr`); the installer writes it
to `/etc/modprobe.d/s9p-bt.conf` as `bd_addr`, and `s9p-bt-board` turns it
into `local-bd-address`.

## Known limits

- Needs `extras/usb-c-display` (s9p-dp creates the expander with BT_REG_ON).
- SCO audio over the PCM link (calls on a headset) goes through ABOX UAIF3 on
  stock and is not done; A2DP over HCI is not affected.

## Hurdles

| Symptom | Cause | Fix |
|---|---|---|
| hci0 registered but never listed by mgmt | `Opcode 0x204b failed: -38`: LE Read Transmit Power advertised but not supported, init aborted | quirk in the btbcm patch |
| every phone with the same address | chip default address, not in btbcm's list | mark it invalid, `local-bd-address` from EFS |
| boot unit hung | `btmgmt` waits forever with stdin on `/dev/null` | no btmgmt: the address goes through the DT |
| GNOME Settings: "No Bluetooth Found" while BlueZ works | GNOME looks for the adapter through rfkill, and the kernel had none | `RFKILL=y` in the stable kernel |
