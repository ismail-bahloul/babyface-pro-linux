#!/usr/bin/env python3
"""Extract the trim-related 0x12 writes from a capture.

The linked AN1/2 strip trim writes EIGHT registers together:
  low map    0x0000/0x001A (AN1 L/R) + 0x0001/0x001B (AN2 L/R)
  standard   0x0034/0x004E (AN1 L/R) + 0x0035/0x004F (AN2 L/R)

Usage: trim_extract.py file.pcap [first_rec] [last_rec]
Prints only 0x12 OUT writes on those registers, one line per write:
  #rec wIdx=0xXXXX wVal=0xXXXX
"""
import struct, sys

TRIM = {0x0000, 0x001a, 0x0001, 0x001b, 0x0034, 0x004e, 0x0035, 0x004f}
FLAG = 0x3fff  # mask off the 0xC000/0x4000/0x8000 transaction flag

def parse(fn, lo, hi):
    b = open(fn, 'rb').read()
    assert struct.unpack_from('<I', b, 0)[0] == 0xA1B2C3D4, 'not LE pcap'
    pos = 24
    rec = 0
    while pos + 16 <= len(b):
        _, _, incl, _ = struct.unpack_from('<IIII', b, pos)
        pos += 16
        rec += 1
        if rec < lo:
            pos += incl
            continue
        if rec > hi:
            break
        if pos + incl > len(b):
            break
        d = b[pos:pos + incl]
        pos += incl
        if len(d) < 27:
            continue
        hlen = struct.unpack_from('<H', d, 0)[0]
        func = struct.unpack_from('<H', d, 14)[0]
        if func != 0x17:
            continue
        if len(d) > 27 and d[27] != 0:  # SETUP stage only
            continue
        info = d[16]
        if (info & 0x01) or not (d[21] == 0 or d[21] & 0x80):  # host->dev
            continue
        pay = max(hlen, 27)
        if pay + 8 > len(d):
            continue
        bm, req = d[pay], d[pay + 1]
        wv, wi, wl = struct.unpack_from('<HHH', d, pay + 2)
        if bm == 0x40 and req == 0x12 and (wi & FLAG) in TRIM:
            print(f'#{rec} wIdx=0x{wi:04x} wVal=0x{wv:04x}')

if __name__ == '__main__':
    fn = sys.argv[1]
    lo = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    hi = int(sys.argv[3]) if len(sys.argv) > 3 else 10**9
    parse(fn, lo, hi)
