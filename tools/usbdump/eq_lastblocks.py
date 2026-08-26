#!/usr/bin/env python3
"""Show the last DISTINCT 64-byte EQ blocks of a pcap (the settled 2-band state)."""
import struct, sys
sys.path.insert(0, '.')
from eq_extract import eq_blocks

path = sys.argv[1] if len(sys.argv) > 1 else "cap_eq8e.pcap"
blocks = eq_blocks(path)
seen = {}
for t, pl in blocks:
    key = pl.hex()
    seen.setdefault(key, t)
# last 6 distinct by time
order = sorted(seen.items(), key=lambda kv: kv[1])[-6:]
for hexs, t in order:
    pl = bytes.fromhex(hexs)
    hdr = pl[0:16]
    print(f"t={t:.3f} hdr={hdr.hex(' ')}")
    for off in (0x04, 0x14, 0x24, 0x34, 0x38):
        w = struct.unpack_from("<i", pl, off)[0]
        print(f"   0x{off:02X}: {w:08X} ({w/(1<<27):+.6f})")
