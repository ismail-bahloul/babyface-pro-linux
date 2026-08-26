#!/usr/bin/env python3
"""lbcap_master.py — per-second 8-bit + 16-bit master values (from the
0x1A/0x12 control writes, setup at pkt[28:]) vs IN ch2 RMS (payload at
pkt[27:]) for cap_lbph.  Correlates the IN-level sweep with the fader
moves."""
import struct, sys

data = open(sys.argv[1], 'rb').read()
off = 24
t0 = None
m8 = m16 = None
buckets = {}   # sec -> [n_in2, s_in2, m8, m16]
while off + 16 <= len(data):
    ts, us, cl, o = struct.unpack_from('<IIII', data, off)
    off += 16
    pkt = data[off:off + cl]
    off += cl
    if cl < 36:
        continue
    t = ts + us / 1e6
    if t0 is None:
        t0 = t
    b = int(t - t0)
    ep = struct.unpack_from('<H', pkt, 20)[0]
    if ep == 0x0000:
        setup = pkt[28:36]
        if setup[0] == 0x40:
            breq, wval, widx, wlen = struct.unpack_from('<BHHH', setup, 1)
            if breq == 0x1A and widx in (0x0006, 0x0007):
                m8 = wval & 0xFF
            if breq == 0x12 and widx in (0x03e2, 0x03e3):
                m16 = wval
            d = buckets.setdefault(b, [0, 0.0, None, None])
            d[2] = m8
            d[3] = m16
        continue
    if ep == 0x8200 and len(pkt) >= 27 + 56:
        payload = pkt[27:]
        n = len(payload) // 56
        s = 0.0
        for f in range(n):
            w = struct.unpack_from('<I', payload, f * 56 + 8)[0]
            v = (w >> 8) & 0xFFFFFF
            if v >= 0x800000:
                v -= 0x1000000
            v /= 8388608.0
            s += v * v
        d = buckets.setdefault(b, [0, 0.0, None, None])
        d[0] += n
        d[1] += s

print(f"{'t':>4} {'m8':>5} {'m16':>6} {'IN ch2 RMS':>10}")
for b in sorted(buckets):
    n, s, m8, m16 = buckets[b]
    r = (s / n) ** 0.5 if n else 0.0
    m8s = f"{m8:#06x}" if m8 is not None else "  -  "
    m16s = f"{m16:#06x}" if m16 is not None else "  -  "
    print(f"{b:4d} {m8s:>5} {m16s:>6} {r:10.5f}")
