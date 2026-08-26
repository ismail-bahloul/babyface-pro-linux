#!/usr/bin/env python3
"""frame_full.py — dump the full first IN-frame (64 bytes) + the next 2
frames from a USBPcap capture, raw bytes."""
import struct, sys

data = open(sys.argv[1], 'rb').read()
off = 24
shown = 0
while off + 16 <= len(data) and shown < 1:
    ts, us, cl, o = struct.unpack_from('<IIII', data, off)
    off += 16
    pkt = data[off:off + cl]
    off += cl
    if cl < 36:
        continue
    ep = struct.unpack_from('<H', pkt, 20)[0]
    if ep == 0x8200 and len(pkt) >= 27 + 56 * 3:
        # payload at pkt[27:] — USBPcap header_len = 27 for audio URBs
        # (28 for control records; pkt[28:] misaligned these frames).
        payload = pkt[27:]
        for fr in range(3):
            base = fr * 56
            print(f'frame {fr} bytes 0-55:')
            for row in range(4):
                b = payload[base + row * 14:base + row * 14 + 14]
                print('   ' + ' '.join(f'{x:02x}' for x in b))
        shown += 1
