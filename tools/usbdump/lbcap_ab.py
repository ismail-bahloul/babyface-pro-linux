#!/usr/bin/env python3
"""lbcap_ab.py — IN-stream ch2 RMS with BOTH sample interpretations
(bytes 0-2 vs bytes 1-3), per second, plus the OUT ch0 (bytes 1-3) for
reference.  Settles the IN frame layout (cap_lbph 2026-08-26)."""
import struct, sys

data = open(sys.argv[1], 'rb').read()
off = 24

# USBPcap header = 27 bytes (hlen field = 27) — the audio URB payload
# starts at pkt[27:].  A 1-byte shift (pkt[28:]) misaligns every frame
# (verified cap_lbph 2026-08-26: 256-frame grid only at offset 27).
PAY = 27

def rms_chan(payload, ch, shift):
    n = len(payload) // 56
    if n == 0:
        return 0.0
    s = 0.0
    for f in range(n):
        base = f * 56 + ch * 4
        word = struct.unpack_from('<I', payload, base)[0]
        v = (word >> shift) & 0xFFFFFF
        if v >= 0x800000:
            v -= 0x1000000
        v /= 8388608.0
        s += v * v
    return (s / n) ** 0.5

acc = {}
t0 = None
while off + 16 <= len(data):
    ts, us, cl, o = struct.unpack_from('<IIII', data, off)
    off += 16
    pkt = data[off:off + cl]
    off += cl
    if cl < 36:
        continue
    ep = struct.unpack_from('<H', pkt, 20)[0]
    t = ts + us / 1e6
    if t0 is None:
        t0 = t
    b = int(t - t0)
    d = acc.setdefault(b, [0, 0.0, 0.0, 0.0, 0.0])
    payload = pkt[PAY:]
    if len(payload) < 56:
        continue
    if ep == 0x8200:
        d[0] += 1
        d[1] += rms_chan(payload, 2, 0) ** 2     # bytes 0-2
        d[2] += rms_chan(payload, 2, 8) ** 2     # bytes 1-3
    elif ep == 0x0100:
        d[3] += 1
        d[4] += rms_chan(payload, 0, 8) ** 2     # OUT bytes 1-3

print(f"{'t':>4} {'INch2 b0-2':>11} {'INch2 b1-3':>11} {'OUTch0':>11}")
for b in sorted(acc):
    n, s02, s13, no, so = acc[b]
    r02 = (s02 / n) ** 0.5 if n else 0
    r13 = (s13 / n) ** 0.5 if n else 0
    ro = (so / no) ** 0.5 if no else 0
    print(f"{b:4d} {r02:11.5f} {r13:11.5f} {ro:11.5f}")
