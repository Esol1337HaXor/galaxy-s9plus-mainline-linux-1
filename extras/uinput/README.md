# uinput

`/dev/uinput`, for programs that make virtual input devices: Galaxy
Companion's button actions (the Bixby button pressing volume, media or
custom keys), remote desktop input, key remappers.

The stable kernel is built without `CONFIG_INPUT_MISC`, so `uinput` is not
there; this builds the kernel's own `drivers/input/misc/uinput.c` as a module
(like the vibration driver, from the same directory) and loads it at boot.
Who may open it is up to the program that needs it (Galaxy Companion gives it
to the `input` group).

```bash
sudo extras/install.sh uinput
```

Needs `scripts/build/build_extras.sh`.
