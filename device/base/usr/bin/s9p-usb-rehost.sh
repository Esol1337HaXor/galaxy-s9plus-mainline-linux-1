#!/bin/sh
# S9+: USB-Host-Re-Enumeration nach Hotplug erzwingen
# PROBLEM (2 Varianten nach Adapter-Abziehen/Wiederanstecken):
#   A) role faellt auf device (peripheral), xhci-Busse werden deregistert
#   B) role bleibt host, aber die xhci-Enumeration schlaegt fehl
#      (nur Root-Ports usb1/usb2 bleiben, Hub 1-1 + Touch fehlen)
# FIX: Rehost-Toggle (device -> host) erzwingt die xhci-Re-Initialisierung
#      und damit die Neu-Enumeration des Hubs und Touchscreens.
# GUARD: Nur eingreifen, wenn kein Hub am Bus haengt (USB tot) — sonst
#        wuerde der Toggle etablierte USB-Devices killen.
ROLE=/sys/class/usb_role/10c00000.usb-role-switch/role

# Hub schon da? Dann nichts tun.
if [ -d /sys/bus/usb/devices/1-1 ] || [ -d /sys/bus/usb/devices/2-1 ]; then
    exit 0
fi

# Rehost-Toggle: device (2s) -> host
echo device > "$ROLE"
sleep 1
echo host > "$ROLE"
