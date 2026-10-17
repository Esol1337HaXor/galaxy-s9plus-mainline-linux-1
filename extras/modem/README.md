# modem

Boots the Shannon modem (CP) with Samsung's own boot daemon. Experimental: no RIL yet.

The CP (a Samsung Shannon, `ss360ap` in its device tree) is driven by Samsung's `modem_v1`
interface, ported to mainline as `s9p-modem.ko` (`src/modem`). Booting it
needs Samsung's `cbd`, which runs unchanged from the phone's own Android
partitions mounted read-only as a chroot:

- `cbd` loads the firmware from `RADIO` and the NV data from `EFS`, has
  EL3 check its signature, starts the CP and does the boot handshake. The
  CP then comes up `ONLINE` and waits for a RIL on `umts_ipc0`/`umts_rfs0`.
- Nothing from Android is copied: the chroot is `SYSTEM` + `VENDOR` as they
  are on the phone. `EFS` is mounted read-only.
- Not there yet: the RIL (SIM, calls, SMS, mobile data) and the RFS server
  the CP asks for its NV files. Without them the CP stays waiting.
- Never unload `s9p-modem`: it oopses, and the reboot after it hangs. Remove
  the feature and reboot instead.
- Needs the module from `scripts/build/build_extras.sh` in `dist/extras/`.

```bash
sudo extras/install.sh modem
```

How it works: [docs/15-modem.md](../../docs/15-modem.md).
