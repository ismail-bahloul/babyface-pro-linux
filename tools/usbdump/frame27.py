#!/usr/bin/env python3
"""frame27.py — dump full IN-stream frames (all 14 u32 words) at the
CORRECT payload offset (hlen=27 for audio URBs), for cap_lbcal and
cap_lbph.  Word-by-word hex + word0/word1 raw u32 (counters?)."""
import struct, sys

data = open(sys.argv[1], 'rb').read()
off = 24
found = 0
while off + 16 <= len(data) and found < 3:
    ts, us, cl, o = struct.unpack_from('<IIII', data, off)
    off += 16
    pkt = data[off:off + cl]
    off += cl
    if cl < 36:
        continue
    ep = struct.unpack_from('<H', pkt, 20)[0]
    if ep == 0x8200 and len(pkt) >= 27 + 56 * 3:
        payload = pkt[27:]
        print(f'--- IN pkt (hlen=27) plen={len(payload)} words[0..13] frame0/1 ---')
        for fr in range(2):
            base = fr * 56
            words = [struct.unpack_from('<I', payload, base + w * 4)[0] for w in range(14)]
            print(f'  fr{fr}: ' + ' '.join(f'{w:08X}' for w in words))
        found += 1
