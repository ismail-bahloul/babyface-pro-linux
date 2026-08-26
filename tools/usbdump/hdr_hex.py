#!/usr/bin/env python3
"""hdr_hex.py — dump the first 40 bytes of one ctrl, one OUT-audio and
one IN-audio USBPcap record, to explain the 27-vs-28 byte header."""
import struct, sys

data = open(sys.argv[1], 'rb').read()
off = 24
seen = {}
while off + 16 <= len(data):
    ts, us, cl, o = struct.unpack_from('<IIII', data, off)
    off += 16
    pkt = data[off:off + cl]
    off += cl
    if cl < 36:
        continue
    ep = struct.unpack_from('<H', pkt, 20)[0]
    if ep == 0x0000:
        k = 'ctrl'
    elif ep == 0x0100:
        k = 'out-audio'
    elif ep == 0x8200:
        k = 'in-audio'
    else:
        continue
    if k in seen:
        continue
    seen[k] = pkt[:40]
    print(f"--- {k} cap_len={cl} ---")
    print(' '.join(f'{x:02x}' for x in pkt[:40]))
    for i, name in [(0, 'hlen(u16@0)'), (20, 'ep(u16@20)'), (22, 'xfertype@22'),
                    (23, 'pad@23'), (24, 'len(u32@24)')]:
        if i + 2 <= len(pkt):
            if name.startswith('len'):
                print(f'  {name} = {struct.unpack_from("<I", pkt, i)[0]}')
            elif name.startswith('ep'):
                print(f'  {name} = {struct.unpack_from("<H", pkt, i)[0]:#06x}')
            elif name.startswith('hlen'):
                print(f'  {name} = {struct.unpack_from("<H", pkt, i)[0]}')
            else:
                print(f'  {name} = {pkt[i]:#04x}')
    if len(seen) == 3:
        break
