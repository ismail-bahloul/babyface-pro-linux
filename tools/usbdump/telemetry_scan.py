#!/usr/bin/env python3
"""Scan the 480-B telemetry (ep 0x85) in a capture for fields that
correlate with audio activity: extract every 480-B payload, compare the
first (idle) vs mid-stream (active) sections, and list the words that
change most."""
import struct, sys

def telemetry(fn):
    b = open(fn, 'rb').read()
    pos = 24
    rec = 0
    out = []
    while pos + 16 <= len(b):
        _, _, incl, _ = struct.unpack_from('<IIII', b, pos)
        pos += 16
        rec += 1
        if pos + incl > len(b): break
        d = b[pos:pos + incl]
        pos += incl
        if len(d) < 27: continue
        hlen = struct.unpack_from('<H', d, 0)[0]
        ep = d[21]
        if ep != 0x85: continue
        pay = max(hlen, 27)
        avail = len(d) - pay
        if avail < 480: continue
        out.append((rec, d[pay:pay + 480]))
    return out

def main():
    fn = sys.argv[1]
    tele = telemetry(fn)
    print(f'{len(tele)} telemetry reads')
    if not tele: return
    # idle section: first 10 reads; active: reads after the audio starts
    # (assume the second half of the capture has the session running).
    idle = tele[:10]
    active = tele[len(tele)//2: len(tele)//2 + 10]
    # per-word (u32) stats
    def words(reads):
        return [[struct.unpack_from('<I', p, w * 4)[0] for w in range(120)] for _, p in reads]
    wi, wa = words(idle), words(active)
    print('\nword  idle(mean)      active(mean)    change')
    for w in range(120):
        mi = sum(r[w] for r in wi) / len(wi)
        ma = sum(r[w] for r in wa) / len(wa)
        if abs(ma - mi) > 0:
            print(f'  {w:3d}  {mi:12.1f}  {ma:12.1f}  d={ma-mi:12.1f}')
    # also dump the known ParsePcap.cs offsets
    print('\nknown offsets (m=0x00 c=0x04 v8=0x08 v10=0x10 v18=0x18 q60=0x60 qE0=0xE0 p140=0x140 p170=0x170 p1B0=0x1B0):')
    for off in (0x00, 0x04, 0x08, 0x10, 0x18, 0x60, 0xE0, 0x140, 0x170, 0x1B0):
        r = struct.unpack_from('<I', tele[len(tele)//2][1], off)
        print(f'  +0x{off:03X}: {r[0]:08X}')

if __name__ == '__main__':
    main()
