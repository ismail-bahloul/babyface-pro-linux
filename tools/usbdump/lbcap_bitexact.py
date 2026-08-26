#!/usr/bin/env python3
"""lbcap_bitexact.py — compare IN vs OUT word-0 samples of cap_lbcal bit-exactly
on a steady window (loopback ON, master not yet swept).  If IN == OUT after a
fixed delay, the device loops the playback into the record at unity (1:1)."""
import struct, sys, math

path = sys.argv[1]
data = open(path, 'rb').read()
off = 24
out_s = []
in_s = []
t0 = None
while off + 16 <= len(data):
    ts_sec, ts_usec, cap_len, orig_len = struct.unpack_from('<IIII', data, off)
    off += 16
    pkt = data[off:off + cap_len]
    off += cap_len
    if cap_len < 36:
        continue
    ep = struct.unpack_from('<H', pkt, 20)[0]
    # USBPcap header_len (u16@0): 28 for control records, 27 for audio
    # URBs — payload at pkt[hlen:] (fixed 2026-08-26: pkt[28:] shifted
    # every audio frame by 1 byte).
    hlen = struct.unpack_from('<H', pkt, 0)[0]
    payload = pkt[hlen:]
    t = ts_sec + ts_usec / 1e6
    if t0 is None:
        t0 = t
    t -= t0
    if ep == 0x0100 and len(payload) >= 64:
        for k in range(0, len(payload) - 63, 64):
            w0 = struct.unpack_from('<I', payload, k)[0]
            s = (w0 >> 8) & 0xffffff
            if s & 0x800000:
                s -= 1 << 24
            out_s.append((t, s))
    elif ep == 0x8200 and len(payload) >= 64:
        for k in range(0, len(payload) - 63, 64):
            w0 = struct.unpack_from('<I', payload, k)[0]
            s = (w0 >> 8) & 0xffffff
            if s & 0x800000:
                s -= 1 << 24
            in_s.append((t, s))
print("out: %d samples, in: %d" % (len(out_s), len(in_s)))

def window(stream, lo, hi):
    return [s for t, s in stream if lo <= t < hi]

o = window(out_s, 10.0, 14.0)
i = window(in_s, 10.0, 14.0)
n = min(len(o), len(i))
print("window samples: %d" % n)
best = (1e18, 0)
for d in range(0, 600):
    m = n - d
    if m <= 0:
        break
    # subsample correlation: count exact matches and mean abs err
    match = 0
    err = 0
    for k in range(0, m, 5):
        if o[k + d] == i[k]:
            match += 1
        err += abs(o[k + d] - i[k])
    cnt = (m + 4) // 5
    if err < best[0]:
        best = (err, d, match)
err, d, match = best
cnt = (n - d + 4) // 5
print("best delay=%d frames: exact-match=%d/%d (%.1f%%), mean|err|=%d" %
      (d, match, cnt, 100.0 * match / cnt, err // max(cnt, 1)))
# level check on the aligned windows
def level(xs):
    p = sum(x * x for x in xs) / len(xs)
    return 20 * math.log10(math.sqrt(p) / (1 << 23)) if p > 0 else -200
print("OUT level=%.2f dBFS  IN level=%.2f dBFS" % (level(o), level(i)))
