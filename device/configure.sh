#!/bin/bash
# Galaxy S9+: hardware configuration, run inside the image during the build
# (image/build-image.sh). Only what this phone needs to work; the optional
# features are in extras/.
set -euo pipefail

chmod 600 /etc/NetworkManager/system-connections/usb0.nmconnection

# USB serial console and network, touch/charger power fix, the stability
# defaults, Wi-Fi reconnect after a failed rekey. hciattach-bcm4361.service
# stays off: the kernel is built without Bluetooth (build71.sh turns BT and
# RFKILL off), so it could only fail and restart every 3 s.
systemctl enable usb-gadget.service s9p-touch-power.service s9p-stability.service \
	s9p-wifi-guard.service serial-getty@ttyGS0.service s9p-boot-ok.timer

# The common image sets up zram swap, but this kernel has no zram (and the
# image carries no modules): zram-generator would create dev-zram0.swap for
# a device that never appears, and every boot would wait 90 s for it, then
# report failed units. An empty generator config switches it off; 6 GB of
# RAM run the desktop without swap. The common zram tuning goes too: on a
# swap file on flash it would push memory out to UFS early.
ln -sf /dev/null /etc/systemd/zram-generator.conf
rm -f /etc/sysctl.d/90-zram.conf

# Suspend never resumes on this port (the M3 cores do not come back).
systemctl mask sleep.target suspend.target hibernate.target hybrid-sleep.target \
	suspend-then-hibernate.target

# Mesa's AFBC repacking corrupts textures on this Mali-G72 (16-pixel dashes
# in icons and text). /etc/environment is what reaches gnome-shell.
grep -q '^PAN_MAX_AFBC_PACKING_RATIO=' /etc/environment ||
	echo 'PAN_MAX_AFBC_PACKING_RATIO=0' >> /etc/environment

cat > /etc/fstab <<'FSTAB'
# The initramfs mounts USERDATA (/dev/sda25) as the root filesystem.
LABEL=s9plus-root  /  ext4  defaults,noatime  0 0
FSTAB

dconf update
