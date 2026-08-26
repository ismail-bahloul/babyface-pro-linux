#!/usr/bin/env python3
"""vendor_writes.py — group the vendor control writes of a USBPcap file by
(req, register), with the distinct values each register saw.

Usage: vendor_writes.py <file.pcap>
Useful to see at a glance what a TotalMix session actually wrote (e.g.
cap_lbcal.pcap = the 30-ch 0x15 loopback map + the AN1/2 master sweep
only).  Requires the USBPcap v1.5 28-byte pseudoheader.
"""
import struct, sys, collections

path = sys.argv[1]
data = open(path, 'rb').read()
off = 24
t0 = None
writes = []
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
        continue  # skip data-phase control (rare)
    t = ts_sec + ts_usec / 1e6
    if t0 is None:
        t0 = t
    writes.append((t - t0, breq, wval, widx))
print("total vendor writes:", len(writes))

# Group by (breq, idx & 0x03ff) — the low bits of the register
groups = collections.defaultdict(list)
for t, breq, wval, widx in writes:
    groups[(breq, widx & 0x03ff)].append((t, wval, widx))

for (breq, reg), lst in sorted(groups.items(), key=lambda x: (x[0][0], x[0][1])):
    vals = [w for _, w, _ in lst]
    uniq = sorted(set(vals))
    print("req=0x%02x idx=0x%04x : %4d writes  vals=%s%s" %
          (breq, reg, len(lst),
           " ".join("%04x" % v for v in uniq[:8]),
           "..." if len(uniq) > 8 else ""))
