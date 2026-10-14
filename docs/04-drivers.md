# The drivers

Every file in `src/` is here because mainline has no Exynos 9810 support for
that block. `scripts/build/apply_drivers.sh` installs them into the kernel
tree before every build, so the tree itself stays a pristine checkout.

| src/... | lands in the tree as |
|---|---|
| `ufs/ufs-exynos9810.c`, `ufs/ufs-cal-9810.[ch]` | `drivers/ufs/host/` |
| `soc/s9p-acpm.c`, `s9p-g3d.c`, `s9p-g3d-pd.c`, `s9p-clkgate.c`, `s9p-max77705-muic.c` | `drivers/soc/samsung/` |
| `soc/s9p-acpm.h` | `include/linux/soc/samsung/` |
| `cpufreq/s9p-cpufreq.c` | `drivers/cpufreq/` |
| `pci/pcie-exynos9810.c` | `drivers/pci/controller/dwc/` |
| `usb/phy-exynos9810-usbdrd.c` | `drivers/phy/samsung/` |
| `gpu/drm/s9p/` | `drivers/gpu/drm/s9p/` |
| generated at build time | `drivers/gpu/drm/sysfb/exynos-bootfb.c` |
| `mainline-dts/exynos9810-star2lte.dts` | `arch/arm64/boot/dts/exynos/` |

## UFS -- src/ufs/

Mainline's `ufs-exynos.c` covers exynos7, exynosauto, fsd and gs101. The 9810
sits between them and its PHY calibration matches none of them. Rather than
guess a variant, this port keeps **Samsung's own `ufs-cal-9810` sequence** --
the code that shipped on this phone, unchanged -- and rewrites only the glue
against the current `ufshcd` variant API:

| hook | does |
|---|---|
| `.hce_enable_notify(PRE)` | host software reset, host init, device reset |
| `.link_startup_notify(PRE/POST)` | `ufs_cal_pre_link` / `ufs_cal_post_link` |
| `.negotiate_pwr_mode` | clamp to what the board asks for: HS gear 3, 2 lanes, series B |
| `.pwr_change_notify(PRE/POST)` | `ufs_cal_pre_pmc` / `ufs_cal_post_pmc` |
| `.hibern8_notify` | `ufs_cal_pre_h8_exit` / `ufs_cal_post_h8_enter` |

The calibration derives every UNIPRO timer from the UNIPRO clock, so that rate
has to be right -- see [01-hardware.md](01-hardware.md).

## Display

Two drivers, in the order the boot uses them.

### exynos-bootfb.c -- the bootloader framebuffer as KMS

Samsung's bootloader leaves DECON scanning out the framebuffer at
`0xcc000000` (it is what shows the boot logo) and keeps refreshing it. That
buffer is a working scanout with no display driver at all -- which is exactly
what `simpledrm` drives.

What `simpledrm` cannot do is get a GPU attached to it. Mesa pairs a
display-only KMS driver with a render node through **kmsro**, and kmsro works
off a list of driver names. `simpledrm` is not on that list; `exynos` is.

So the driver is `simpledrm` **verbatim**, regenerated from the kernel tree
with `sed` on every build so it cannot drift from the kernel it is compiled
against, with `DRIVER_NAME` changed to `exynos`.

### gpu/drm/s9p/s9p-decon.c -- the real scanout driver

`DECON=1` hands the display over from the bootloader framebuffer to a
`drm_simple_display_pipe` that programs DECON's own IDMA address register.
The driver evicts simpledrm itself with
`aperture_remove_conflicting_devices()`, so early boot is identical to the
proven bootfb builds.

Three things in it are load-bearing.

**The flip latch.** A page flip writes the new scanout address and then has to
wait for DECON's shadow registers to take it (`decon_reg_update_req_and_unmask`
then `wait_update_done_and_mask`, the vendor's handshake). Without the wait
the address lands mid-frame: the user sees the *old* contents for one frame
and the new ones appear a moment later -- the "first an artefact, then it
loads" symptom.

    obj = to_drm_gem_dma_obj(fb->obj[0]);
    s9p_decon_program_address(priv, obj->dma_addr);
    if (s9p_wait_shadow_idle(priv))
            drm_err_ratelimited(&priv->drm, "flip did not latch within 50 ms");

**The poll period.** `SHADOW_POLL_US` is 250 us, not 10. At 10 us the wait
alone was about 1600 wakeups per frame.

**Panel power, not just backlight.** `s9p_panel_down()` sends DCS `0x28`
(display off), waits 20 ms, then DCS `0x10` (sleep in) and waits 120 ms;
`s9p_panel_up()` reverses it. Switchable with the `panel_sleep` module
parameter, default on. Without the sleep-in the panel supply stays up and the
backlight is visibly glowing on a screen that is meant to be off.

## GPU -- src/soc/s9p-g3d.c

Panfrost drives the Mali-G72, but on this SoC nothing else sets up its clock
or its rail. This driver is both the clock provider and the DVFS actor.

**Three independent rate requests, lowest wins:** `g3d_req_khz` (devfreq),
`g3d_thermal_khz` (the TMU cap), `g3d_max_khz` (a user ceiling).
`s9p_g3d_apply()` takes `min3()` of them.

**It is a platform driver on `samsung,exynos9810-g3d-clock`, and it publishes
the clock only once the rail is up.** That ordering is the whole point:
`s9p_g3d_rail_up()` raises BUCK6 to 950 mV first, and until that succeeds
probe returns `-EPROBE_DEFER`, so panfrost cannot bind and cannot touch the
GPU at sboot's 260 MHz on 644 mV. Two builds that let panfrost probe at 0.9 s,
before the rail was up, hung the boot.

**The mux switch is glitch-free.** Clearing `MUX_SEL` and `PLL_CON0_ENABLE` in
one write leaves a window where the mux has not finished moving and the PLL is
already down -- the G3D block briefly has no clock at all, and a GPU job in
flight across that window comes back as `JOB_BUS_FAULT` or
`DATA_INVALID_FAULT`. Once devfreq started changing the rate on its own, those
faults landed within milliseconds of every single rate change. The driver now
switches to the oscillator as a step of its own, with a `PLL_CON0_MUX_BUSY`
poll, before the PLL is touched -- and again on the way back.

**Clock provider API:** `determine_rate()`, not `round_rate()`; 7.1 removed
the latter. `CLK_GET_RATE_NOCACHE`, because the rate changes underneath the
framework.

`step_volt=1` turns on per-step rail voltages (950 / 900 / 850 mV). It is off
by default -- see [06-known-issues.md](06-known-issues.md).

### s9p-g3d-pd.c

Keeps the G3D power domain on. Without it the domain drops as soon as nothing
holds a runtime-PM reference, and the next register read is a bus hang.

## CPU frequency -- src/cpufreq/s9p-cpufreq.c

sboot leaves both clusters at about 1.05 GHz and there is no upstream cpufreq
driver for this SoC. This one drives both cluster PLLs and both rails through
the ACPM mailbox, and arbitrates through **cpufreq QoS** so the governor and
`policy->cur` stay truthful:

| request | holds |
|---|---|
| thermal | the TMU cap from [01-hardware.md](01-hardware.md) |
| busy-core | the rule that keeps one cluster from starving the other |
| `qos_idle` | the optional idle floor, off by default |

Rail changes follow the safe ordering in both directions: voltage before clock
going up, clock before voltage coming down.

## ACPM -- src/soc/s9p-acpm.c

The S2MPS18 PMIC is only reachable through the APM firmware's mailbox. This
driver implements that protocol in the kernel; `s9p-acpm` in
`extras/debug-tools/` implements the same thing from userspace
through `/dev/mem` for diagnosis.

Only 32-bit accesses work, and the first three SRAM pages fault when mapped.

> One bug here cost a lot of time: the two ring buffers were swapped. The
> symptom is a mailbox that looks like it works and returns nothing.

## Clock gates and PMU bits -- src/soc/s9p-clkgate.c

A small driver that applies literal register writes from the device tree
before anything else probes, for blocks with no upstream clock driver:

    s9p,set-registers   = <addr value>, ... ;   /* write value */
    s9p,clear-registers = <addr bits>,  ... ;   /* mask bits out */

`clear-registers` is what makes the watchdog work: PMU
`MASK_WDT_RESET_REQUEST` (`0x1406040c`, bit 23) is set by the bootloader, so
an expired watchdog raises nothing at all. Clearing it turns the register from
`ffffffff` into `ff7fffff`, and the log line proves it happened:

    s9p-clkgate: 1406040c: ffffffff & ~00800000 -> ff7fffff

The same mechanism arms WDT1 from the very first boot instruction:

    <0x10060004 23805>,     /* WTDAT */
    <0x10060008 23805>,     /* WTCNT */
    <0x10060000 0xff39>,    /* prescaler 255, div 128, enable, reset */

## PCIe and Wi-Fi -- src/pci/pcie-exynos9810.c

A DesignWare root complex with the vendor PHY calibration. Three things that
each cost a boot:

* **RAM base `0x170000`** for the BCM4361 firmware.
* **INTx, not MSI.**
* **`dma-coherent` in the device tree is fatal.** Leaving it out is the fix.

Firmware and the regulatory database are built into the kernel with `WLAN=1`,
so there is no firmware-loading race at boot.

## USB -- src/usb/phy-exynos9810-usbdrd.c

The vendor CAL sequence for PHY revision `0x300`. With `GADGET=configfs` the
gadget provides an ACM console (`getty` on `ttyGS0`) and an NCM network
interface; the phone is `172.16.42.1` and serves DHCP to the host.

## MUIC -- src/soc/s9p-max77705-muic.c

TWRP resets the MAX77705, which leaves the charger input limit at 375 mA --
low enough that the phone discharges while plugged into a PC. This driver and
the `s9p-touch-power` boot service put it back to 500 mA.

## Charger (charge stop) -- src/soc/max77705_charger.c

The upstream MAX77705 charger driver this port ships with has no way to stop
charging: its only writable knobs are current limits with a lower clamp, which
is throttling, not an interruption. Since this phone runs as an always-plugged
server, `src/soc/max77705_charger.c` replaces the pristine upstream file during
`apply_drivers.sh` and adds the standard
`POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR` property:

    /sys/class/power_supply/max77705-charger/charge_behaviour
        auto              charging enabled (CHG_EN = 1)
        inhibit-charge    charging stopped in the IC (CHG_EN = 0)

The property maps straight onto the `CHG_EN` regmap field the driver already
uses internally (`max77705_charger_enable/disable`), and the psy descriptor
advertises both behaviours via `charge_behaviours`. A small userspace
charge limiter can then hold the battery in, say, a 75-80 % window by writing
this file.

Guard rails: `apply_drivers.sh` only overwrites the tree's driver after
comparing it against the pinned upstream copy in
`src/reference/max77705_charger.c` (torvalds/linux v7.1, unmodified); if a
mainline rebase changed the upstream file, the build aborts instead of
silently patching stale code. Re-check `src/soc/max77705_charger.c` after any
rebase.

Verified on the device: after flash and reboot `charge_behaviour` reads
`auto [inhibit-charge]`, writing `inhibit-charge` stops the charge current,
writing `auto` resumes it, and the battery gauge reports Discharging/Charging
accordingly.

## Device tree -- src/mainline-dts/exynos9810-star2lte.dts

On top of the upstream board file: UFS, the Mali node, the G3D clock and power
domain, ramoops, reboot-mode, the watchdog, the touch controller and the CMA
region.

The CMA node is the single most important line in the file:

    display_cma: linux,cma {
            compatible = "shared-dma-pool";
            reg = <0x0 0x98000000 0x14000000>;      /* 320 MiB */
            reusable;
            linux,cma-default;
    };

It must be **below 4 GB**, because DECON's scanout address register is 32 bit.
See [05-performance.md](05-performance.md).

The GPU's OPP table lives **inside** the `gpu` node, not as a sibling.
`fw_devlink` treats `operating-points-v2` as a supplier link, and with the
table as a sibling panfrost's probe was delayed to 14.3 s.

## Out-of-tree modules -- src/usbc-dp/, src/sound/, src/leds/, src/bluetooth/, src/haptic/

Not installed into the tree: `scripts/build/build_extras.sh` builds them
against the finished stable kernel, together with ALSA, ASoC, HID and the
Cirrus madera drivers as modules, for the optional `extras/usb-c-display`,
`extras/audio`, `extras/notification-led`, `extras/bluetooth` and `extras/vibration`.

| src/... | does | documented in |
|---|---|---|
| `usbc-dp/s9p-usbc.c` | MAX77705 Type-C/PD: DisplayPort alt mode, VBUS, data role | [08-usb-c-display.md](08-usb-c-display.md) |
| `usbc-dp/s9p-usbhost.c` | DWC3 as dual-role, host on hubs and docks | [08-usb-c-display.md](08-usb-c-display.md) |
| `usbc-dp/s9p-dp-*.c` | DP PHY, controller, link training, DECON2, DRM device | [08-usb-c-display.md](08-usb-c-display.md) |
| `usbc-dp/s9p-dpaudio.c` | HDMI audio: PL330 into the DP audio FIFO | [08-usb-c-display.md](08-usb-c-display.md) |
| `sound/s9p-audio-board.c` | codec SPI and amplifier I2C buses, codec power | [09-audio.md](09-audio.md) |
| `sound/s9p-abox.c` | ABOX DSP: SysMMU, stock firmware boot, IPC | [09-audio.md](09-audio.md) |
| `sound/s9p-abox-pcm.c` | the ASoC card: speaker and headphone PCMs | [09-audio.md](09-audio.md) |
| `sound/s9p-max98512.c` | the MAX98512 amplifiers | [09-audio.md](09-audio.md) |
| `leds/s9p-leds.c`, `leds/leds-max77705.c` | the RGB LED node, upstream LED driver with a current ceiling | [10-notification-led.md](10-notification-led.md) |
| `bluetooth/s9p-bt-board.c`, `bluetooth/btbcm-bcm4361.patch` | the Bluetooth UART and its serdev child; btbcm fixes for the BCM4361B2 | [11-bluetooth.md](11-bluetooth.md) |
| `haptic/s9p-haptic.c`, `haptic/max77693-haptic.c` | the PWM block and haptic node; upstream haptic driver with limits | [12-vibration.md](12-vibration.md) |
