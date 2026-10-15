# USB-C display, USB host and HDMI audio

Mainline has no DisplayPort for the Exynos 9810, and the port's Type-C
controller driver only switches the USB data lines. `src/usbc-dp/` adds the
whole chain from scratch, written against Samsung's 4.9 kernel for the
SM-G965F (register by register; the vendor sources are GPL):

```
USB-C plug
 ├─ CC/PD  MAX77705 Type-C/PD MCU, I2C 0x25 ........ s9p-usbc.ko
 │          DP alt mode (Discover/Enter/Configure, pin C/D/E), HPD, VBUS
 ├─ VBUS   MAX77705 charger OTG boost (CNFG_00) ...... s9p-usbc.ko
 ├─ SBU    AUX switch on a PCAL6524 expander, I2C 0x22 on hsi2c@10930000
 │          (bus and expander created at run time) .. s9p-dp.ko (board)
 ├─ D+/D-  DWC3 as dual-role, host while we source VBUS  s9p-usbhost.ko
 └─ SS lanes  USB/DP combo PHY (PMA 0x110a0000) ...... s9p-dp.ko (hw)
              DP controller 0x11090000: AUX, DPCD, EDID, link training, SST
              DECON2 0x16050000 + DMA channel VG0 + blender window 4
              DRM device "s9p-dp": one pipe, DP connector, vblank IRQ 194
              SST audio FIFO <- PL330 PDMA0, request 28 .. s9p-dpaudio.ko
```

Everything is a module: build with `scripts/build/build_extras.sh`, install
with `sudo extras/install.sh usb-c-display`. The kernel image needs no
change; two small patches in `src/` make it nicer (below).

## What you get

- GNOME sees a second monitor (`DP-1`, modes from the EDID, 1080p60 by
  default) and lays it out like on a laptop; with `s9p-panel-off` enabled the
  internal panel is left out and the external monitor is the only one.
- Hot-plug, both cable orientations, HPD IRQs (link checked and retrained).
- Keyboards, mice and storage on the adapter's hub (USB 2.0: with pin
  assignment D the two SuperSpeed lanes carry DisplayPort).
- HDMI audio: an ALSA card `S9pDP` with a UCM profile; PipeWire offers it when
  a monitor is connected (jack control `HDMI/DP,pcm=0 Jack`).

## The pieces

### Type-C and DisplayPort alt mode -- s9p-usbc

The MAX77705 runs its own MCU; the AP talks to it with opcodes through the
I2C registers (the vendor `max77705_usbc.c` protocol). The driver polls the
interrupt registers (no IRQ line is wired in the port's DT) and:

- turns the alt mode on (opcode `0x55`), follows CC status and data role;
- when the phone sources (partner is a sink/hub): writes `CONTROL1` (D+/D- to
  the AP) **before** switching VBUS on, then the charger's OTG boost; a hub
  that powers up with open data lines does not enumerate;
- drives Discover Identity / SVIDs / Modes, Enter Mode and DP Configure with
  structured VDMs (opcode `0x48`), falling back to an AP-initiated Discover
  Identity when the MCU stays silent;
- reports `connected / pin / orientation / hpd / irq / configured` to the DP
  driver through a notifier, and whether it sources VBUS (`host`) to the USB
  role driver;
- turns VBUS off in `.shutdown`, so a reboot with a hub plugged does not hand
  a powered port to the bootloader.

It binds to the same compatible as the port's MUIC driver; `s9p-dp load`
unbinds that one first. debugfs: `/sys/kernel/debug/s9p-usbc/{state,log,opcode}`.

### Board -- s9p-dp-board.c

The SBU/AUX switch sits on a PCAL6524 GPIO expander on an I2C bus the port's
DT does not describe. The module opens the bus' clock gates, sets its USI to
I2C, muxes gpp6-2/3, releases the expander reset (gpg3-6) and adds the bus and
the expander with an OF changeset. P0_1 selects the orientation, P0_4 (active
low) enables the switch.

### PHY, controller, link -- s9p-dp-hw.c

- PMA bring-up and the DP mode of the combo PHY for the pin assignment and
  orientation (lane map), PLL lock per link rate.
- Controller: AUX over the SBU pair, DPCD and EDID, link training as the
  vendor does it (clock recovery, channel equalisation, TPS2/TPS3), starting
  at the lowest rate that carries the mode: 1080p60 on two lanes trains at
  HBR (2.7 Gb/s).
- SST video: TU size, MSA, Mvid/Nvid; a BIST mode (colour bars made by the
  controller, no DECON) for bring-up.
- DECON2 with the VG0 DMA channel and blender window 4. DECON2 shares a few
  registers with DECON0 (the panel's): the DP connection select and a clock
  control bit, written with read-modify-write.
- A guard checks every 2 s that the USB PHY driver has not powered the combo
  PHY down under us, and restores it.

### DRM -- s9p-dp-drm.c

A `drm_simple_display_pipe` with a DisplayPort connector, GEM DMA buffers,
fbdev, and vblank from DECON2's frame-start IRQ (GIC SPI 194).

- **Blanking keeps the link up.** When GNOME blanks the screen the driver
  keeps the link trained and scans out a black frame. Some USB-C/HDMI
  adapters go into a deep standby without a video signal, where AUX stops
  answering until a replug. On unblank the controller and PHY are reset and
  the link retrained, as on the first start (see the alignment below).
- **DECON2 starts on the DP vertical sync.** The DP controller's timing
  generator paces DECON2 by pulling pixels, and runs whether DECON2 does or
  not; DECON2 started mid-frame put its first pixel there, and the picture
  came out shifted and wrapped by a random amount, with background lines at
  the bottom. DECON2 is enabled right after `VSYNC_DET` (vsync and back porch
  cover its start). Restarting DECON2, the video stream or even the training
  under a running controller still left it shifted: only a fresh controller
  (soft reset + PHY) starts aligned, so unblank goes through that.
- **Imported GPU buffers.** The IDMA takes 32-bit addresses. The coherent DMA
  mask is 32 bits (dumb and fbdev buffers below 4 GiB) but the streaming mask
  is 64 bits, and `gem_prime_import_sg_table` refuses buffers the IDMA cannot
  scan (scattered or above 4 GiB) at once. Without this every import attempt
  bounced megabytes through swiotlb and the desktop stuttered; with it
  GNOME falls back to a GPU copy into our buffer.
- debugfs `/sys/kernel/debug/s9p-dp/{log,state,regs,cmd}`; `cmd` takes
  `aux on|off`, `up`, `probe`, `train [vic]`, `bist [vic]`, `pattern [vic]`,
  `fill RRGGBB`, `restore`, `off`, `down`, `fake <pin> <cc>`, `hotplug`.

### USB host -- s9p-usbhost

The port's DT pins the DWC3 to peripheral (USB gadget networking). The module
unbinds it, changes its node to `dr_mode = "otg"` + `usb-role-switch`
(default peripheral) and binds it again; then it follows s9p-usbc: host while
the phone sources VBUS, device otherwise, so the USB network to a PC comes
back when the hub is unplugged. The exynos9810 USB PHY driver has no
`set_mode`, so the vendor's host settings (D+/D- pull-downs, no forced VBUS
valid, host column of the HS tuning) are written here.

The port's kernel has no HID support: `build_extras.sh` builds HID (generic,
Logitech DJ/HID++, Apple, Microsoft, multitouch...) as modules.

### HDMI audio -- s9p-dpaudio

The same path as the vendor's `dp_dma.c`: no ABOX, no firmware. The PL330
PDMA0 (0x1a2e0000) copies the PCM ring into the SST1 audio FIFO of the DP
controller (+0x5818), and s9p-dp programs asynchronous Maud/Naud, the audio
InfoFrame and the IEC 60958 channel status. PDMA0 is not in the port's DT: it
is added with an OF changeset and registered on the AMBA bus by hand (the
upstream `pl330` driver takes it). S16 and S24 stereo, 32-96 kHz.

## Kernel patches (optional, in src/)

- `src/usb/phy-exynos9810-usbdrd.c` exports `s9p_usbdp_dp_hold()`: while DP
  uses the combo PHY, USB PHY power-down leaves the PMA, its PMU isolation and
  the PHY reset alone.
- `src/gpu/drm/s9p/s9p-decon.c` writes DECON0's shadow-update request with
  read-modify-write, so the panel driver does not clear the global request DP
  depends on.

Without them the modules still work (the guard restores the PHY).

## Using it

```bash
sudo s9p-dp status              # Type-C, link, DECON, DRM
sudo s9p-dp bist                # colour bars from the DP controller
sudo s9p-dp logs                # everything into ./logs-dp-<date>/
```

Module parameters go to `/etc/s9p-dp.conf` (one line, `name=value ...`):

| s9p-dp parameter | default | |
|---|---|---|
| `max_rate` | `0x0a` | highest link rate code: `0x06` 1.62, `0x0a` 2.7, `0x14` 5.4 Gb/s |
| `max_clock` | 148500 | highest pixel clock offered (kHz); 297000 for modes above 1080p60 |
| `keep_link` | Y | keep the link up with a black frame while blanked |
| `chan` | `vg0` | DECON DMA channel (`g1` is the vendor's) |
| `pixel_order` | 0 | 4 if red and blue come out swapped |
| `orient` | auto | `cc` / `gpio`: where the cable orientation comes from |
| `force_hpd` | 0 | force the controller's internal HPD when AUX does not start |

## Known limits

- USB on the hub is USB 2.0.
- With the battery nearly empty (about 5 %) the MAX77705 no longer completes
  the power negotiation with the adapter: no PD contract, no alt mode, no USB.
  Charge the phone, or use an adapter with a USB-C PD input.
- The internal panel's DECON0 keeps whatever sboot set up when
  `s9p-panel-off` is enabled (it is only left out of the desktop).

## Hurdles

| Symptom | Cause | Fix |
|---|---|---|
| Hub devices: "Cannot enable. Maybe the USB cable is bad?" after a replug | the MCU resets the D+/D- switch on detach; the hub powered up with open lines | `CONTROL1` written before VBUS on every attach |
| 1 s freezes in GNOME after unplugging the monitor | output stopped under an enabled CRTC, every commit waited 100 ms for a vblank | `drm_crtc_vblank_off()` when the link goes away |
| Desktop stutters on the monitor, `swiotlb buffer is full` | GPU buffers above 4 GiB bounced through swiotlb on every import attempt | 64-bit streaming mask, refuse unscannable imports |
| No picture after the screen blanked, AUX dead | the adapter slept without a video signal | keep the link up with a black frame |
| Picture shifted and wrapped (different every time), black lines at the bottom | DECON2 started at a random point of the DP frame; the controller kept its place across restarts | DECON2 on `VSYNC_DET`; controller + PHY reset on unblank (measured with an HDMI capture: offset 0 on boot and 6/6 unblanks) |
| Reboot with a hub plugged waited for an unplug | VBUS stayed on into the bootloader | VBUS off in `.shutdown` |
| AUX and the hub dead late at night | battery at 5 %: no PD contract | charge the phone |
| Phone stopped charging after using a hub | the charger was left in a mode without charging when OTG went off; input limit stayed at 500 mA | `CNFG_00` back to charge mode when OTG goes off; input limit from the Type-C current the source advertises (0.5 / 1.5 / 3 A), battery charge current to match (450 / 900 / 1500 mA) |
