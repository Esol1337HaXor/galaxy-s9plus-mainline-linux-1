# Performance, and how each number was measured

Four changes turned this port from "it runs" into "it runs well". Each one is
here with the measurement that justified it, because three of them looked
like something else first.

## 1. CMA -- the reason everything felt slow

**Symptom:** the desktop lagged, the compositor missed frames, and the log
was full of scanout buffer allocation errors.

**Cause:** the CMA region was 128 MiB. One 1440x2960x4 frame is 16.3 MiB, so
exactly one fitted. The second `DRM_IOCTL_MODE_CREATE_DUMB` returned `ENOMEM`,
and the compositor fell back to a CPU copy for every frame.

**Fix:** a 320 MiB `linux,cma` region at `0x98000000` -- below 4 GB, because
DECON's scanout address register is 32 bit.

| | before | after |
|---|---|---|
| allocation errors | continuous | 0 |
| frames reaching the panel | CPU copy path | 60/s |

## 2. The flip latch -- "first an artefact, then it loads"

**Symptom:** clicking anything showed garbage for a moment before the real
content appeared.

**Cause:** the page flip programmed DECON's scanout address and returned
without waiting for the shadow registers to latch it. The address took effect
mid-frame.

**Fix:** the vendor's `wait_update_done_and_mask` half of the handshake, with
a 250 us poll and a 50 ms timeout. See [04-drivers.md](04-drivers.md).

## 3. GPU rail before GPU clock

**Symptom:** `DATA_INVALID_FAULT` from Panfrost every few seconds, and two
kernels that hung during boot.

**Cause:** sboot leaves BUCK6 at 644 mV and the GPU at 260 MHz. Panfrost was
binding at 0.9 s and driving the GPU on that rail.

**Fix:** `s9p-g3d` became a platform driver that raises BUCK6 to 950 mV first
and only then publishes the clock. Until it does, panfrost gets
`-EPROBE_DEFER`. Plus the glitch-free mux switch, which removed the faults
that came with every devfreq rate change.

| | before | after |
|---|---|---|
| GPU faults after a full load run | 16 in 2 min | 0 |
| temperature under GPU load | 66 C | 58 C |
| panfrost probe | 14.3 s | 0.9 s |

The probe time also needed the GPU's OPP table moved **inside** the `gpu`
node: `fw_devlink` treats `operating-points-v2` as a supplier link, and a
sibling table delayed the probe by 13 seconds on its own.

## 4. AFBC repacking -- the artefacts in icons and text

**Symptom:** icons and glyphs showed **horizontal dashes exactly 16 pixels
long**. glmark2 was completely clean; only the desktop was affected.

That combination is not a contradiction, it is the clue. glmark2 draws
geometry and loads almost no textures; a desktop lives on icon and glyph
textures.

**Cause:** not the kernel. Mesa 25.2's AFBC **repacking** (`afbc_pack`) on
Mali-G72 r0p1 writes some blocks incompletely. 16 pixels of RGBA is 64 bytes,
one cache line. Static textures get repacked; render targets never do -- which
is exactly why the benchmark was clean.

**Fix:** `PAN_MAX_AFBC_PACKING_RATIO=0` in `/etc/environment`.

`PAN_MESA_DEBUG=noafbc` also fixes the artefacts and is the **wrong** choice:

| glmark2 test (1080x1920) | AFBC on (as shipped) | `noafbc` | `packing_ratio=0` |
|---|---|---|---|
| build | 747 | 839 | 919 |
| texture | 988 | 1105 | 1139 |
| shading | 911 | 1068 | 1093 |
| bump | 1344 | 809 | 1764 |
| **desktop** | **217** | **18** | **218** |
| **Score** | **840** | 766 | **1025** |

`noafbc` costs the desktop test a factor of twelve. `packing_ratio=0` keeps
the full performance and removes the artefacts.

### How that was found, and what it rules out

Reusable method:

1. **Read the real screen out of `/dev/mem`.** The live scanout address is in
   DECON's IDMA register at `0x16070000 + 0x1000*(1+ch) + 0x40`. Read
   1440*2960*4 bytes from there and save a PNG -- that is exactly what the
   user sees, without having to ask them.
2. **Capture twice, seconds apart, and compare hashes.** Bit-identical means a
   frozen texture, not a per-frame drawing error.
3. **Force a full rebuild** by switching the display off and on. If the damage
   survives, it is in the texture, not in the compositing.
4. **Read the structure.** 1-pixel-high runs of 16 pixels are linear 64-byte
   losses. 4x4 blocks would have meant u-interleaved tiling.
5. **Only then** try the Mesa switches.

Two false leads this ruled out:

* **The CPU write path.** The suspicion was that cache lines from zeroing
  shmem were overwriting write-combined writes -- exactly what
  `should_map_wc()` warns about. A test that allocated 120 buffers through
  the panfrost ioctls, wrote a pattern, flooded the caches with 96 MB and read
  back found **0 bad bytes**.
* **Compositing.** The damage survived a full display off/on bit-identically.

### Measuring anything in the session: read this first

`/etc/environment.d/` does **not** reach `gnome-shell`; `/etc/environment`
(pam_env) does. And the `systemd --user` manager keeps its environment across
`systemctl restart gdm` -- a removed value survived three measurements that
way. Always:

    loginctl terminate-user ubuntu
    systemctl restart gdm

## 5. PLL relock -- every frequency change stalled the cluster for 3.3 ms

**Symptom:** `pll_lock_timeouts` grew with every frequency change: 3348 of
3541 transitions in the first three minutes after boot.

**Cause:** a transition parks the whole cluster on the 26 MHz OSC while the
PLL relocks, then waited up to 3 ms (a sleeping poll) for `PLL_CON0.STABLE`
plus 300 us when it did not come -- 3.3 ms of a cluster at 26 MHz, on almost
every change. With schedutil that added up to seconds per minute.

**Fix:** an atomic poll of 100 us (eight times the programmed lock time,
`LOCKTIME = 150 * P` OSC cycles, about 12 us), then a 50 us margin. STABLE
now arrives in time on 98 % of the transitions; the schedutil rate limit
(`transition_delay_us`) went from 20 to 5 ms.

| 300 bursts of 4 ms work, 30 ms rests | before | after |
|---|---|---|
| PLL timeouts / transitions | 722 / ~760 | 14 / 807 |
| M3 core: worst burst | 6.93 ms | 3.88 ms |

### Boost, the busy-core rule and the thermal ceiling (2026-10-17)

Measured with `s9p-cpu-validate` (extras/debug-tools: `stress-ng --verify`,
600 s per run, clocks and TMU temperatures every 2 s):

| run | errors | M3 clock most of the time | M3 mean temp | bogo ops |
|---|---|---|---|---|
| one M3 core, boost on | 0 | 1794 MHz (94 %) | 72 C | 67 267 |
| four A55 cores at 1794 MHz / 1000 mV | 0 | -- | 66 C | 179 706 |
| all eight + memory worker | 0 | 1066 MHz | 85 C | 272 014 |
| frequency torture, ~22k transitions, 400 MHz under load | 0 | mixed | 86 C | -- |
| four M3 cores, 3-4-busy cap raised to 2327 MHz | 0 | 1066-1469 MHz | 82 C | 177 174 |

**Thermal levels at the stock control temperatures** (M3 83 C, 5 C of
hysteresis) did the same work to within 1.5 % and ran 7-9 C hotter: the
sustained ceiling is the heat the phone sheds, so the cooler levels stay.

**Boost stays off.** A three-second burst from 56 C runs at 1794 MHz with
boost on or off. The busy-core rule counts `!idle_cpu()` at the instant of
the poll, and on a running desktop some other M3 core nearly always is
(a kworker, the compositor), so the 3-4-busy cap of 1794 MHz applies.
Samsung only runs 2704 MHz with the other M3 cores *offline* (the hotplug
governor's single mode); with all four online a wake-up can always land on
them, and four cores at 2704 MHz is the measured hard reset. Since kernel #14
the rule is re-checked every 10 ms while the cluster is above 2327 MHz, and
`s9p_cpufreq.cap_many_busy` (default 1794000) can raise the 3-4-busy cap to
2327000 for testing: no error in 30 minutes, but the thermal levels kept the
cluster below 2327 MHz almost all of that time, which is not a validation.

**Battery guard.** `s9p_cpufreq.temp_batt` is the fuel gauge's temperature;
at 43 C both clusters are held to 1499/1794 MHz, at 46 C to 1150/1066 MHz,
released 2 C lower -- for a phone that lives on the charger.

## 6. Idle power -- measured at the battery

**Method.** With the phone on USB, the MAX77705's `CHG_CNFG_00` set to 0
(charger and input buck off) makes the system run from the battery while the
cable stays in; the fuel gauge's `current_now` is then the whole phone. 40-60 s
averages, idle GNOME desktop, no monitor, Wi-Fi up; `CNFG_00` goes back to
`0x05` on exit. The averages scatter by about +-60 mW between runs, so every
number below is a pair taken back to back.

| idle desktop | battery |
|---|---|
| as booted (WFI only, memory at 1794 MHz, every PMU domain on) | 1.79-1.93 W |
| CPU clusters held at their lowest steps | -100 mW |
| Wi-Fi power save on | -160 mW (off by default: see known issues) |
| unused PMU domains off (`s9p-pd-off`) | -90 mW |
| memory at 421 MHz instead of 1794 MHz | -165 mW |
| memory governor (`s9p-mif`) instead of the fixed 1794 MHz | -150 mW |

**The memory clock.** sboot leaves the MIF at 1794 MHz and the port never
touched it. The vendor kernel scales it through the APM firmware with
`exynos_acpm_set_rate(dvfs_mif, kHz)`; its acpm_dvfs device is the CMU node,
which has no `acpm-ipc-channel`, so the channel number stays 0 and the request
goes out on ACPM **channel 0** -- not channel 5, the one the DT names for
`acpm_dvfs`, which never answers. On channel 0 the APM switches the memory
itself (PLL_MIF `0x1b800120`, or the shared PLLs below 1014 MHz). 3917 random
level changes under three memory-copy jobs and a CPU job: no wrong digest.
At 421 MHz a single memory-copy thread runs 38 % slower, so the governor
follows CPU busy time (not the CPU clock, which schedutil lifts for a few ms
whenever anything runs), the GPU clock and the USB-C monitor.

**The power domains.** All fifteen are on after sboot; the vendor kernel turns
off DCRD/DCPOST/DCF in `pmucal_lpm_init` and the rest through runtime PM.
`s9p-pd-off` does the same for the eleven no driver of the port uses, with the
TrustZone protection save (`SMC 0x82000410`) for those that have `need_smc`.
MFC refuses a second power-down after being switched back on (STATUS stays
`0x0015000f`); the module then puts it back on cleanly.

**What is left.** About 1.6 W. The CPUs never power down (no cpuidle state,
hotplug does not reach the PMU: CPU_OFF leaves the core's STATUS at `0xf`),
and that is the next target. Wi-Fi power save is worth another 160 mW.

## Where it stands

| | before | after |
|---|---|---|
| `gnome-shell` CPU at an idle desktop | 12.4 % | 1.2 % |
| idle temperature | 52 C | 43-46 C |
| glmark2 score | 840 | 1025 |

## Still open: screen transfer at 1080p

Mirroring the phone's screen to a PC through a GNOME screen-cast is bound by
`gnome-shell`, not by the link:

| capture resolution | frames/s |
|---|---|
| 1920x1080 | 9 |
| 1280x720 | 19 |
| 960x540 | 31-41 |

Exactly linear in pixel count, with `gnome-shell` at 91.7 % CPU. A gdb profile
points at `clutter_stage_paint_to_buffer` then
`cogl_framebuffer_read_pixels_into_bitmap`.

Mutter does advertise DMABuf (`Video:modifier` shows up in
`pw-cli enum-params`, and `libgstpipewire.so` contains `memory:DMABuf`), but a
pipeline override asking for `video/x-raw(memory:DMABuf)` still negotiated
system memory -- and the extra render pass Mutter does for a *virtual*
monitor, not the copy, is what dominates. Unresolved.
