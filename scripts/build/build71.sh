#!/bin/bash
# End-to-end build of the mainline 7.1 kernel for the Galaxy S9+ (star2lte):
# device tree, config, initramfs, Image, Samsung boot image. One WSL round trip.
set -uo pipefail
PROJ=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
mount_build || exit 1
SRC="$BUILD/src/mainline"
DIST="${DIST:-$PROJ/dist}"
cd "$SRC" || exit 1

STAMP=$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p "$PROJ/build-logs"
LOG="$PROJ/build-logs/build71-$STAMP.log"

echo "=== device tree ==="
cp "$PROJ/src/mainline-dts/exynos9810-star2lte.dts" arch/arm64/boot/dts/exynos/
if [ -n "${NOFB:-}" ]; then
	echo "NOFB=1: dropping the bootloader framebuffer"
	sed -i 's|/\* FB-STATUS \*/|status = "disabled";|' arch/arm64/boot/dts/exynos/exynos9810-star2lte.dts
fi
if [ -n "${UFS:-}" ]; then
	echo "UFS=1: enabling the UFS node"
	sed -i 's/status = "disabled"; \/\* UFS-STATUS \*\//status = "okay";/' arch/arm64/boot/dts/exynos/exynos9810-star2lte.dts
fi
if [ -n "${GPU:-}" ]; then
	echo "GPU=1: enabling the Mali node"
	sed -i 's/status = "disabled"; \/\* GPU-STATUS \*\//status = "okay"; \/* GPU-STATUS *\//' 		arch/arm64/boot/dts/exynos/exynos9810-star2lte.dts
	grep -n 'GPU-STATUS' arch/arm64/boot/dts/exynos/exynos9810-star2lte.dts
fi
if [ -n "${DECON:-}" ]; then
	echo "DECON=1: display takeover by the DECON scanout driver"
	sed -i 's|status = "disabled"; /\* DECON-STATUS \*/|status = "okay"; /* DECON-STATUS */|' \
		arch/arm64/boot/dts/exynos/exynos9810-star2lte.dts
	# the bootloader framebuffer node stays enabled: the DECON driver evicts
	# simpledrm itself (aperture_remove_conflicting_devices), so the early
	# boot is identical to the proven bootfb builds. NOFB=1 still drops it.
	grep -n 'DECON-STATUS\|FB-STATUS\|status = "disabled"; status' \
		arch/arm64/boot/dts/exynos/exynos9810-star2lte.dts || true
fi
if ! grep -q star2lte arch/arm64/boot/dts/exynos/Makefile; then
	python3 - <<'PY'
from pathlib import Path
TAB = chr(9); BS = chr(92); NL = chr(10)
p = Path("arch/arm64/boot/dts/exynos/Makefile")
s = p.read_text()
anchor = [l for l in s.splitlines(keepends=True) if "exynos9810-starlte.dtb" in l]
assert len(anchor) == 1, "no starlte entry to anchor on"
p.write_text(s.replace(anchor[0], anchor[0] + TAB + "exynos9810-star2lte.dtb" + TAB + TAB + BS + NL))
print("dts added to Makefile")
PY
fi

# The early colour bands served their purpose; on a working device they are
# only stripes across the bottom of the boot logo.
python3 "$PROJ/scripts/build/purge_fbband.py" "$SRC" || exit 1

echo "=== out-of-tree drivers ==="
bash "$PROJ/scripts/build/apply_drivers.sh" "$SRC" || exit 1

echo "=== initramfs ==="
RAMFS="$BUILD/out/mainline-initramfs"
rm -rf "$RAMFS"
mkdir -p "$RAMFS"/bin "$RAMFS"/dev "$RAMFS"/proc "$RAMFS"/sys "$RAMFS"/run "$RAMFS"/tmp
# a static busybox: the initramfs has no C library (toolchain.sh fetches it)
if [ ! -f "$BUILD/out/busybox-arm64" ]; then
	echo "missing $BUILD/out/busybox-arm64: run scripts/build/toolchain.sh," \
		"or python3 scripts/build/fetch_busybox.py $BUILD/out/busybox-arm64"
	exit 1
fi
if readelf -l "$BUILD/out/busybox-arm64" | grep -q 'program interpreter'; then
	echo "$BUILD/out/busybox-arm64 is dynamically linked; replace it with a static one:" \
		"python3 scripts/build/fetch_busybox.py $BUILD/out/busybox-arm64"
	exit 1
fi
install -m755 "$BUILD/out/busybox-arm64" "$RAMFS/bin/busybox"
# the applet list comes from running busybox: natively on an arm64 build host,
# through qemu-user elsewhere
if [ "$(uname -m)" = aarch64 ]; then
	applets=$("$RAMFS/bin/busybox" --list)
else
	applets=$(qemu-aarch64-static "$RAMFS/bin/busybox" --list)
fi
for applet in $applets; do
	case "$applet" in busybox) continue ;; esac
	ln -sf busybox "$RAMFS/bin/$applet"
done
# /init is a shell script: without /bin/sh the kernel panics with
# "No working init found"
[ -e "$RAMFS/bin/sh" ] || { echo "initramfs has no /bin/sh (busybox --list failed)"; exit 1; }
# INIT=boot builds the image that hands over to the Ubuntu install; the
# default builds the probe image that reports and then panics on purpose.
if [ "${INIT:-probe}" = boot ]; then
	install -m755 "$PROJ/device/mainline-init-boot" "$RAMFS/init"
	echo "init: hand over to Ubuntu on USERDATA"
else
	install -m755 "$PROJ/device/mainline-init" "$RAMFS/init"
	echo "init: probe and panic"
fi
sed -i 's/\r$//' "$RAMFS/init"
if [ -f "$PROJ/device/mainline-extra-probe" ]; then
	install -m755 "$PROJ/device/mainline-extra-probe" "$RAMFS/extra-probe"
	sed -i 's/\r$//' "$RAMFS/extra-probe"
fi
mknod -m600 "$RAMFS/dev/console" c 5 1 2>/dev/null
mknod -m666 "$RAMFS/dev/null" c 1 3 2>/dev/null
mknod -m600 "$RAMFS/dev/kmsg" c 1 11 2>/dev/null
mknod -m600 "$RAMFS/dev/mem" c 1 1 2>/dev/null

echo "=== config ==="
export LC_ALL=C ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
if [ ! -f out/.config ] || [ -n "${FRESH_CONFIG:-}" ]; then
	make -s O=out defconfig
fi
cfg() { ./scripts/config --file out/.config "$@"; }

# arm64 defconfig is a distro kernel for four dozen SoC families; the Image it
# produces is 54 MB. sboot loads the kernel into a fixed window, so everything
# that is not this phone comes back out first.
for sym in ARCH_ACTIONS ARCH_AIROHA ARCH_SUNXI ARCH_ALPINE ARCH_APPLE ARCH_BCM 	ARCH_BCM2835 ARCH_BCMBCA ARCH_BRCMSTB ARCH_BERLIN ARCH_BLAIZE ARCH_EXYNOSAUTOV9 	ARCH_SPARX5 ARCH_K3 ARCH_LG1K ARCH_HISI ARCH_KEEMBAY ARCH_MEDIATEK ARCH_MESON 	ARCH_MVEBU ARCH_MXC ARCH_LAYERSCAPE ARCH_MA35 ARCH_NPCM ARCH_NXP ARCH_QCOM 	ARCH_REALTEK ARCH_RENESAS ARCH_ROCKCHIP ARCH_S32 ARCH_SEATTLE ARCH_INTEL_SOCFPGA 	ARCH_STM32 ARCH_SYNQUACER ARCH_TEGRA ARCH_TESLA_FSD ARCH_SPRD ARCH_THUNDER 	ARCH_THUNDER2 ARCH_UNIPHIER ARCH_VEXPRESS ARCH_VISCONTI ARCH_XGENE ARCH_ZYNQMP 	ARCH_SOPHGO ARCH_STM32MP ARCH_MMP ARCH_WPCM450 ARCH_VT8500; do
	cfg --disable "$sym"
done
for sym in SOUND SND MEDIA_SUPPORT PCI PCIE_DW ACPI EFI NUMA XEN KVM VIRTUALIZATION 	VIRTIO_MENU INFINIBAND RDMA_RXE MTD ATA MD NVME_CORE SCSI_LOWLEVEL 	ETHERNET WLAN WWAN USB_NET_DRIVERS PHYLIB NET_VENDOR_SAMSUNG 	INPUT_JOYSTICK INPUT_TABLET INPUT_MISC 	HID_SUPPORT IIO HWMON W1 FUSION FIREWIRE MACINTOSH_DRIVERS 	BT CAN NFC WIRELESS 	BTRFS_FS XFS_FS F2FS_FS GFS2_FS OCFS2_FS JFS_FS NTFS3_FS EROFS_FS SQUASHFS 	NFS_FS NFSD CIFS CEPH_FS 9P_FS CODA_FS AFS_FS ORANGEFS_FS 	DRM_AMDGPU DRM_NOUVEAU DRM_I915 DRM_RADEON DRM_MSM DRM_ETNAVIV DRM_ROCKCHIP 	DRM_MEDIATEK DRM_TEGRA DRM_VC4 DRM_SUN4I DRM_ARM DRM_KOMEDA DRM_HDLCD DRM_MALI_DISPLAY 	DRM_PL111 DRM_RCAR_DU DRM_IMX DRM_MXSFB DRM_MESON DRM_TIDSS DRM_VIRTIO_GPU 	DRM_LIMA DRM_V3D DRM_EXYNOS DRM_TVE200 DRM_LOGICVC DRM_SSD130X 	DRM_PANEL_BRIDGE DRM_DISPLAY_DP_HELPER DRM_DP_AUX_CHARDEV 	STAGING COMEDI GREYBUS FPGA MAILBOX_TEST TEE OPTEE 	CPU_FREQ SERIAL_8250 I2C_MUX SPI_MEM MTD_SPI_NOR 	COMPAT DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT FTRACE STACK_TRACER 	SECURITY_SELINUX SECURITY_APPARMOR SECURITY_SMACK SECURITY_TOMOYO 	NET_SCHED NET_CLS_ACT IP_VS NETFILTER BRIDGE VLAN_8021Q; do
	cfg --disable "$sym"
done
# Drop what the strip loop hid. PCI, WLAN and CPU_FREQ come back on below,
# and without this pass defconfig's children of theirs come back with them
# (PCIe port services/AER/PME, cpufreq-dt, SCMI, iwlwifi, ath1xk): a fresh
# tree then builds a different kernel than one whose .config went through a
# build before.
make -s O=out olddefconfig

# rfkill: GNOME shows Bluetooth (extras/bluetooth) only through it. Built in:
# cfg80211 is built in and depends on "RFKILL || !RFKILL".
cfg --enable RFKILL

# Containers and a wired network, so the phone can run Docker / Podman and
# Tailscale as a small server. Everything built in: the root filesystem
# carries no kernel modules (extras ship their own), so "=m" here would be
# as good as off -- which is what OVERLAY_FS and VETH were until now.
# Checked against moby's contrib/check-config.sh: every "Generally
# Necessary" item plus bridge/overlay/ipvlan/macvlan networks, IPVS (swarm
# service load balancing), the nftables backend Ubuntu's iptables uses, and
# the cgroup controllers for CPU quotas and block I/O limits.
for sym in NETFILTER NETFILTER_ADVANCED NETFILTER_INGRESS NETFILTER_NETLINK \
	NF_CONNTRACK NF_CONNTRACK_MARK NF_CONNTRACK_FTP NF_CONNTRACK_TFTP \
	NF_CT_NETLINK NF_NAT NF_NAT_FTP NF_NAT_TFTP NF_NAT_MASQUERADE NF_NAT_REDIRECT \
	NF_TABLES NF_TABLES_INET NF_TABLES_NETDEV NF_TABLES_IPV4 NF_TABLES_IPV6 \
	NF_TABLES_BRIDGE NFT_CT NFT_NAT NFT_MASQ NFT_REDIR NFT_REJECT NFT_COMPAT \
	NFT_LIMIT NFT_LOG NFT_COUNTER NFT_FIB_IPV4 NFT_FIB_IPV6 NFT_FIB_INET \
	NETFILTER_XTABLES NETFILTER_XTABLES_LEGACY NETFILTER_XT_MARK NETFILTER_XT_CONNMARK \
	NETFILTER_XT_NAT NETFILTER_XT_TARGET_MASQUERADE NETFILTER_XT_TARGET_REDIRECT \
	NETFILTER_XT_TARGET_CHECKSUM NETFILTER_XT_TARGET_LOG NETFILTER_XT_TARGET_TCPMSS \
	NETFILTER_XT_MATCH_ADDRTYPE NETFILTER_XT_MATCH_BPF NETFILTER_XT_MATCH_COMMENT \
	NETFILTER_XT_MATCH_CONNTRACK NETFILTER_XT_MATCH_IPVS NETFILTER_XT_MATCH_MARK \
	NETFILTER_XT_MATCH_MULTIPORT NETFILTER_XT_MATCH_STATE NETFILTER_XT_MATCH_LIMIT \
	NETFILTER_XT_MATCH_PKTTYPE NETFILTER_XT_MATCH_PHYSDEV \
	NF_DEFRAG_IPV4 NF_DEFRAG_IPV6 NF_REJECT_IPV4 NF_REJECT_IPV6 \
	IP_NF_IPTABLES IP_NF_IPTABLES_LEGACY IP_NF_FILTER IP_NF_MANGLE IP_NF_RAW IP_NF_NAT \
	IP_NF_TARGET_MASQUERADE IP_NF_TARGET_REDIRECT IP_NF_TARGET_REJECT \
	IP6_NF_IPTABLES IP6_NF_IPTABLES_LEGACY IP6_NF_FILTER IP6_NF_MANGLE IP6_NF_RAW IP6_NF_NAT \
	IP6_NF_TARGET_MASQUERADE IP6_NF_TARGET_REJECT \
	IP_VS IP_VS_IPV6 IP_VS_NFCT IP_VS_PROTO_TCP IP_VS_PROTO_UDP IP_VS_RR \
	BRIDGE BRIDGE_NETFILTER BRIDGE_VLAN_FILTERING VLAN_8021Q \
	VETH VXLAN IPVLAN MACVLAN DUMMY TUN OVERLAY_FS \
	XFRM_USER INET_ESP CRYPTO_SEQIV CRYPTO_GCM CRYPTO_GHASH \
	NET_SCHED NET_SCH_FQ_CODEL NET_CLS NET_CLS_CGROUP CGROUP_NET_PRIO \
	CFS_BANDWIDTH BLK_DEV_THROTTLING; do
	cfg --enable "$sym"
done
# USB Ethernet adapters (and phones tethering over USB): the server should
# not depend on the WiFi. Realtek RTL8152/8153/8156, ASIX AX88xxx, Microchip
# LAN78xx/SMSC95xx (the hub-with-Ethernet chips), and the class drivers CDC
# ECM/NCM/EEM and RNDIS. The adapter goes into the USB-C hub of
# extras/usb-c-display, which already powers the bus.
for sym in NETDEVICES USB_NET_DRIVERS USB_USBNET USB_RTL8152 USB_NET_AX8817X \
	USB_NET_AX88179_178A USB_LAN78XX USB_NET_SMSC95XX USB_NET_CDCETHER \
	USB_NET_CDC_NCM USB_NET_CDC_EEM USB_NET_RNDIS_HOST; do
	cfg --enable "$sym"
done
cfg --enable BLK_DEV_INITRD
cfg --set-str INITRAMFS_SOURCE "$RAMFS"
cfg --set-val INITRAMFS_ROOT_UID 0
cfg --set-val INITRAMFS_ROOT_GID 0
# display: bootloader framebuffer until the real DECON driver lands
cfg --enable DRM
cfg --disable DRM_SIMPLEDRM
cfg --enable DRM_EXYNOS_BOOTFB
cfg --enable DRM_FBDEV_EMULATION
# Boot logo: this port's own, drawn by fbcon the moment it binds to the panel
# (scripts/make_logo.py renders it). Replaces Tux, and covers the bootloader's
# Samsung logo with something of this build's own.
install -m644 "$PROJ/branding/shero_logo.ppm" drivers/video/logo/logo_shero_clut224.ppm
cfg --enable LOGO
cfg --enable LOGO_LINUX_CLUT224
cfg --set-str LOGO_LINUX_CLUT224_FILE "drivers/video/logo/logo_shero_clut224.ppm"
cfg --enable FRAMEBUFFER_CONSOLE
cfg --enable FRAMEBUFFER_CONSOLE_DETECT_PRIMARY
cfg --enable SYSFB_SIMPLEFB
# platform
cfg --enable ARCH_EXYNOS
cfg --enable PINCTRL_EXYNOS
cfg --enable KEYBOARD_GPIO
cfg --enable DRM_PANFROST
cfg --enable IOMMU_IO_PGTABLE_LPAE
cfg --enable ARM_MALI_LPAE
cfg --enable EXYNOS_PMU
cfg --enable EXYNOS_PM_DOMAINS
cfg --enable INPUT_EVDEV
cfg --enable EXYNOS_PMU
# CPU frequency scaling with the vendor PLL tables and BUCK rails via ACPM
cfg --enable CPU_FREQ
cfg --enable CPU_FREQ_GOV_SCHEDUTIL
cfg --enable CPU_FREQ_DEFAULT_GOV_SCHEDUTIL
cfg --enable CPU_FREQ_STAT
cfg --enable ARM_S9P_CPUFREQ
# contiguous, write-combined dumb buffers for the DECON scanout driver
cfg --enable DMA_CMA
# 1440x2960 XRGB is 17 MB per buffer: fbcon + mutter's triple buffering +
# screenshots/Xwayland; 64 MB ran dry (CREATE_DUMB -ENOMEM measured).
cfg --set-val CMA_SIZE_MBYTES 128
	if [ -n "${DECON:-}" ]; then
		cfg --enable DRM_S9P_DECON
	fi
	# WiFi over the PCIe channel 0 root complex (BCM4361). Comes after the
	# strip loop above, which turns PCI/WLAN/WIRELESS off wholesale.
	#
	# The chip's firmware never signals MSI (vendor dhdpcie_chip_support_msi,
	# stock DT use-msi = "false"), so PCI_MSI stays off and the endpoint runs
	# on INTA through the root complex's INTx domain.
	#
	# WLAN=1 builds everything in and embeds the firmware into the Image
	# (EXTRA_FIRMWARE), so the driver probes before the rootfs is mounted and
	# needs nothing from it. The files are the phone's own, renamed the way
	# brcmfmac asks for them: bcmdhd_sta.bin_b2 -> brcmfmac4361-pcie.bin,
	# nvram.txt_r02a_b2 -> .txt, bcmdhd_clm.blob -> .clm_blob. Copied under
	# $BUILD because the kernel's firmware Makefile cannot take a directory
	# with a space in it.
	cfg --set-str EXTRA_FIRMWARE ""
	if [ -n "${WLAN:-}" ]; then
		echo "WLAN=$WLAN: PCIe RC for the BCM4361 plus brcmfmac"
		sed -i 's|status = "disabled"; /\* WLAN-STATUS \*/|status = "okay"; /* WLAN-STATUS */|' \
			arch/arm64/boot/dts/exynos/exynos9810-star2lte.dts
		grep -n 'WLAN-STATUS' arch/arm64/boot/dts/exynos/exynos9810-star2lte.dts
		cfg --enable PCI
		cfg --disable PCI_MSI
		cfg --enable PCIE_DW
		cfg --enable PCIE_DW_HOST
		cfg --enable WLAN
		cfg --enable WIRELESS
		if [ "$WLAN" = m ]; then
			echo "WLAN=m: host controller and brcmfmac as modules (loaded by hand over SSH)"
			cfg --module PCIE_EXYNOS9810
			cfg --module CFG80211
			cfg --module BRCMUTIL
			cfg --module BRCMFMAC
		else
			cfg --enable PCIE_EXYNOS9810
			cfg --enable CFG80211
			cfg --enable BRCMUTIL
			cfg --enable BRCMFMAC
			FWDIR="$BUILD/firmware"
			mkdir -p "$FWDIR/brcm"
			cp "$PROJ/firmware/bcmdhd_sta.bin_b2" "$FWDIR/brcm/brcmfmac4361-pcie.bin"
			cp "$PROJ/firmware/nvram.txt_r02a_b2" "$FWDIR/brcm/brcmfmac4361-pcie.txt"
			cp "$PROJ/firmware/bcmdhd_clm.blob" "$FWDIR/brcm/brcmfmac4361-pcie.clm_blob"
			sed -i 's/\r$//' "$FWDIR/brcm/brcmfmac4361-pcie.txt"
			# wireless-regdb (kernel.org, signed by sforshee whose key the
			# kernel carries), so 5 GHz channels follow the country code
			cp "$PROJ/firmware/regulatory.db" "$PROJ/firmware/regulatory.db.p7s" "$FWDIR/"
			cfg --set-str EXTRA_FIRMWARE_DIR "$FWDIR"
			cfg --set-str EXTRA_FIRMWARE "brcm/brcmfmac4361-pcie.bin brcm/brcmfmac4361-pcie.txt brcm/brcmfmac4361-pcie.clm_blob regulatory.db regulatory.db.p7s"
		fi
		cfg --enable BRCMFMAC_PCIE
	fi
cfg --enable SCSI
cfg --enable BLK_DEV_SD
cfg --enable SCSI_UFSHCD
cfg --enable SCSI_UFSHCD_PLATFORM
cfg --enable SCSI_UFS_EXYNOS9810
# Samsung's inline crypto (FMP) is not wired up here; let the core ignore the
# controller's crypto capability rather than half-enable it.
cfg --disable SCSI_UFS_CRYPTO
# USB gadget. GADGET=serial (default) builds the legacy g_serial in: it binds
# the UDC before userspace exists and needs nothing from the rootfs, which is
# what a bring-up wants. GADGET=configfs leaves the UDC to the rootfs'
# usb-gadget.service instead (ACM console plus an NCM network for SSH).
cfg --enable USB_GADGET
cfg --enable USB_LIBCOMPOSITE
cfg --enable USB_U_SERIAL
cfg --enable USB_F_ACM
if [ "${GADGET:-serial}" = configfs ]; then
	echo "GADGET=configfs: no legacy gadget, functions for the rootfs service"
	cfg --disable USB_G_SERIAL
	cfg --enable USB_CONFIGFS
	cfg --enable USB_CONFIGFS_SERIAL
	cfg --enable USB_CONFIGFS_ACM
	cfg --enable USB_CONFIGFS_NCM
	cfg --enable USB_CONFIGFS_ECM
	cfg --enable USB_CONFIGFS_RNDIS
	cfg --enable USB_CONFIGFS_MASS_STORAGE
	cfg --enable USB_F_NCM
	cfg --enable USB_F_ECM
	cfg --enable USB_F_RNDIS
	cfg --enable USB_F_MASS_STORAGE
	cfg --enable USB_U_ETHER
else
	cfg --enable USB_G_SERIAL
fi
# "depends on TYPEC || !TYPEC": with TYPEC=m the PHY driver is capped at m,
# and the rootfs has no modules for this kernel. Nothing here needs Type-C.
cfg --disable TYPEC
cfg --enable GENERIC_PHY
cfg --enable PHY_EXYNOS5_USBDRD
# touch (S6SY761 on hsi2c@104b0000) and the MAX77705 PMIC (charger + fuel
# gauge on hsi2c@14360000): mainline drivers, stock wiring.
cfg --enable I2C
cfg --enable I2C_CHARDEV
cfg --enable I2C_EXYNOS5
cfg --enable INPUT_TOUCHSCREEN
cfg --enable TOUCHSCREEN_S6SY761
cfg --enable MFD_MAX77705
cfg --enable MFD_SIMPLE_MFD_I2C
cfg --enable CHARGER_MAX77705
cfg --enable BATTERY_MAX1720X
cfg --enable BATTERY_MAX17042
cfg --enable POWER_SUPPLY
cfg --enable REGMAP_I2C
cfg --enable REGMAP_IRQ
cfg --enable USB_CONFIGFS
cfg --enable USB_DWC3_GADGET
cfg --enable EXT4_FS
cfg --enable TMPFS
cfg --enable DEVTMPFS
cfg --enable DEVTMPFS_MOUNT
# the only channel back to the host: ramoops the stock 4.9 kernel can parse
cfg --enable PSTORE
cfg --enable PSTORE_RAM
cfg --enable PSTORE_CONSOLE
cfg --disable PSTORE_PMSG
cfg --disable PSTORE_COMPRESS
cfg --enable MAGIC_SYSRQ
cfg --enable DEVMEM
cfg --disable STRICT_DEVMEM
cfg --enable POWER_RESET_SYSCON
cfg --enable POWER_RESET_SYSCON_POWEROFF
cfg --enable SYSCON_REBOOT_MODE
# Cluster 1's watchdog. Every second warm reboot wedges in the probe phase and
# the device then stays on the Samsung logo until someone holds the buttons;
# armed, that becomes a reset into TWRP. systemd pets it once the system runs.
cfg --enable WATCHDOG
cfg --enable WATCHDOG_SYSFS
cfg --enable S3C2410_WATCHDOG
cfg --disable WATCHDOG_NOWAYOUT
# sboot appends console=ram and its own debug args; force our own instead
# One less unknown in early boot: Samsung's bootloader patches the device tree
# itself and reserves its own window at the bottom of RAM, and KASLR buys
# nothing on a phone that is being brought up.
cfg --disable RANDOMIZE_BASE
# 4K pages with a 52-bit VA and PA need FEAT_LVA and FEAT_LPA2. The Mongoose
# M3 in this SoC has neither. Mainline does fall back at runtime, but that
# fallback lives in the early page table setup -- before any console exists,
# which is exactly the window this port kept dying in. 48 bits is what every
# other Exynos port runs and what arm64 defconfig picked until recently.
cfg --disable ARM64_VA_BITS_52
cfg --disable ARM64_PA_BITS_52
cfg --enable ARM64_VA_BITS_48
cfg --enable ARM64_PA_BITS_48
# Allow per-task latency hints without pinning entire CPU policies at max.
cfg --enable UCLAMP_TASK
cfg --enable CMDLINE_FORCE
# Verified star2lte eight-core startup: initialize the M3 PMU defaults before
# CPU_ON and bring up both clusters before arm64 finalizes CPU capabilities.
# The required port patches must already be present in SRC (archived per build).
# Console policy: the panel shows nothing at boot. The kernel log goes to the
# ramoops console only (full loglevel, readable from TWRP/pstore after a
# crash); tty0 is not a console at all, so no kernel or systemd text ever
# reaches the display, the fbcon cursor is off and plymouth stays out.
CMDLINE="${BOOT_CMDLINE:-console=ramoops-1 loglevel=7 panic=5 earlycon=s9pram keep_bootcon maxcpus=8 s9p_late_ramlog s9p_vendor_m3_defaults vt.global_cursor_default=0 plymouth.enable=0 systemd.show_status=false}"
if [ -n "${POKE:-}" ]; then
	CMDLINE="$CMDLINE s9p.poke=$POKE"
fi
cfg --set-str CMDLINE "$CMDLINE"
for extra in "$@"; do
	cfg $extra
done
make -s O=out olddefconfig
grep -E '^CONFIG_(PSTORE|PSTORE_RAM|PSTORE_CONSOLE|PSTORE_COMPRESS|DEVMEM|MAGIC_SYSRQ|SYSCON_REBOOT_MODE|DRM_SIMPLEDRM|CMDLINE)=' out/.config

echo "=== build ==="
if ! make -j"$(nproc)" O=out Image dtbs >"$LOG" 2>&1; then
	echo "--- BUILD FAILED ---"
	grep -nE '\*\*\*|error:|Error [0-9]|warning: .*undefined' "$LOG" | head -40
	sync
	exit 1
fi
tail -2 "$LOG"
if [ "${WLAN:-}" = m ]; then
	echo "=== modules ==="
	if ! make -j"$(nproc)" O=out modules >>"$LOG" 2>&1; then
		echo "--- MODULES FAILED ---"; grep -nE "error:|Error [0-9]" "$LOG" | head -20; sync; exit 1
	fi
fi
ls -l out/arch/arm64/boot/Image out/arch/arm64/boot/dts/exynos/exynos9810-star2lte.dtb

echo "=== text_offset fixup ==="
# Mainline has set the arm64 image header's text_offset field to 0 since 5.8,
# because the kernel relocates itself. Samsung's 2018 sboot still reads that
# field and places the kernel by it -- the 4.9 kernel that boots on this phone
# carries 0x00080000 there. Writing that value back costs nothing (a
# relocatable kernel does not read its own header) and keeps the bootloader
# putting the image where it expects it.
python3 - "$SRC/out/arch/arm64/boot/Image" <<'PYEOF'
import struct
import sys
from pathlib import Path

p = Path(sys.argv[1])
b = bytearray(p.read_bytes())
old = struct.unpack_from("<Q", b, 8)[0]
if old != 0x80000:
    struct.pack_into("<Q", b, 8, 0x80000)
    p.write_bytes(b)
print(f"text_offset: 0x{old:x} -> 0x{struct.unpack_from('<Q', p.read_bytes()[:16], 8)[0]:x}")
PYEOF

echo "=== boot image ==="
# Samsung's bootloader is happier with a ramdisk present in the header,
# and an empty archive costs nothing: the real initramfs is built into
# the kernel image.
if [ ! -f "$BUILD/out/empty-ramdisk.cpio.gz" ]; then
	printf "" | cpio -o -H newc 2>/dev/null | gzip -9 > "$BUILD/out/empty-ramdisk.cpio.gz"
fi
OUT="$DIST/m71-$STAMP"
mkdir -p "$OUT"
# sboot wants the DTBH table dtbTool-exynos produces, not a bare blob.
# mkdtbh.py and mkboot.py write the same bytes as dtbTool-exynos and
# Samsung's mkbootimg did, without a prebuilt binary; mkboot.py also
# appends the SEANDROIDENFORCE trailer.
python3 "$PROJ/scripts/build/mkdtbh.py" -o "$OUT/dt.img" -s 2048 \
	out/arch/arm64/boot/dts/exynos/exynos9810-star2lte.dtb || { sync; exit 1; }
od -A x -t x1z -N 48 "$OUT/dt.img"
python3 "$PROJ/scripts/build/mkboot.py" --kernel out/arch/arm64/boot/Image --dt "$OUT/dt.img" \
	--ramdisk "$BUILD/out/empty-ramdisk.cpio.gz" \
	--cmdline "$CMDLINE" \
	--base 0x10000000 --kernel_offset 0x00008000 --ramdisk_offset 0x01000000 \
	--second_offset 0x00f00000 --tags_offset 0x00000100 --pagesize 2048 \
	-o "$OUT/boot.img" || { sync; exit 1; }
SIZE=$(stat -c %s "$OUT/boot.img")
echo "boot.img: $SIZE bytes ($((57671680 - SIZE)) spare in BOOT)"
[ "$SIZE" -le 57671680 ] || { echo "TOO BIG"; sync; exit 1; }
sha256sum "$OUT/boot.img" | tee "$OUT/SHA256SUMS"
cp arch/arm64/boot/dts/exynos/exynos9810-star2lte.dts "$OUT/"
cp out/.config "$OUT/kernel.config"
if [ "${WLAN:-}" = m ]; then
	rm -rf "$OUT/modules"
	STAGE="$BUILD/modstage"; rm -rf "$STAGE"
	make -s O=out modules_install INSTALL_MOD_PATH="$STAGE" INSTALL_MOD_STRIP=1 >>"$LOG" 2>&1 || { echo "--- MODULES_INSTALL FAILED ---"; tail -5 "$LOG"; }
	mkdir -p "$OUT/modules" && cp -a "$STAGE/lib/modules/." "$OUT/modules/" 2>/dev/null
	find "$OUT/modules" -name "*.ko" | sed "s|$OUT/||"
fi
cp out/System.map out/vmlinux "$OUT/"
cp out/arch/arm64/boot/Image "$OUT/Image"
cp out/arch/arm64/boot/dts/exynos/exynos9810-star2lte.dtb "$OUT/"
git diff --binary > "$OUT/kernel-source.patch"
mkdir -p "$OUT/port-sources"
cp drivers/ufs/host/ufs-exynos9810.c drivers/ufs/host/ufs-cal-9810.[ch] \
	drivers/gpu/drm/sysfb/exynos-bootfb.c "$OUT/port-sources/"
cp -a "$PROJ/src" "$OUT/port-sources/port"
cp -a "$PROJ/scripts/build" "$OUT/port-sources/build-scripts"
git rev-parse HEAD > "$OUT/kernel-base-commit.txt"
sync
echo "BUILD71_OK $OUT"
