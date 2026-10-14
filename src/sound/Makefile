# Internal audio: codec/amplifier buses, ABOX DSP firmware boot and IPC, the
# ASoC card and the MAX98512 amplifiers. Built by scripts/build/build_extras.sh.
obj-m += s9p-audio-board.o s9p-abox.o
# need ASoC: built against the extras configuration (CONFIG_SND_SOC=m)
obj-$(CONFIG_SND_SOC) += s9p-abox-pcm.o s9p-max98512.o
