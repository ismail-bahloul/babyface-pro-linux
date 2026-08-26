#!/usr/bin/env python3
"""hlen_check.py — report the USBPcap header_len field (u16@0) for
control, OUT-audio and IN-audio URBs in a capture."""
import struct, sys
from collections import Counter

data = open(sys.argv[1], 'rb').read()
off = 24
c = Counter()
while off + 16 <= len(data):
    ts, us, cl, o = struct.unpack_from('<IIII', data, off)
    off += 16
    pkt = data[off:off + cl]
    off += cl
    if cl < 36:
        continue
    hlen = struct.unpack_from('<H', pkt, 0)[0]
    ep = struct.unpack_from('<H', pkt, 20)[0]
    if ep == 0x0000:
        kind = 'ctrl'
    elif ep == 0x0100:
        kind = 'out-audio'
    elif ep == 0x8200:
        kind = 'in-audio'
    else:
        continue
    c[(kind, hlen, cl)] += 1
for (kind, hlen, cl), n in sorted(c.items()):
    print(f'{kind:10s} hlen={hlen} cap_len={cl} x{n}')
