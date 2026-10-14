# USB-C: Type-C/PD alt mode (MAX77705), USB host role, DisplayPort output and
# DisplayPort audio. Out-of-tree modules, built by scripts/build/build_extras.sh.
obj-m += s9p-usbc.o s9p-usbhost.o s9p-dp.o
s9p-dp-y := s9p-dp-drm.o s9p-dp-hw.o s9p-dp-board.o
# needs ALSA: built against the extras configuration (CONFIG_SND=m)
obj-$(CONFIG_SND) += s9p-dpaudio.o
