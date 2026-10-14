# audio

Built-in speaker and headphone jack through Samsung's ABOX audio DSP.

All of the phone's audio goes through ABOX, a Cortex-A7 DSP whose DMA engines
only its firmware drives. This boots the stock firmware on mainline Linux and
adds an ALSA card with two outputs: **Speaker** (two MAX98512 amplifiers) and
**Headphones** (Cirrus Logic CS47L93 codec). PipeWire shows both.

- The firmware is never in this repository: `post-install` copies it from the
  phone's own Android `VENDOR` partition (`calliope_*.bin` and friends).
- Speaker volume is capped on purpose: stock Android protects the speaker with
  firmware DSP processing (Maxim DSM) that is not used here.
- No jack detection yet: pick the output in Settings → Sound. No microphones yet.
- ABOX boots once per phone boot; reloading its module needs a reboot.
- Needs the modules from `scripts/build/build_extras.sh` in `dist/extras/`.

```bash
sudo extras/install.sh audio
```

How it works: [docs/09-audio.md](../../docs/09-audio.md).
