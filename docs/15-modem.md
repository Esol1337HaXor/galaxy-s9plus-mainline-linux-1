# Modem

State (2026-10-17): the CP boots on every boot from the phone's own
firmware, and Samsung's own RIL runs on top of it: the SIM is read, the radio
reaches the network and a call is dialled, but **calls do not get through
yet**: they stay in DIALING for 30-60 s and are released (see below). No
call audio, SMS or data yet, and no desktop integration: the test client is
`s9p-radio`.

## Pieces

| | what | where |
|---|---|---|
| kernel | Samsung's `modem_v1` (SS310AP over shared memory, SBD IPC), `mcu_ipc`, `shm_ipc` and the CP part of `pmu-cp`/`pmucal`, from the SM-G965F 4.9 kernel | `src/modem` → `s9p-modem.ko` |
| device tree | `mcu_ipc@14170000`, `shmem@f6e00000`, `mif_pdata` (from the stock DT, no GPIOs) | `exynos9810-star2lte.dts` |
| memory | CP region `0xf0000000` (`0x9200000`) and cp-logging `0xf9200000`, reserved | same DT |
| userspace | Samsung's `cbd`, unchanged, from `VENDOR` | `extras/modem` (`s9p-modem.service`) |
| RIL | Samsung's `rild` + `libsec-ril.so` under Android's own `init`, in a container | `extras/modem` (`s9p-android.service`) |
| kernel, RIL | binder (binderfs) and a patch for binder without an LSM | `build71.sh`, `patch_tree.py` |
| test client | `s9p-radio` (libgbinder-radio): status, radio power, PIN, dial | `src/modem/userspace` |

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

## The RIL: Samsung's rild in an Android container

`libsec-ril.so` serves both the IPC channel (`umts_ipc0`, Samsung IPC) and the
CP's remote file system (`umts_rfs0`) itself, and it is a HIDL service:
`IRadio@1.4`, `ISehRadio@2.1`, `IRadioConfig`, `ISap`, over hwbinder. So
it needs Android's property service, `hwservicemanager` and friends.
`s9p-android.service` runs Android's own `init` (second stage) from the
phone's `SYSTEM`, in its own PID and mount namespaces, with a short
`init.rc`: logd, the three service managers, the suspend HAL and rild.

What makes Android's userspace run on Ubuntu's kernel:

- **No SELinux.** `s9p-noselinux.so` (LD_PRELOAD, built for bionic)
  answers the libselinux calls init and the service managers make: every
  check allowed, made-up but consistent contexts. In the kernel, binder
  asks the LSM for the caller's context when the target node wants it (the
  service managers do) and fails the transaction without one:
  `S9P-BINDER-NOLSM` in `patch_tree.py` sends none instead.
- **First-stage init is skipped**: `INIT_STARTED_AT`/`INIT_SELINUX_TOOK`
  in the environment, `/dev/socket` and `/dev/.coldboot_done` prepared.
- **Nothing reaches the host**: a private `/dev` (binderfs, `umts_*`,
  `mbin0` = `RADIO`), a private `/proc` with `sysrq-trigger` on
  `/dev/null` and `/proc/sys` read-only (the sysctls init insists on
  writing are fake files), a fake `/sys/power` (the suspend HAL never
  suspends anything: autosuspend is system_server's call). A fatal init
  error reboots only the namespace.
- **EFS stays read-only.** The CP writes NV files through RFS to `/efs`: an
  overlay with the real `EFS` underneath and its upper layer in
  `/var/lib/s9p-modem/efs`.
- The binderfs instance is mounted on the host (`/run/s9p-modem/binderfs`,
  `/dev/hwbinder`), so host clients such as libgbinder reach the same
  service managers.
- rild's wakelocks wait for `ISystemSuspend`: without the suspend HAL
  every request blocks forever.

`s9p-logcat` reads the container's log. Samsung's ship build logs little.

Known gaps: rild waits about 10 s each time it tries to reach the IMS
service (`imsd`, not running), and `getVoiceRegistrationState` /
`getOperator` get no answer yet; `dial` does.

## Calls (2026-10-17, Movistar SIM, PLMN 214-07)

The IPC trace (`options s9p-modem pkt_dump_len=96` prints whole Samsung IPC
messages; main/sub/type at bytes 8-10) shows the call: `CALL_OUTGOING` is
accepted (`GEN_PHONE_RES 0x8000`), `CALL_STATUS` says DIALING, and after
30-60 s RELEASED; it never reaches ALERTING. On LTE the CP stays on LTE
(`NET_REGIST` does not change), so the circuit-switched fallback a call
needs without VoLTE/IMS does not complete. With 2G/3G forced, one call got
the network's ringback tone; with 2G only there is no `NET_REGIST` at all.
Next: decode `NET_REGIST` (registration and reject cause per domain), and
find what Android's IMS stack or telephony framework normally provides.

## Next

- Call audio: the CP's voice path runs through ABOX (the `VSS` image).
- The missing answers (registration, operator), the IMS wait.
- A desktop stack: oFono with its binder plugin (Sailfish/Droidian) on
  `/dev/hwbinder`, for GNOME Calls, SMS and mobile data on `rmnet0-7`.
