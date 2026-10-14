# bluetooth

Bluetooth of the BCM4361 combo (the Wi-Fi half already works over PCIe).

`s9p-bt-board` adds the chip's UART (`uart@10840000`) with a `bluetooth`
child: samsung_tty hands it to serdev and upstream `hci_bcm` powers the chip,
loads its patchram and runs it at 3 Mbaud with sleep control. The Bluetooth
stack (`bluetooth`, `hci_uart`, `btbcm`, `rfcomm`, `bnep`, `hidp`) is built
as modules by `scripts/build/build_extras.sh`; the kernel image is not changed.

```bash
sudo extras/install.sh usb-c-display   # needed: BT_REG_ON is on its GPIO expander
sudo extras/install.sh bluetooth
bluetoothctl                           # or GNOME Settings > Bluetooth
```

The installer takes two things from the phone itself, read-only:

- the patchram `bcm4361B2_semco.hcd` from the Android `VENDOR` partition, as
  `/lib/firmware/brcm/BCM.samsung,star2lte.hcd` (falls back to the image's copy);
- the phone's Bluetooth address from `EFS` (`bluetooth/bt_addr`), as
  `options s9p_bt_board bd_addr=...` in `/etc/modprobe.d/s9p-bt.conf`. Without
  it every S9+ would use the chip's default `43:61:B2:00:1F:AC`.

`hciattach-bcm4361.service` of the image is disabled: it cannot work with the
serdev UART. Details: [docs/11-bluetooth.md](../../docs/11-bluetooth.md).
