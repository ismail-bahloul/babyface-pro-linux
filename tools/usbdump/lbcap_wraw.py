#!/usr/bin/env python3
"""lbcap_wraw.py — dump the RAW bytes of the Windows IN loopback words (w2/3,
Phones bus in cap_lbph) to compare the exact byte layout with the Linux dump."""
import struct, sys

path = sys.argv[1]
data = open(path, 'rb').read()
off = 24
shown = 0
while off + 16 <= len(data) and shown < 4:
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
    if n < 1:
        continue
    fr = payload[:56]
    w0 = fr[0:4]
    w1 = fr[4:8]
    w2 = fr[8:12]
    w3 = fr[12:16]
    print("win frame: w0=%s w1=%s w2=%s w3=%s" %
          (w0.hex(), w1.hex(), w2.hex(), w3.hex()))
    print("          w2 >>8 = %d, w2 as bytes0-2 = %d, w3 >>8 = %d" %
          (struct.unpack_from('<I', w2, 0)[0] >> 8,
           (w2[0] | w2[1] << 8 | w2[2] << 16),
           struct.unpack_from('<I', w3, 0)[0] >> 8))
    shown += 1
