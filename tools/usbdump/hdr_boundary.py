#!/usr/bin/env python3
"""hdr_boundary.py — dump the USBPcap record bytes 18-40 of the first
IN audio URB to find the exact payload offset."""
import struct, sys

data = open(sys.argv[1], 'rb').read()
off = 24
shown = 0
while off + 16 <= len(data) and shown < 2:
    ts, us, cl, o = struct.unpack_from('<IIII', data, off)
    off += 16
    pkt = data[off:off + cl]
    off += cl
    if cl < 36:
        continue
    ep = struct.unpack_from('<H', pkt, 20)[0]
    if ep == 0x8200:
        hlen = struct.unpack_from('<H', pkt, 0)[0]
        print(f'ep=0x{ep:04X} hlen(u16@0)={hlen} cap_len={cl} pkt len={len(pkt)}')
        print('  pkt[16:44]: ' + ' '.join(f'{x:02x}' for x in pkt[16:44]))
        print('  pkt[26:40] as u32 words:', [hex(struct.unpack_from('<I', pkt, i)[0]) for i in range(26, 42, 4)])
        shown += 1
