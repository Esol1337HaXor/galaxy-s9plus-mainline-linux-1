#!/bin/sh
# S9+ Auto-Dock beim Boot: USB-Rehost + DP-only-Anzeige
# PROBLEM: (1) Nach dem Boot ist der USB-Role "device" (s9p-usbhost: DWC3 is
# dual-role default device) und KEIN change-Event feuert -> udev-Regel greift
# nicht -> USB/DP tot. (2) Mutter matcht beim Boot die 1-Monitor-Konfigurationen
# aus monitors.xml NICHT (beide Monitore present -> kein Complete-Match ->
# Default "beide nebeneinander").
# FIX: (1) USB-Rehost beim Boot, (2) DP-only via ApplyMonitorsConfig (method 2
# = persistent) erzwingen, sobald Mutter laeuft und DP-1 present ist.
/usr/bin/s9p-usb-rehost.sh

export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
i=0
while [ $i -lt 12 ]; do
  sleep 5
  STATE=$(gdbus call --session --dest org.gnome.Mutter.DisplayConfig \
    --object-path /org/gnome/Mutter/DisplayConfig \
    --method org.gnome.Mutter.DisplayConfig.GetCurrentState 2>/dev/null)
  [ -z "$STATE" ] && i=$((i + 1)) && continue

  # Mutter ist da. Nur eingreifen, wenn der externe Monitor present ist.
  if echo "$STATE" | grep -q "DP-1"; then
    SERIAL=$(echo "$STATE" | grep -o 'uint32 [0-9]*' | head -1 | cut -d' ' -f2)
    gdbus call --session --dest org.gnome.Mutter.DisplayConfig \
      --object-path /org/gnome/Mutter/DisplayConfig \
      --method org.gnome.Mutter.DisplayConfig.ApplyMonitorsConfig \
      "$SERIAL" 2 \
      "[(0, 0, 1.0, 0, true, [('DP-1', '1920x1080@60.000', {})])]" \
      "{'layout_mode': <uint32 2>}" >/dev/null 2>&1 \
      && echo "DP-only angewendet (serial $SERIAL)"
  else
    echo "DP-1 nicht present — kein Eingriff (nur internes Display)"
  fi
  exit 0
done
echo "Mutter nach 60 s nicht erreichbar — kein Apply"
