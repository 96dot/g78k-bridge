#!/bin/sh
# Watch the GameSir G7 Pro 8K's raw Xbox-mode (3537:1034/1035) input stream for 15 s
# using the kernel's USB monitor (read-only: changes nothing on the controller).
# Prints each distinct packet once, so pressing a button shows which bytes change.
# Usage: tools/sniff-xbox-mode.sh      (asks for sudo)
set -eu
DEV=
for d in /sys/bus/usb/devices/*; do
	[ "$(cat "$d/idVendor" 2>/dev/null)" = 3537 ] || continue
	case "$(cat "$d/idProduct")" in 1034|1035) DEV=$d ;; esac
done
[ -n "$DEV" ] || { echo "Controller not in Xbox mode (3537:1034/1035)"; exit 1; }
BUS=$(cat "$DEV/busnum"); NUM=$(cat "$DEV/devnum")
echo "Controller: bus $BUS device $NUM. Recording 15 s: press L4, R4, L5, R5, then A."
sudo modprobe usbmon
OUT=$(mktemp -d "${TMPDIR:-/tmp}/g78k-sniff.XXXXXX")
NUMP=$(printf '%03d' "$NUM")
trap 'rm -rf "$OUT"' EXIT
# usbmon text format: ... Ii:BUS:DEV:EP status len = data words.
# usbmon sees the whole bus (keyboards too), so keep only this controller's input,
# filtered as it is captured; nothing else is ever written to disk.
sudo timeout 15 cat "/sys/kernel/debug/usb/usbmon/${BUS}u" | grep --line-buffered -E " C Ii:${BUS}:${NUMP}:" > "$OUT/in.txt" || true
echo
echo "packets per endpoint in 15 s:"
awk '{split($4, a, ":"); print "endpoint " a[4]}' "$OUT/in.txt" | sort | uniq -c
echo
echo "distinct packets (first 60), endpoint then data:"
awk '{split($4, a, ":"); d=""; for (i=8; i<=NF; i++) d=d " " $i; if (!seen[a[4] d]++) print "ep" a[4] ":" d}' "$OUT/in.txt" | head -60
