#!/bin/sh
# Round-trip latency + xrun test for the kernel driver (loopback).
# Usage: sh latency-test.sh  (the Babyface must be the card under test,
# adjust the card number via CARD= below).
#
# Principle: play a periodic impulse train on PB1, enable the AS1/2
# output loopback (which lands on the optical input = device words 4/5
# = capture ch4/5 in the 12-ch PCM), capture 12 ch, and cross-check
# the impulse positions: the phase of the first impulse = the
# round-trip latency; any period != the nominal one = a glitch/xrun.

CARD=${CARD:-3}
PERIOD=4800          # samples between impulses (100 ms at 48 kHz)
WIDTH=60             # impulse width in samples
DUR=30               # test duration (s)
RATE=48000

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"; [ -n "$LBID" ] && amixer -c "$CARD" cset "$LBID" "${LB_BEFORE:-off}" >/dev/null 2>&1' EXIT

# Find the AS1/2 loopback control (the third 'Loopback Switch', index=2),
# which lands on capture ch4/5 under the driver's identity capture map.
LBID=$(amixer -c "$CARD" controls | awk -F'[ ,]' '/Loopback Switch/ && /index=2/ {print $1}' | head -1)
[ -z "$LBID" ] && { echo "AS1/2 loopback control not found"; exit 1; }
LB_BEFORE=$(amixer -c "$CARD" cget "$LBID" 2>/dev/null | sed -n 's/.*values=//p' | tail -1)

python3 - "$TMP/imp.raw" "$PERIOD" "$WIDTH" "$DUR" "$RATE" <<'EOF'
import struct, sys
out, period, width, dur, rate = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
n = int(rate * dur)
with open(out, 'wb') as f:
    for i in range(n):
        v = int(0.5 * 0x7FFFFF) if (i % period) < width else 0
        s = struct.pack('<i', v)
        f.write(s + s)
EOF

amixer -c "$CARD" cset "$LBID" on >/dev/null
( aplay -q -D "hw:$CARD,0" -f S24_LE -c 2 -r "$RATE" "$TMP/imp.raw" & )
sleep 0.3
arecord -t raw -D "hw:$CARD,0" -f S24_LE -c 12 -r "$RATE" -d "$DUR" "$TMP/cap.raw" 2>/dev/null
pkill -f imp.raw 2>/dev/null
amixer -c "$CARD" cset "$LBID" "${LB_BEFORE:-off}" >/dev/null

python3 - "$TMP/cap.raw" "$PERIOD" "$RATE" <<'EOF'
import struct, sys
path, period, rate = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
data = open(path, 'rb').read()
frame = 12 * 4
n = len(data) // frame
print(f"frames: {n} (expected {30 * rate})")
s = [int.from_bytes(data[i*frame+4*4:i*frame+4*4+3], 'little', signed=True)
     for i in range(n)]
pos = [i for i in range(1, n)
       if abs(s[i]) > 0x7FFFFF * 0.3 and abs(s[i-1]) < 0x7FFFFF * 0.1]
print(f"impulses: {len(pos)}")
if pos:
    print(f"latency: {pos[0]} samples = {pos[0] / rate * 1000:.2f} ms "
          f"(first-impulse phase)")
periods = [pos[i+1] - pos[i] for i in range(len(pos) - 1)]
bad = [p for p in periods if p != period]
print(f"glitched periods: {len(bad)}")
if bad:
    print("  examples:", bad[:10])
EOF
