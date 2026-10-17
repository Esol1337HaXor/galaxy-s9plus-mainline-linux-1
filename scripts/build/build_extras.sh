#!/bin/bash
# Build the optional modules of extras/usb-c-display, extras/audio,
# extras/notification-led, extras/bluetooth, extras/vibration, extras/microsd, extras/low-power
# extras/uinput and extras/flashlight
# for the kernel that
# build_stable.sh produced.
#
#   sudo scripts/build/build_extras.sh          -> dist/extras/<kernel release>/
#
# Needs a finished stable build ($BUILD/src/mainline/out). The kernel image is
# not touched: ALSA, ASoC, HID, the Cirrus madera drivers and the Bluetooth
# stack are built as modules from a copy of its configuration
# (src/*/extras.config), then this port's own out-of-tree modules against both.
set -euo pipefail
PROJ=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
. "$PROJ/scripts/build/env.sh"
mount_build
SRC="$BUILD/src/mainline"
WORK="$BUILD/extras"
export ARCH=arm64 CROSS_COMPILE=${CROSS_COMPILE:-aarch64-linux-gnu-}
K="make -s -j$(nproc)"

[ -f "$SRC/out/Module.symvers" ] || { echo "no stable build in $SRC/out: run build_stable.sh first" >&2; exit 1; }
cd "$SRC"
KREL=$(cat out/include/config/kernel.release)
echo "=== kernel $KREL ==="

# SND_SOC_CS47L92 has no prompt upstream (only machine drivers select it)
sed -i '/^config SND_SOC_CS47L92$/{n;s/^\ttristate$/\ttristate "Cirrus Logic CS47L92\/93 (s9p)"/}' \
	sound/soc/codecs/Kconfig

# All =m modules of the stable configuration: their symbols must be in
# out/Module.symvers before anything is built against it, and a few of them
# (DP helpers, CEC, multicolor LEDs, force feedback) are installed with the extras.
echo "=== modules of the stable kernel ==="
$K O=out modules

echo "=== extras configuration (out-extras) ==="
mkdir -p out-extras
cp out/.config out-extras/.config
cp out/Module.symvers out-extras/
cat "$PROJ"/src/*/extras.config | grep -E '^CONFIG_' | while IFS='=' read -r k v; do
	case $v in
	y) ./scripts/config --file out-extras/.config -e "${k#CONFIG_}" ;;
	m) ./scripts/config --file out-extras/.config -m "${k#CONFIG_}" ;;
	esac
done
$K O=out-extras olddefconfig
# guard: only sound/HID/madera/Bluetooth may differ from the running kernel
if diff <(grep '^CONFIG_' out/.config | sort) <(grep '^CONFIG_' out-extras/.config | sort) |
	grep '^[<>]' | grep -vE 'SND|SOUND|HID|MADERA|CS47L|ARIZONA|CS_DSP|WM_ADSP|AC97|CONFIG_BT' ; then
	echo "the extras configuration changes more than sound/HID/Bluetooth: refusing" >&2
	exit 1
fi
$K O=out-extras modules_prepare

echo "=== HID, ALSA/ASoC and madera as out-of-tree builds ==="
rm -rf "$WORK"; mkdir -p "$WORK/madera/mfd"
cp -r drivers/hid "$WORK/hid"
cp -r sound "$WORK/sound"
M=$WORK/madera
cp drivers/mfd/madera-core.c drivers/mfd/cs47l92-tables.c drivers/mfd/madera-spi.c drivers/mfd/madera.h \
   drivers/pinctrl/cirrus/pinctrl-madera-core.c drivers/pinctrl/cirrus/pinctrl-cs47l92.c \
   drivers/pinctrl/cirrus/pinctrl-madera.h drivers/gpio/gpio-madera.c drivers/irqchip/irq-madera.c \
   drivers/regulator/arizona-ldo1.c drivers/regulator/arizona-micsupp.c \
   drivers/firmware/cirrus/cs_dsp.c drivers/firmware/cirrus/cs_dsp.h "$M/mfd/"
cp drivers/pinctrl/pinctrl-utils.h drivers/pinctrl/core.h drivers/pinctrl/pinmux.h "$M/"
cat > "$M/mfd/Makefile" <<'MK'
madera-objs := madera-core.o cs47l92-tables.o
pinctrl-madera-objs := pinctrl-madera-core.o pinctrl-cs47l92.o
obj-m += madera.o madera-spi.o pinctrl-madera.o gpio-madera.o irq-madera.o arizona-ldo1.o arizona-micsupp.o cs_dsp.o
MK
echo 'obj-m += mfd/' > "$M/Makefile"
$K O=out-extras M="$WORK/hid" modules
# sound and madera need each other's symbols (wm_adsp <-> cs_dsp, micsupp <-> ASoC)
$K O=out-extras M="$WORK/sound" KBUILD_MODPOST_WARN=1 modules >/dev/null 2>&1 || true
$K O=out-extras M="$M" KBUILD_EXTRA_SYMBOLS="$WORK/sound/Module.symvers" modules
$K O=out-extras M="$WORK/sound" KBUILD_EXTRA_SYMBOLS="$M/Module.symvers" modules

echo "=== Bluetooth: net/bluetooth, hci_uart (BCM), btbcm ==="
cp -r net/bluetooth "$WORK/bt-net"
cp -r drivers/bluetooth "$WORK/bt-drv"
patch -s -p3 -d "$WORK/bt-drv" < "$PROJ/src/bluetooth/btbcm-bcm4361.patch"
# hidp needs HID
$K O=out-extras M="$WORK/bt-net" KBUILD_EXTRA_SYMBOLS="$WORK/hid/Module.symvers" modules
$K O=out-extras M="$WORK/bt-drv" KBUILD_EXTRA_SYMBOLS="$WORK/bt-net/Module.symvers" modules

echo "=== this port's modules ==="
for d in usbc-dp sound bluetooth; do
	rm -rf "$WORK/$d-src"; cp -r "$PROJ/src/$d" "$WORK/$d-src"
	$K O=out M="$WORK/$d-src" modules
	cp "$WORK/$d-src/Module.symvers" "$WORK/$d-base.symvers"
	$K O=out-extras M="$WORK/$d-src" CONFIG_SND=m CONFIG_SND_SOC=m \
		KBUILD_EXTRA_SYMBOLS="$WORK/sound/Module.symvers $WORK/$d-base.symvers" modules
done

echo "=== exFAT (microSD cards above 32 GB) ==="
rm -rf "$WORK/exfat"; cp -r fs/exfat "$WORK/exfat"
echo 'ccflags-y += -DCONFIG_EXFAT_DEFAULT_IOCHARSET=\"utf8\"' >> "$WORK/exfat/Makefile"
$K O=out M="$WORK/exfat" CONFIG_EXFAT_FS=m modules

echo "=== uinput (/dev/uinput: no CONFIG_INPUT_MISC in the stable kernel) ==="
rm -rf "$WORK/uinput"; mkdir -p "$WORK/uinput"
cp drivers/input/misc/uinput.c drivers/input/input-compat.h "$WORK/uinput/"
sed -i 's|"../input-compat.h"|"input-compat.h"|' "$WORK/uinput/uinput.c"
echo 'obj-m += uinput.o' > "$WORK/uinput/Makefile"
$K O=out M="$WORK/uinput" CONFIG_INPUT_UINPUT=m modules

for d in leds haptic sdcard power modem flash; do
	echo "=== src/$d ==="
	rm -rf "$WORK/$d-src"; cp -r "$PROJ/src/$d" "$WORK/$d-src"
	$K O=out-extras M="$WORK/$d-src" modules
done

echo "=== src/modem/userspace: SELinux shim for the modem's Android container ==="
# bionic, Android 10 (API 29); no libc at link time: strdup and strlen are
# resolved from the process's own libc when the linker preloads it
command -v clang >/dev/null || { echo "clang and lld are needed for the modem shim (apt install clang lld)" >&2; exit 1; }
clang --target=aarch64-linux-android29 -O2 -fPIC -shared -nostdlib -fuse-ld=lld -Wall \
	-Wl,--hash-style=both -Wl,-soname,s9p-noselinux.so \
	-o "$WORK/s9p-noselinux.so" "$PROJ/src/modem/userspace/s9p-noselinux.c"

OUT="$PROJ/dist/extras/$KREL"
rm -rf "$OUT"; mkdir -p "$OUT"/{kernel,usbc-dp,sound,hid,alsa,madera,leds,bluetooth,haptic,sdcard,power,modem,input,flash}
# =m in the stable kernel, needed by the extras (the image installs no modules)
cp out/drivers/gpu/drm/display/drm_display_helper.ko out/drivers/media/cec/core/cec.ko \
   out/drivers/leds/led-class-multicolor.ko out/drivers/input/ff-memless.ko \
   out/crypto/kpp.ko out/crypto/ecc.ko out/crypto/ecdh_generic.ko out/crypto/aes.ko out/crypto/cmac.ko \
   "$OUT/kernel/"
cp "$WORK"/usbc-dp-src/*.ko "$OUT/usbc-dp/"
cp "$WORK"/sound-src/*.ko "$OUT/sound/"
find "$WORK/hid" -name '*.ko' -exec cp {} "$OUT/hid/" \;
find "$WORK/sound" -name '*.ko' -exec cp {} "$OUT/alsa/" \;
find "$M" -name '*.ko' -exec cp {} "$OUT/madera/" \;
cp "$WORK"/leds-src/*.ko "$OUT/leds/"
cp "$WORK"/haptic-src/*.ko "$OUT/haptic/"
cp "$WORK"/uinput/uinput.ko "$OUT/input/"
cp "$WORK"/flash-src/*.ko "$OUT/flash/"
cp "$WORK"/sdcard-src/*.ko "$WORK"/exfat/exfat.ko "$OUT/sdcard/"
cp "$WORK"/power-src/*.ko "$OUT/power/"
cp "$WORK"/modem-src/*.ko "$WORK/s9p-noselinux.so" "$OUT/modem/"
echo "=== modem userspace: s9p-mobile, s9p-radio (libgbinder) ==="
bash "$PROJ/scripts/build/build_modem_userspace.sh" "$BUILD/modem-user" "$OUT/modem"
cp "$WORK"/bluetooth-src/*.ko "$OUT/bluetooth/"
find "$WORK/bt-net" "$WORK/bt-drv" -name '*.ko' -exec cp {} "$OUT/bluetooth/" \;
"${CROSS_COMPILE}strip" --strip-debug "$OUT"/*/*.ko
(cd "$OUT" && sha256sum */*.ko modem/*.so modem/s9p-mobile modem/s9p-radio > SHA256SUMS)
echo "=== done: $(ls "$OUT"/*/*.ko | wc -l) modules in dist/extras/$KREL ==="
echo "copy the repository (with dist/) to the phone, then: sudo extras/install.sh usb-c-display / audio / notification-led / bluetooth / vibration / microsd / low-power / modem"
