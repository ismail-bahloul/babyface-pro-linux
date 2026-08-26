#!/usr/bin/env python3
"""lbcap_timeline.py — full timeline of cap_lbcal: master 16-bit value (held
from the writes), OUT ep 0x100 word0 440 Hz level, IN ep 0x8200 word0 440 Hz
level, per second."""
import struct, sys, math, collections

path = sys.argv[1]
data = open(path, 'rb').read()
off = 24
t0 = None
writes = []       # (t, breq, wval, widx)
out_s = collections.defaultdict(list)  # sec -> samples
in_s = collections.defaultdict(list)
while off + 16 <= len(data):
    ts_sec, ts_usec, cap_len, orig_len = struct.unpack_from('<IIII', data, off)
    off += 16
    pkt = data[off:off + cap_len]
    off += cap_len
    if cap_len < 36:
        continue
    ep = struct.unpack_from('<H', pkt, 20)[0]
    # USBPcap header_len (u16@0): 28 for control records, 27 for audio
    # URBs — payload at pkt[hlen:], setup follows the 28-byte ctrl
    # header (fixed 2026-08-26: pkt[28:] misaligned the audio frames).
    hlen = struct.unpack_from('<H', pkt, 0)[0]
    payload = pkt[hlen:]
    t = ts_sec + ts_usec / 1e6
    if t0 is None:
        t0 = t
    t -= t0
    if ep == 0x0000:
        setup = pkt[28:36]
        if setup[0] == 0x40:
            breq, wval, widx, wlen = struct.unpack_from('<BHHH', setup, 1)
            if wlen == 0:
                writes.append((t, breq, wval, widx))
    elif ep in (0x0100, 0x8200) and len(payload) >= 64:
        dst = out_s if ep == 0x0100 else in_s
        for k in range(0, len(payload) - 63, 64):
            w0 = struct.unpack_from('<I', payload, k)[0]
            s = (w0 >> 8) & 0xffffff
            if s & 0x800000:
                s -= 1 << 24
            dst[int(t)].append(s)

def l440(samples):
    if len(samples) < 1000:
        return None
    win = min(48000, len(samples))
    cw = 2 * math.cos(2 * math.pi * 440 / 48000)
    s0 = s1 = 0.0
    for x in samples[:win]:
        tt = x + cw * s0 - s1
        s1, s0 = s0, tt
    p = s1 * s1 + s0 * s0 - cw * s1 * s0
    amp = math.sqrt(p * 2) / win
    return 20 * math.log10(amp / (1 << 23)) if amp > 0 else -200

print("%5s %8s | %8s %8s" % ("t", "master", "OUT", "IN"))
master = None
for sec in range(0, 35):
    for t, breq, wval, widx in writes:
        if int(t) == sec and breq == 0x12 and widx in (0x03e0, 0x83e0, 0xc3e0, 0x43e0):
            master = wval
    o = l440(out_s.get(sec, []))
    i = l440(in_s.get(sec, []))
    print("%5d %#8x | %8s %8s" % (sec, master or 0,
                                   ("%.1f" % o) if o else "",
                                   ("%.1f" % i) if i else ""))
