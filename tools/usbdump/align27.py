#!/usr/bin/env python3
"""align27.py — dump the first frame at offset 27 (the real 256-frame
grid) as 14 u32 words, from the first big IN URB."""
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
        p = pkt[27:]
        for fr in range(2):
            print(f'frame {fr} @27, 14 words:')
            for wi in range(14):
                w = struct.unpack_from('<I', p, fr * 56 + wi * 4)[0]
                b = p[fr * 56 + wi * 4:fr * 56 + wi * 4 + 4]
                print(f'  w{wi:2d}: {w:08X}  ({b.hex(" ")})')
        break
