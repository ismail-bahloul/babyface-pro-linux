#!/usr/bin/env python3
"""Streaming USBPcap parser: group control writes by (req, wIdx).

Usage: reg_hist.py file.pcap [first_rec] [last_rec] [--req 0x12]
Streams the file (no full load) — safe for multi-hundred-MB captures.
"""
import struct, sys

def iter_records(f):
    hdr = f.read(24)
    assert len(hdr) == 24 and struct.unpack_from('<I', hdr, 0)[0] == 0xA1B2C3D4, 'not LE pcap'
    rec = 0
    while True:
        h = f.read(16)
        if len(h) < 16:
            return
        ts_sec, ts_usec, incl, orig = struct.unpack_from('<IIII', h, 0)
        d = f.read(incl)
        if len(d) < incl:
            return
        rec += 1
        yield rec, ts_sec + ts_usec / 1e6, d

def parse(fn, lo, hi, reqs_filter):
    counts = {}
    with open(fn, 'rb') as f:
        for rec, _t, d in iter_records(f):
            if rec < lo:
                continue
            if rec > hi:
                break
            if len(d) < 27:
                continue
            hlen = struct.unpack_from('<H', d, 0)[0]
            func = struct.unpack_from('<H', d, 14)[0]
            if func != 0x17:
                continue
            if len(d) > 27 and d[27] != 0:
                continue
            info = d[16]
            if (info & 0x01):
                continue
            pay = max(hlen, 27)
            if pay + 8 > len(d):
                continue
            bm, req = d[pay], d[pay + 1]
            if bm != 0x40:
                continue
            if reqs_filter and req not in reqs_filter:
                continue
            wv, wi, wl = struct.unpack_from('<HHH', d, pay + 2)
            key = (req, wi)
            counts[key] = counts.get(key, 0) + 1
    for (req, wi), n in sorted(counts.items()):
        print(f'req=0x{req:02x} wIdx=0x{wi:04x}  x{n}')

if __name__ == '__main__':
    fn = sys.argv[1]
    lo = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    hi = int(sys.argv[3]) if len(sys.argv) > 3 else 10**9
    filter_args = [a for a in sys.argv[4:] if a.startswith('--req')]
    reqs = None
    if filter_args:
        reqs = set(int(a.split()[-1], 16) for a in filter_args)
    parse(fn, lo, hi, reqs)
