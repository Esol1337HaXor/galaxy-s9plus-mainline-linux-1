#!/bin/sh
# S9+: USB-Host-Re-Enumeration nach Hotplug erzwingen
# PROBLEM (2 Varianten nach Adapter-Abziehen/Wiederanstecken):
#   A) role faellt auf device (peripheral), xhci-Busse werden deregistert
#   B) role bleibt host, aber die xhci-Enumeration schlaegt fehl
#      (nur Root-Ports usb1/usb2 bleiben, Hub 1-1 + Touch fehlen)
# FIX: Rehost-Toggle (device -> host) erzwingt die xhci-Re-Initialisierung
#      und damit die Neu-Enumeration des Hubs und Touchscreens.
# GUARD 1: Nur eingreifen, wenn kein Hub am Bus haengt (USB tot) — sonst
#          wuerde der Toggle etablierte USB-Devices killen.
# GUARD 2: Nach dem DP-change-Event ist die USB-Enumeration oft noch im
#          Aufbau — erst 4 s warten und erneut pruefen, bevor eingegriffen
#          wird (dmesg zeigt connect/disconnect/error -71-Loops bei zu
#          fruehem Eingriff).
# GUARD 3: Cooldown 12 s — nur EIN Toggle pro Zeitfenster. Ohne das
#          verstaerkt ein Enumeration-Loop sich selbst (jeder Loop-Fehler
#          triggert change-Events -> Toggle -> Loop geht weiter).
ROLE=/sys/class/usb_role/10c00000.usb-role-switch/role
LOCK=/tmp/s9p-usb-rehost.lock

hub_da() {
    [ -d /sys/bus/usb/devices/1-1 ] || [ -d /sys/bus/usb/devices/2-1 ]
}

# Hub schon da? Dann nichts tun + Lock aufraeumen.
if hub_da; then
    rm -f "$LOCK"
    exit 0
fi

# Cooldown: innerhalb von 12 s nur EINMAL eingreifen.
if [ -f "$LOCK" ]; then
    NOW=$(date +%s)
    T=$(cat "$LOCK" 2>/dev/null || echo 0)
    AGE=$((NOW - T))
    [ "$AGE" -lt 12 ] 2>/dev/null && exit 0
fi

# Enumeration kann dauern — 4 s warten und erneut pruefen.
sleep 4
if hub_da; then
    rm -f "$LOCK"
    exit 0
fi

# Cooldown-Stamp setzen und Rehost-Toggle machen.
date +%s > "$LOCK"
echo device > "$ROLE"
sleep 1
echo host > "$ROLE"
