#!/bin/sh
# selhold_probe2.sh — dump EVERY 0x17 readback frame at ~20 Hz for 20 s.
# Sequence: 3 quick SELECT taps, HOLD SELECT ~5 s, release.
# Prints every frame so the tap vs hold vs release pattern is exact.
set -u
DEV="${1:-003/002}"
U=/home/iswad/DATA/05_Code/Projects/TuxMix/tools/kernel/usbwrite
echo "== every-frame 0x17 dump on $DEV — taps, hold, release =="
i=0
while [ "$i" -lt 400 ]; do
	st=$(sudo -n "$U" "$DEV" r 0x17 0x0000 2>/dev/null | sed -n 's/.*-> //p')
	printf "%4d  %s\n" "$i" "${st:-<read failed>}"
	i=$((i + 1))
	sleep 0.05
done
echo "== done =="
