#!/usr/bin/env python3
"""in_words.py — per-second RMS of every IN-stream word (0..13), payload
at the correct offset (27), for a capture.  Shows which words carry
which bus (AN1/2 vs Phones vs status) and where the loopback lands."""
import struct, sys

data = open(sys.argv[1], 'rb').read()
off = 24
t0 = None
acc = {}   # sec -> [n, sum[14]]
NW = 14
while off + 16 <= len(data):
    ts, us, cl, o = struct.unpack_from('<IIII', data, off)
    off += 16
    pkt = data[off:off + cl]
    off += cl
    if cl < 36:
        continue
    ep = struct.unpack_from('<H', pkt, 20)[0]
    if ep != 0x8200 or len(pkt) < 27 + 56:
        continue
    payload = pkt[27:]
    t = ts + us / 1e6
    if t0 is None:
        t0 = t
    b = int(t - t0)
    d = acc.setdefault(b, [0, [0.0] * NW])
    n = len(payload) // 56
    d[0] += n
    for f in range(n):
        base = f * 56
        for w in range(NW):
            v = struct.unpack_from('<I', payload, base + w * 4)[0]
            s = (v >> 8) & 0xFFFFFF
            if s >= 0x800000:
                s -= 0x1000000
            d[1][w] += (s / 8388608.0) ** 2

print(f"{'t':>4} " + " ".join(f"w{w:<6}" for w in range(6)))
for b in sorted(acc):
    n, sums = acc[b]
    r = [((s / n) ** 0.5) if n else 0 for s in sums]
    print(f"{b:4d} " + " ".join(f"{r[w]:.4f} " for w in range(6)))
