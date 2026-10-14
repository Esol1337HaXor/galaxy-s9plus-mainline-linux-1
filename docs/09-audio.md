# Audio: speaker and headphones

The port's kernel has no sound at all, and every audio path of this phone
goes through **ABOX**, Samsung's audio subsystem with its own Cortex-A7 DSP
("Calliope"). Its DMA engines are programmed only by that DSP's firmware, so
`src/sound/` boots the **stock firmware** on mainline and drives the rest
directly, following Samsung's 4.9 kernel (`sound/soc/samsung/abox/`).

```
ALSA card "Galaxy S9+" (s9p-abox-pcm)
 PCM 0 Headphones: RDMA0 -> SPUS IN0 -> SIFS0 (mixer) -> UAIF0 (I2S) -> CS47L93 AIF1 -> HPOUT2
 PCM 1 Speaker:    RDMA1 -> SPUS IN1 -> SIFS1         -> UAIF1 (I2S) -> MAX98512 x2 (L 0x39, R 0x3a)
   RDMA: firmware, by IPC         routing, UAIF, clocks, pins: AP registers
```

Build with `scripts/build/build_extras.sh`, install with
`sudo extras/install.sh audio`, reboot. HDMI audio is a separate, much simpler
path without ABOX: see [08-usb-c-display.md](08-usb-c-display.md).

## The modules

| module | does |
|---|---|
| `s9p-audio-board` | the codec's SPI bus (USI01) and the amplifiers' I2C bus (USI07), added with an OF changeset; pin mux; MCLK1 (26 MHz TCXO on the PMU's CLKOUT0); codec 1.2 V rail on |
| madera, cs47l92 (upstream) | the Cirrus Logic CS47L93 codec; upstream covers it with the CS47L92 driver |
| `s9p-abox` | ABOX: SysMMU, firmware load, CA7 start, IPC, I2S pins, UAIF bit clocks |
| `s9p-max98512` | the two amplifiers (revision A), own minimal driver |
| `s9p-abox-pcm` | the ASoC card: PCMs over RDMA0/1 by IPC, UAIF0/1 DAIs, codec FLL/SYSCLK |

## Codec power and buses

- The codec core (DCVDD/CPVDD2, **LDO34** of the S2MPS18 PMIC, 1.2 V) is **off**
  after sboot; without it the codec answers nothing (MISO stays high). The
  PMIC sits on the APM's SPEEDY bus; `s9p-audio-board` turns LDO34 on through
  the port's `s9p-acpm` IPC API. LDO33 (1.8 V I/O) is already on.
- The codec's SPI chip select does not work through the controller's hardware
  CS: it is a GPIO (`cs-gpios`, gpp0-7). The GPIO banks of the port's DT have
  no phandles; the board module gives them one at run time.
- Upstream madera probes it (`CS47L93 silicon revision 1`); the card sets FLL1
  from MCLK1 (26 MHz) to SYSCLK 98.304 MHz and runs AIF1 on SYSCLK.

## ABOX

### Firmware and memory

The firmware is Samsung's and is **not** in this repository: the extra copies
it from the phone's own Android `VENDOR` partition (`calliope_sram.bin`,
`calliope_dram.bin`, `calliope_iva.bin`, plus the DSM and voice parameter
blobs it expects in DRAM). Firmware version: "CRL0".

The firmware runs behind the ABOX **SysMMU** (v7.2, non-VM layout) and uses
fixed I/O addresses, so `s9p-abox` programs the SysMMU itself with a private
page table of 1 MiB sections (section format: `PA >> 4`, prot `[5:4] = 3`):

| IOVA | what |
|---|---|
| `0x80000000` | 18 MiB: `calliope_dram.bin` and the extra blobs |
| `0x90000000` | `calliope_iva.bin` |
| `0x91000000 + n MiB` | RDMAn playback buffers |
| `0xa0000000`, `0xa1000000` | VSS (voice, shared with the modem on stock): zeroed stand-ins |
| `0x10000000` | chip id, identity |

### Boot and IPC

1. SRAM firmware to `0x17d00000`, DRAM/IVA firmware to their buffers.
2. `SYSPOWER_CTRL`, then the CA7 through the PMU (`0x415c` local power,
   `0x4164` bit 28 enable, wait for `0x4160` bit 0).
3. `BOOT_DONE` arrives, the I2S pad retention is released, `ABOX_RESUME`.

IPC is a 764-byte message in SRAM (`+0x30000` to the DSP, `+0x30300` from it)
plus an SGI on ABOX's own GIC. Messages from the DSP and the per-period
`RDMAn_BUF_EMPTY` SGIs are **polled every 20 ms**: with the GIC interrupt
routed to the AP the system hung in an interrupt storm at boot.

The AP sends, per stream, what the vendor's `abox_rdma.c` sends:
`PCM_PLTDAI_OPEN`, `IPC_ABOX_CONFIG` (SIFS rate/format), `PCM_SET_BUFFER`
(IOVA, period size, periods), `PCM_PLTDAI_HW_PARAMS`, `PCM_PLTDAI_PREPARE`,
`PCM_PLTDAI_TRIGGER`. Routing (`SPUS_CTRL0/1`, `ROUTE_CTRL0`), the UAIF
format and enable, and the UAIF bit clocks (`AUDIF = PLL_AUD / 16 = 73.728
MHz`, `/48` for 1.536 MHz) are written directly. PLL_AUD is already at
1179.648 MHz from sboot.

The playback position: the vendor clears its saved pointer on every
`BUF_EMPTY` and reads `RDMA_STATUS` (`RBUF_OFFSET << 4 + RBUF_CNT`).

### I2S pins

The UAIF pins are not in the port's DT and come up as GPIO inputs: no sound
at all leaves the SoC until gpb0-1..4 (UAIF0) and gpb1-0..3 (UAIF1) are set
to function 2, after forcing the AUD pinctrl block's clock on through its
Q-channel (`CMU_AUD + 0x304c`).

## Amplifiers

Two MAX98512 **revision A** (`REV_ID 0x0402 = 0x41`) on I2C 0x39 (left) and
0x3a (right), I2S consumers on UAIF1. The left one can NACK the first access
after idling; the driver retries. Stock Android routes the speaker through
the firmware's Maxim DSM protection (and an I/V feedback loop): it is not
used here, so the speaker gain starts at +3 dB and its control stops at +9 dB.

## PipeWire

- UCM profile `s9p-audio`: devices **Speaker** (`hw:...,1`, priority 300)
  and **Headphones** (`hw:...,0`, mixer `HPOUT2 Digital`).
- The codec's DAC digital volume goes up to +31.5 dB and clips; the card caps
  it at 0 dB.
- `/etc/wireplumber/main.lua.d/51-s9p-audio.lua` turns timer-based
  scheduling off for this card (`api.alsa.disable-tsched`): the position moves
  in polled steps, and with tsched PipeWire underran constantly.

## Debugging

```bash
sudo cat /sys/kernel/debug/s9p-abox/log     # boot, IPC, clocks, pins
sudo cat /sys/kernel/debug/s9p-abox/state   # CPU, SysMMU, routing, UAIF, RDMA0/1
amixer -c S9 contents
speaker-test -D hw:S9,1 -c2 -r48000 -FS16_LE -t sine   # speaker, PipeWire stopped
```

## Known limits

- Headphone jack detection and microphones are not done yet.
- 48 kHz, S16, stereo only (PipeWire resamples).
- ABOX boots once per phone boot. After a successful boot and a suspend,
  booting it again leaves the DSP waiting in WFI: reloading `s9p-abox` needs a
  reboot.
- No speaker protection (see above): keep the volume sensible.

## Hurdles

| Symptom | Cause | Fix |
|---|---|---|
| Codec reads `0xffff` | LDO34 (codec 1.2 V) off after sboot; hardware SPI CS not reaching the pin | LDO34 on via ACPM; GPIO chip select |
| Firmware stops at its first DRAM write | SysMMU section prot `1` is read-only in the v5+ format | prot `3` (read/write) |
| Firmware faults at `0xa0670000` | it touches the voice (VSS) area at boot | map zeroed VSS buffers |
| Hard hang a few ms after the CA7 starts | interrupt storm from ABOX's GIC | poll it every 20 ms |
| Oops in `memset` after reloading the card | the RDMA buffer was put in `substream->dma_buffer`, which ALSA frees | own `snd_dma_buffer` |
| Data flows, no sound | UAIF pins left as GPIO inputs | function 2 on gpb0/gpb1 |
| Choppy playback | PipeWire timer scheduling on a stepping position | `api.alsa.disable-tsched` |
| Headphones very loud and harsh | DAC digital volume at +31.5 dB | capped at 0 dB |
