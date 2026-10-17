#!/bin/bash
# One-time: the cross toolchain and host tools (Ubuntu 24.04 host), then the
# static arm64 busybox for the initramfs. Nothing else is downloaded: the
# boot image and the DTBH table are written by mkboot.py and mkdtbh.py.
set -e
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
  build-essential bc bison flex libssl-dev libelf-dev libncurses-dev \
  gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu \
  clang lld llvm \
  device-tree-compiler python3 python3-pip git wget curl ca-certificates \
  debootstrap qemu-user-static binfmt-support \
  cpio gzip lz4 zstd xz-utils rsync kmod fakeroot \
  e2fsprogs dosfstools android-sdk-libsparse-utils 2>&1 | tail -5
# arm64 GLib for the modem's userspace (libgbinder, s9p-mobile), cross-built
# by build_extras.sh: the host's archive serves amd64 only, ports.ubuntu.com
# serves arm64
if [ "$(dpkg --print-architecture)" != arm64 ] &&
   ! dpkg --print-foreign-architectures | grep -qx arm64; then
  dpkg --add-architecture arm64
  sed -i '/^Types: deb/,/^$/{/^Architectures:/d;s/^\(URIs:.*\)$/\1\nArchitectures: amd64/}' \
    /etc/apt/sources.list.d/ubuntu.sources 2>/dev/null || true
  . /etc/os-release
  cat > /etc/apt/sources.list.d/ubuntu-ports-arm64.sources <<EOF2
Types: deb
URIs: http://ports.ubuntu.com/ubuntu-ports
Suites: $VERSION_CODENAME $VERSION_CODENAME-updates $VERSION_CODENAME-security
Components: main universe
Architectures: arm64
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
EOF2
  apt-get update -qq
fi
apt-get install -y -qq --no-install-recommends libglib2.0-dev:arm64 pkgconf 2>&1 | tail -2
echo "--- versions ---"
aarch64-linux-gnu-gcc --version | head -1
clang --version | head -1
dtc --version
debootstrap --version
echo "--- busybox for the initramfs ---"
mount_build
if [ ! -x "$BUILD/out/busybox-arm64" ]; then
  python3 "$(dirname "${BASH_SOURCE[0]}")/fetch_busybox.py" "$BUILD/out/busybox-arm64"
fi
echo TOOLCHAIN_OK
