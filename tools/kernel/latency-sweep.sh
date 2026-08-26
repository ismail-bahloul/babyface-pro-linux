#!/bin/sh
# latency-sweep.sh — round-trip latency + xrun sweep across buffer/period
# sizes, using the phase-anchored looplat.c measurement (linked streams
# starting together on the device clock — see looplat.c).
#
# Usage: sh latency-sweep.sh [duration_s]   (default 8 s per point)
#
# The period floor = frames_per_urb (PERIOD_SIZE constraint, 2026-08-25)
# for any channel count: 256 frames at the default 256 fpu (5.3 ms),
# down to 16 frames at fpu=16 (0.33 ms, validated with nurbs=16).
# looplat links its streams on the device clock; latency is
# URB-quantized and channel-independent.
#
# Result format: latency_ms (loopback round-trip, URB-quantized) + xruns.

CARD=${CARD:-3}
DUR=${1:-8}
RATE=48000
SRC="$(dirname "$0")/looplat.c"
BIN="${TMPDIR:-/tmp}/looplat"

[ -x "$BIN" ] || gcc -O2 -o "$BIN" "$SRC" -lasound || exit 1

PERIODS_2CH="16 32 64 128 256 512 1024 2048 4096 8192"
PERIODS_12CH="16 32 64 128 256 512 1024 2048 4096 8192"

echo "== latency/xrun sweep @ ${RATE} Hz, ${DUR}s/point =="
echo "-- 2 ch (floor = fpu = 16 frames at fpu=16) --"
for p in $PERIODS_2CH; do
	printf "  2ch period=%-5s buffer=%-6s " "$p" "$((p * 2))"
	"$BIN" "$CARD" 2 "$p" "$((p * 2))" "$DUR" 2>/dev/null
done
echo "-- 12 ch (same floor: one URB = fpu frames) --"
for p in $PERIODS_12CH; do
	printf " 12ch period=%-5s buffer=%-6s " "$p" "$((p * 2))"
	"$BIN" "$CARD" 12 "$p" "$((p * 2))" "$DUR" 2>/dev/null
done
