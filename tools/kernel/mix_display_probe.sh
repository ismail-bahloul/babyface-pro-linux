#!/bin/sh
# mix_display_probe.sh — probe the 0x1A 0x000A/0x000B "panel display"
# shadow during MIX (fader) mode: write a sweep of values and watch the
# physical input VU for a single flashing LED (the TotalMix MIX-mode
# display, cap_mix.pcap 2026-08-23: values 0x0001..0x0005 while the
# fader ran 0x0003..0x0117).
#
# Usage: sh mix_display_probe.sh [bus/dev]
#   default bus/dev = 003/002.  Press MIX on the card FIRST (fader mode).
set -u
DEV="${1:-003/002}"
U="$(dirname "$0")/usbwrite"
[ -x "$U" ] || { gcc -o "$U" "$(dirname "$0")/usbwrite.c" || exit 1; }

echo "== MIX display shadow sweep on $DEV =="
echo "   (watch the input VU meters for a single flashing LED)"
for v in 0x00 0x04 0x08 0x0c 0x10 0x14 0x18 0x1c 0x1f 0x24 0x28 0x2c 0x30; do
	printf "%s -> " "$v"
	sudo -n "$U" "$DEV" w 0x1a "$v" 0x000a || echo "write failed"
	sleep 1.5
done
echo "== resetting to 0 =="
sudo -n "$U" "$DEV" w 0x1a 0x0000 0x000a
sudo -n "$U" "$DEV" w 0x1a 0x0000 0x000b
echo done
