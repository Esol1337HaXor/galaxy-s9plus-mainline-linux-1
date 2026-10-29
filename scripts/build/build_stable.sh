#!/bin/bash
# S9+ kernel, stable configuration:
#  - the port drivers keep their log lines (their register read-backs are
#    part of the sequences; silencing them removed those reads and hung the
#    boot), so the panel shows the boot log again
#  - no Tux logo, no plymouth splash
#  - WiFi built in, firmware and regulatory database embedded
#  - no boot-mark driver
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
mount_build || exit 1
export INIT=boot UFS=1 GPU=1 DECON=1 WLAN=1 GADGET=configfs
#  - the cluster-1 watchdog starts with the driver (tmr_atboot), so the
#    probe phase where the warm-reboot hang lives is covered end to end;
#    systemd's RuntimeWatchdogSec takes over once userspace is up
#  - nowayout and stop_on_reboot=0 keep it armed through shutdown:
#    systemd-shutdown closes /dev/watchdog with the magic close, which
#    otherwise stops it, and a reboot that hangs after that never resets
export BOOT_CMDLINE="console=tty0 console=ramoops-1 loglevel=7 panic=5 earlycon=s9pram keep_bootcon maxcpus=8 s9p_late_ramlog s9p_vendor_m3_defaults plymouth.enable=0 s3c2410_wdt.tmr_atboot=1 s3c2410_wdt.tmr_margin=60 watchdog.stop_on_reboot=0 s3c2410_wdt.nowayout=1 s9p_decon.panel_sleep=0"
PROJ=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
bash "$PROJ/scripts/build/build71.sh" > "$BUILD/stable-build.log" 2>&1
rc=$?
tail -5 "$BUILD/stable-build.log"
echo "BUILD EXIT $rc"
sync
exit "$rc"
