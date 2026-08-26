#!/usr/bin/env python3
"""align.py — find the frame-grid offset in a big IN audio URB: try
payload offsets 0/27/28 and report the word-0 counters per 56-B frame."""
import struct, sys

data = open(sys.argv[1], 'rb').read()
off = 24
while off + 16 <= len(data):
    ts, us, cl, o = struct.unpack_from('<IIII', data, off)
    off += 16
    pkt = data[off:off + cl]
    off += cl
    if cl < 36:
        continue
    ep = struct.unpack_from('<H', pkt, 20)[0]
    if ep == 0x8200 and len(pkt) > 1000:
        hlen = struct.unpack_from('<H', pkt, 0)[0]
        print(f'big IN URB: cap_len={cl} hlen={hlen}')
        for base in (0, 27, 28, 26, 25):
            p = pkt[base:]
            n = len(p) // 56
            c0 = [struct.unpack_from('<I', p, f * 56)[0] for f in range(min(8, n))]
            print(f'  offset {base}: {n} frames, word0: {[hex(c) for c in c0]}')
        break
