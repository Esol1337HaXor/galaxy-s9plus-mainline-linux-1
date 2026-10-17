#!/bin/bash
# The modem's host userspace: libglibutil, libgbinder and libgbinder-radio
# (Jolla/mer-hybris, BSD) at fixed versions, linked statically into
# s9p-mobile (mobile data, extras/modem) and s9p-radio (test client).
#   build_modem_userspace.sh <work dir> <output dir>
# Native on arm64; elsewhere CROSS_COMPILE=aarch64-linux-gnu- with the arm64
# GLib from toolchain.sh.
set -euo pipefail
PROJ=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
W=$1 OUT=$2
SRC=$PROJ/src/modem/userspace
GLIBUTIL=fa82adf84ba308626c4389c6e5b95bb4c12ce1d6	# 1.0.83
GBINDER=770f4ce2a5aca0bcaf4df2ba58e21fa30e74b1bc	# 1.1.54
GBINDER_RADIO=7ecf42f4c56e406406fb525efd6610fc1ac143b7

if [ "$(uname -m)" = aarch64 ]; then
	export CC=gcc
else
	export CC=${CROSS_COMPILE:-aarch64-linux-gnu-}gcc
	export PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig
fi
fetch() {	# repo commit dir
	[ -d "$W/$3" ] && [ "$(git -C "$W/$3" rev-parse HEAD)" = "$2" ] && return
	rm -rf "$W/$3"
	git clone -q "https://github.com/$1.git" "$W/$3"
	git -C "$W/$3" checkout -q "$2"
}
mkdir -p "$W" "$OUT"
fetch sailfishos/libglibutil $GLIBUTIL libglibutil
fetch mer-hybris/libgbinder $GBINDER libgbinder
fetch mer-hybris/libgbinder-radio $GBINDER_RADIO libgbinder-radio

# each library finds the one before through pkg-config
PC=$W/pkgconfig; mkdir -p "$PC"
pc() {	# name dir requires
	cat > "$PC/$1.pc" <<PCEOF
Name: $1
Description: $1 (s9p build)
Version: 0
Requires: $3
Cflags: -I$2/include
Libs: $2/build/release/$1.a
PCEOF
}
export PKG_CONFIG_PATH=$PC
make -s -C "$W/libglibutil" -j"$(nproc)" release
pc libglibutil "$W/libglibutil" "glib-2.0 gobject-2.0"
make -s -C "$W/libgbinder" -j"$(nproc)" release
pc libgbinder "$W/libgbinder" "libglibutil"
make -s -C "$W/libgbinder-radio" -j"$(nproc)" release
# libgbinder-radio builds a shared library only: archive its objects
AR=${CC%gcc}ar; command -v "$AR" >/dev/null || AR=ar
rm -f "$W/libgbinder-radio/build/release/libgbinder-radio.a"
$AR rcs "$W/libgbinder-radio/build/release/libgbinder-radio.a" "$W"/libgbinder-radio/build/release/*.o
pc libgbinder-radio "$W/libgbinder-radio" "libgbinder"
# the .a goes before GLib on the link line
LIBS="$W/libgbinder-radio/build/release/libgbinder-radio.a $W/libgbinder/build/release/libgbinder.a \
	$W/libglibutil/build/release/libglibutil.a $(pkg-config --libs gobject-2.0 glib-2.0) -lpthread"
CFLAGS="-O2 -Wall $(pkg-config --cflags libgbinder-radio) -I$W/libgbinder-radio/include"
$CC $CFLAGS -o "$OUT/s9p-mobile" "$SRC/s9p-mobile.c" "$SRC/s9p-ril.c" $LIBS
$CC $CFLAGS -o "$OUT/s9p-radio" "$SRC/s9p-radio.c" $LIBS
${CC%gcc}strip "$OUT/s9p-mobile" "$OUT/s9p-radio" 2>/dev/null || strip "$OUT/s9p-mobile" "$OUT/s9p-radio"
echo "modem userspace: $(cd "$OUT" && ls s9p-mobile s9p-radio | tr '\n' ' ')"
