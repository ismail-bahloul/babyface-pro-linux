#!/usr/bin/env python3
"""Dump the first frames of an audio URB to nail the sample format."""
import struct, sys
sys.path.insert(0, '.')
from eq_extract import read_records

path = sys.argv[1] if len(sys.argv) > 1 else "cap_fx_live.pcap"
ep = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x01
shown = 0
for ts_sec, ts_usec, fr in read_records(path):
    if len(fr) < 27:
        continue
    hlen = struct.unpack_from("<H", fr, 0)[0]
    endpoint = fr[0x15]
    plen = struct.unpack_from("<I", fr, 0x17)[0]
    if endpoint == ep and plen >= 1000:
        pl = fr[hlen:hlen+plen]
        n = len(pl) // 56
        print(f"ep=0x{ep:02X} urb {plen} B, {n} frames, frame0 + first 3 frames:")
        for f in range(3):
            base = f * 56
            print(f"  frame {f}:", pl[base:base+56].hex(' '))
        break
