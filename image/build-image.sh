#!/bin/bash
# Build the Ubuntu 24.04 root filesystem for the Galaxy S9+ from scratch.
#
#   sudo image/build-image.sh
#
# Output in dist/image/: s9plus-rootfs.img (a raw ext4 image for USERDATA)
# and SHA256SUMS. The kernel is a separate boot image (docs/02-building.md);
# it has every driver built in, so the root filesystem carries no modules.
#
# Settings (user, password, time zone, keyboard, SSH key) are described in
# image/common/lib.sh. The hardware features of extras/defaults are
# installed on the phone's first boot; the rest of extras/ stays optional.
set -euo pipefail
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$REPO/image/common/lib.sh"
WORK=${WORK:-/var/tmp/s9plus-image}
OUT=${OUT:-$REPO/dist/image}

# Galaxy Companion, the port's settings app (github.com/agcarbajo/galaxy-companion)
GC_VERSION=2.3.0
GC_SHA256=776e6f4d15b8ce30d28f10c018d8046a194cd8b533c5b3e2106e8447b388805d

# The hardware works out of the box: extras/defaults are installed on the
# phone's first boot (s9p-hardware-setup.service, device/base), so the image
# carries the extras, the modules built for them (dist/extras, from
# scripts/build/build_extras.sh) and their packages, plus Galaxy Companion.
port_hardware() {
	local R=$1 P=$1/usr/local/share/s9p-port pkgs f deb
	log "hardware features and Galaxy Companion"
	rm -rf "$P"; mkdir -p "$P/dist"
	cp -a "$REPO/extras" "$P/"
	if compgen -G "$REPO/dist/extras/*/SHA256SUMS" >/dev/null; then
		cp -a "$REPO/dist/extras" "$P/dist/"
	else
		echo "WARNING: no dist/extras (scripts/build/build_extras.sh): the image" \
			"has no modules for its hardware features" >&2
	fi
	pkgs=$(for f in $(sed 's/#.*//' "$REPO/extras/defaults"); do
		sed 's/#.*//' "$REPO/extras/$f/packages" 2>/dev/null; done | tr -s ' \t\n' ' ')
	[ -z "${pkgs// /}" ] || in_chroot "$R" apt-get -y --no-install-recommends install $pkgs
	deb=galaxy-companion_${GC_VERSION}_all.deb
	curl -fsSL -o "$R/tmp/$deb" \
		"https://github.com/agcarbajo/galaxy-companion/releases/download/v$GC_VERSION/$deb"
	echo "$GC_SHA256  $R/tmp/$deb" | sha256sum -c --quiet - || die "$deb: checksum mismatch"
	in_chroot "$R" apt-get -y --no-install-recommends install "/tmp/$deb"
	rm -f "$R/tmp/$deb"
}

require_host
image_settings s9plus
R=$WORK/rootfs
trap chroot_umount EXIT

rootfs_bootstrap "$R"
chroot_mount "$R"
rootfs_install "$R" "$REPO/device/packages.txt"
rootfs_configure "$R"
rootfs_device "$R" "$REPO/device/base" "$REPO/device/configure.sh"
port_hardware "$R"
rootfs_finish "$R"
chroot_umount

mkdir -p "$OUT"
rm -f "$OUT/s9plus-rootfs.img" "$OUT/SHA256SUMS"
make_ext4 "$R" "$OUT/s9plus-rootfs.img" s9plus-root
checksums "$OUT"
log "done: flash $OUT/s9plus-rootfs.img to USERDATA (docs/03-installing.md)"
