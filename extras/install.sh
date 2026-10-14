#!/bin/sh
# Install or remove one optional feature on the phone. Nothing in extras/ is
# part of the normal image; each feature is installed only when you ask.
#
#   sudo extras/install.sh                     list the features
#   sudo extras/install.sh <feature>           install it
#   sudo extras/install.sh <feature> --remove  remove it again
#   sudo NO_START=1 extras/install.sh <feature> install and enable, start at the
#                                              next boot (before a reboot, or
#                                              when changing the kernel)
#
# A feature is a directory with files/ (copied to /), and optionally
# `packages` (installed with apt), `units` (systemd units enabled),
# `post-install` and `pre-remove` (scripts run after install / before removal).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
if [ $# -eq 0 ]; then
	for d in "$HERE"/*/; do
		d=${d%/}
		printf '%-18s %s\n' "${d##*/}" "$(sed -n '3p' "$d/README.md")"
	done
	exit 0
fi
F=$1
D=$HERE/$F
[ -d "$D/files" ] || { echo "no such feature: $F (run without arguments for the list)" >&2; exit 1; }
[ "$(id -u)" = 0 ] || { echo "run as root (sudo)" >&2; exit 1; }
units=$(sed 's/#.*//' "$D/units" 2>/dev/null | tr -s ' \n' ' ' || true)
units=${units# }; units=${units% }

if [ "${2:-}" = --remove ]; then
	[ ! -f "$D/pre-remove" ] || sh "$D/pre-remove"
	[ -z "$units" ] || systemctl disable --now $units || true
	(cd "$D/files" && find . ! -type d) | while read -r f; do rm -f "/${f#./}"; done
	systemctl daemon-reload
	echo "$F removed"
	exit 0
fi

pkgs=$(sed 's/#.*//' "$D/packages" 2>/dev/null | tr -s ' \n' ' ' || true)
pkgs=${pkgs# }; pkgs=${pkgs% }
[ -z "$pkgs" ] || apt-get install -y --no-install-recommends $pkgs
# --keep-directory-symlink: never turn /lib or /bin (symlinks into /usr) into
# real directories, should a feature ever ship files below them.
(cd "$D/files" && tar --owner=0 --group=0 --numeric-owner -cf - .) |
	tar -C / --no-overwrite-dir --keep-directory-symlink -xpf -
systemctl daemon-reload
[ ! -d "$D/files/etc/sysctl.d" ] || sysctl --system >/dev/null
if [ -n "${NO_START:-}" ]; then
	[ -z "$units" ] || systemctl enable $units
else
	[ -z "$units" ] || systemctl enable --now $units
fi
[ ! -f "$D/post-install" ] || NO_START=${NO_START:-} sh "$D/post-install"
echo "$F installed (remove with: sudo extras/install.sh $F --remove)"
