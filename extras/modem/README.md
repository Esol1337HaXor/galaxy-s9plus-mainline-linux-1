# modem

Boots the Shannon modem, runs Samsung's own RIL on it and brings mobile data up. Experimental: no calls or SMS yet.

The CP (a Samsung Shannon, `ss360ap` in its device tree) is driven by Samsung's `modem_v1`
interface, ported to mainline as `s9p-modem.ko` (`src/modem`). Booting it
needs Samsung's `cbd`, which runs unchanged from the phone's own Android
partitions mounted read-only as a chroot:

- `cbd` loads the firmware from `RADIO` and the NV data from `EFS`, has
  EL3 check its signature, starts the CP and does the boot handshake. The
  CP then comes up `ONLINE` and waits for a RIL on `umts_ipc0`/`umts_rfs0`.
- Nothing from Android is copied: the chroot is `SYSTEM` + `VENDOR` as they
  are on the phone. `EFS` is mounted read-only.
- `s9p-android.service` runs Samsung's `rild` under Android's own `init`
  in a container (private namespaces, nothing reaches the host); EFS stays
  read-only, what the CP writes goes to `/var/lib/s9p-modem/efs`.
- Test it with `s9p-radio` (`src/modem/userspace`, needs libgbinder-radio):
  `status`, `on`, `pin`, `dial`, `hangup`. `s9p-logcat` shows rild's log.
- `s9p-mobile.service`: mobile data at every boot (LTE), behind Wi-Fi;
  settings in `/etc/default/s9p-modem`.
- Not there yet: calls (they need VoLTE on networks without 3G), call
  audio, SMS, a toggle in GNOME.
- Never unload `s9p-modem`: it oopses, and the reboot after it hangs. Remove
  the feature and reboot instead.
- Needs the module from `scripts/build/build_extras.sh` in `dist/extras/`.

```bash
sudo extras/install.sh modem
```

How it works: [docs/15-modem.md](../../docs/15-modem.md).
