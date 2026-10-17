# Modem

State (2026-10-17): the CP boots and comes up `ONLINE` on every boot, from
the phone's own firmware. There is no RIL yet, so no SIM, calls, SMS or data.

## Pieces

| | what | where |
|---|---|---|
| kernel | Samsung's `modem_v1` (SS310AP over shared memory, SBD IPC), `mcu_ipc`, `shm_ipc` and the CP part of `pmu-cp`/`pmucal`, from the SM-G965F 4.9 kernel | `src/modem` → `s9p-modem.ko` |
| device tree | `mcu_ipc@14170000`, `shmem@f6e00000`, `mif_pdata` (from the stock DT, no GPIOs) | `exynos9810-star2lte.dts` |
| memory | CP region `0xf0000000` (`0x9200000`) and cp-logging `0xf9200000`, reserved | same DT |
| userspace | Samsung's `cbd`, unchanged, from `VENDOR` | `extras/modem` |

`s9p-modem.ko` registers, in this order, the CP PMU (`s9p-cp-pmu.c`),
`shm_ipc`, `mcu_ipc` and the modem interface (`s9p-modem-core.c`). The vendor
files are kept as close to the original as the 7.1 API allows; the rest is
shims under `src/modem/include` and `s9p-compat.h`.

## Boot sequence

`cbd -tss310 -bm -mm -P …/RADIO -n /mnt/vendor/efs`, as `init.baseband.rc`:

1. reads the TOC of `RADIO`: `BOOT`, `MAIN`, `VSS`, and `NV` from
   `EFS/nv_data.bin`;
2. `IOCTL_SECURITY_REQUEST` mode 2 (non-secure): first CP init
   (`CP_CTRL_NS`/`CP_CTRL_S` through SMC `0x82000700`);
3. writes the four images into the CP region through `umts_boot0`;
4. `IOCTL_SECURITY_REQUEST` mode 0: EL3 verifies the signed images
   (`CP_NO_ERROR`);
5. `IOCTL_MODEM_ON`: `AP2CP_CFG`, reset release; `BOOT_ON`, `DL_START`
   (magic `BOOT`), the `0x900d`/`0xa00d` and `0x9f00`/`0xaf00` handshake,
   `BOOT_OFF`;
6. the CP sends `INIT_START`, the AP sets up the SBD ring buffers and answers
   `PIF_INIT_DONE`, the CP sends `CP_START`: state `ONLINE`.

From there the CP repeats `CP_START` every 100 ms until a RIL opens
`umts_ipc0` and `umts_rfs0`.

## What it took

- **Shared memory is a carveout.** It is outside the linear map, so the
  vendor's `vmap` of its pages fails: `shm_ipc` maps the regions with
  `ioremap_wc`. The zero-copy buffers (ZMB) are therefore uncached, and the
  vendor's `dma_map_single` of them, there only for cache maintenance, is
  dropped.
- **The ACPM log region** is not in our DT; `shm_ipc` falls back to the 9810
  SRAM window (`0x0203c000`, `0x1d000`).
- **cbd runs as Android's radio user.** It `setuid(1001)` with keep-caps and
  never sets its groups, so the `system:radio 0660` of `ueventd.rc` is not
  enough: udev gives `umts_*` and `RADIO` to uid 1001.
- **Android in a chroot.** `SYSTEM` and `VENDOR` read-only; the APEX
  directories under `/system/apex` bind-mounted on `/apex` (bionic lives
  there on Android 10); `EFS` read-only on `/mnt/vendor/efs`. The missing
  property service, `logd` and wakelock files only cost log lines.
  `NV validation TIMEOUT` (5 s) is cbd waiting for an Android property.
- **Never `rmmod s9p-modem`.** `misc_deregister` oopses, and before kernel
  #18 the reboot after it hung with the watchdog disarmed.

## Next

- RFS: the CP asks for its NV files over `umts_rfs0` (Samsung's `rfsd`, or
  libsamsung-ipc's implementation).
- RIL on `umts_ipc0` (Samsung IPC FMT): Samsung's `libsec-ril` needs most of
  Android; the open path is libsamsung-ipc + oFono/ModemManager, which do not
  know this modem generation yet.
- Data on `rmnet0-7`.
