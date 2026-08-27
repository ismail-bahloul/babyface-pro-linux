#!/bin/sh
# selhold_probe.sh — watch the 0x17 panel readback while SELECT is held.
# Prints every change of the 4-byte state at ~20 Hz for ~12 s.  HOLD
# SELECT (do NOT tap it) and watch byte0's 0x80 "engaged" bit + byte3.
set -u
DEV="${1:-003/002}"
U="$(dirname "$0")/usbwrite"
[ -x "$U" ] || { gcc -o "$U" "$(dirname "$0")/usbwrite.c" || exit 1; }
echo "== watching 0x17 readback on $DEV — HOLD SELECT for ~12 s =="
prev=""
i=0
while [ "$i" -lt 240 ]; do
	st=$(sudo -n "$U" "$DEV" r 0x17 0x0000 2>/dev/null | sed -n 's/.*-> //p')
	if [ -n "$st" ] && [ "$st" != "$prev" ]; then
		printf "t=%4ds  %s\n" "$((i / 20))" "$st"
		prev="$st"
	fi
	i=$((i + 1))
	sleep 0.05
done
echo "== done =="
