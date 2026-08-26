#!/usr/bin/env python3
"""lbcap_rawframes.py — dump the first IN frames of a Windows pcap (aligned
grid: payload at pkt[27:], 56-byte frames) to compare the word layout with the
Linux kernel driver's raw dump."""
import struct, sys

path = sys.argv[1]
data = open(path, 'rb').read()
off = 24
frames = []
while off + 16 <= len(data) and len(frames) < 4:
    ts_sec, ts_usec, cap_len, orig_len = struct.unpack_from('<IIII', data, off)
    off += 16
    pkt = data[off:off + cap_len]
    off += cap_len
    if cap_len < 36:
        continue
    ep = struct.unpack_from('<H', pkt, 20)[0]
    if ep != 0x8200:
        continue
    payload = pkt[27:]
    n = len(payload) // 56
    if n < 2:
        continue
    for f in range(2):
        fr = payload[f * 56:(f + 1) * 56]
        w = struct.unpack_from('<14I', fr, 0)
        s8 = [(v >> 8) & 0xFFFFFF for v in w]
        s8 = [x - 0x1000000 if x >= 0x800000 else x for x in s8]
        frames.append((s8, fr.hex()))
        if len(frames) >= 4:
            break
print("Windows IN frames (aligned grid, 56 B), sample = word >> 8:")
for i, (s8, hexs) in enumerate(frames):
    print("f%d: w0=%7d w1=%7d w2=%7d w3=%7d w4=%7d w5=%7d w12=%7d w13=%7d" %
          (i, s8[0], s8[1], s8[2], s8[3], s8[4], s8[5], s8[12], s8[13]))
