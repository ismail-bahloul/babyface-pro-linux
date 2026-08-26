#!/usr/bin/env python3
"""frame_dump.py — dump consecutive IN-stream frames (ch2/ch3 words) from a
USBPcap capture to verify the 24-bit sample byte position."""
import struct, sys

data = open(sys.argv[1], 'rb').read()
off = 24
found = 0
while off + 16 <= len(data) and found < 2:
    ts, us, cl, o = struct.unpack_from('<IIII', data, off)
    off += 16
    pkt = data[off:off + cl]
    off += cl
    if cl < 36:
        continue
    ep = struct.unpack_from('<H', pkt, 20)[0]
    if ep == 0x8200 and len(pkt) >= 27 + 56 * 10:
        # payload at pkt[27:] — USBPcap header_len = 27 for audio URBs
        # (28 for control records; pkt[28:] misaligned these frames).
        payload = pkt[27:]
        print(f'IN pkt plen={len(payload)}')
        for fr in range(10):
            base = fr * 56
            c2 = payload[base + 8:base + 12]
            c3 = payload[base + 12:base + 16]
            w2 = struct.unpack_from('<I', payload, base + 8)[0]
            w3 = struct.unpack_from('<I', payload, base + 12)[0]
            b13_2 = (w2 >> 8) & 0xFFFFFF
            b13_3 = (w3 >> 8) & 0xFFFFFF
            b02_2 = w2 & 0xFFFFFF
            b02_3 = w3 & 0xFFFFFF
            print(f'  fr{fr}: ch2={c2.hex(" ")} ch3={c3.hex(" ")} | '
                  f'b13 {b13_2:06X}/{b13_3:06X} b02 {b02_2:06X}/{b02_3:06X}')
        found += 1
