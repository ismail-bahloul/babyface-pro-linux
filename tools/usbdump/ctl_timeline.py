#!/usr/bin/env python3
"""Dump the vendor control writes of a USBPcap file chronologically
with relative timestamps (28-byte pseudoheader).  Usage:
    ctl_timeline.py <file.pcap> [max_writes]
"""
import struct, sys

path = sys.argv[1]
maxw = int(sys.argv[2]) if len(sys.argv) > 2 else 0
data = open(path, 'rb').read()
off = 24
t0 = None
n = 0
out = []
while off + 16 <= len(data):
    ts_sec, ts_usec, cap_len, orig_len = struct.unpack_from('<IIII', data, off)
    off += 16
    pkt = data[off:off + cap_len]
    off += cap_len
    if cap_len < 36:
        continue
    ep = struct.unpack_from('<H', pkt, 20)[0]
    if ep != 0x0000:
        continue
    setup = pkt[28:36]
    if setup[0] != 0x40:
        continue
    breq, wval, widx, wlen = struct.unpack_from('<BHHH', setup, 1)
    if wlen != 0:
        continue
    t = ts_sec + ts_usec / 1e6
    if t0 is None:
        t0 = t
    out.append((t - t0, breq, wval, widx))
    n += 1
    if maxw and n >= maxw:
        break

for t, breq, wval, widx in out:
    print("%8.4f  req=0x%02x wVal=0x%04x wIdx=0x%04x" % (t, breq, wval, widx))
print("total:", n)
