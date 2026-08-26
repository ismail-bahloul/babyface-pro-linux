#!/usr/bin/env python3
"""Extract the SELECT-cycle capture: 0x17 readback replies + interrupt
status stream + control writes, printing only CHANGES so the button
presses stand out. Usage: select_scan.py cap_select2.pcap
"""
import struct, sys

def parse(fn):
    b = open(fn, 'rb').read()
    pos = 24
    rec = 0
    last_frame = None
    frame = []
    last_write = None
    n = 0
    while pos + 16 <= len(b):
        _, _, incl, _ = struct.unpack_from('<IIII', b, pos)
        pos += 16
        rec += 1
        if pos + incl > len(b):
            break
        d = b[pos:pos + incl]
        pos += incl
        if len(d) < 27:
            continue
        hlen = struct.unpack_from('<H', d, 0)[0]
        func = struct.unpack_from('<H', d, 14)[0]
        info = d[16]
        ep = d[21]
        dlen = struct.unpack_from('<I', d, 23)[0]
        pay = max(hlen, 27)
        # 0x17 control reads: COMPLETE stage carries the 4-byte reply
        if func == 0x17 and len(d) > 27 and d[27] == 3 and (ep & 0x80) and dlen and pay < len(d):
            data = d[pay:pay + dlen]
            print(f'#R{rec} read0x17={data.hex(" ")}')
        # interrupt status stream (ep 0x82) — group the 5 quads into a frame
        elif func == 0x8 and (ep & 0x80) and dlen and pay < len(d):
            data = d[pay:pay + dlen]
            frame.append((rec, data))
            if len(frame) == 5:
                sig = tuple(q for _, q in frame)
                if sig != last_frame:
                    recs = ','.join(str(r) for r, _ in frame)
                    print(f'#S[{recs}] stat={" ".join(q.hex() for _, q in frame)}')
                    last_frame = sig
                frame = []
        # control writes (OUT, SETUP stage)
        elif func == 0x17 and len(d) > 27 and d[27] == 0 and not (info & 1):
            bm, req = d[pay], d[pay + 1]
            wv, wi, wl = struct.unpack_from('<HHH', d, pay + 2)
            if bm == 0x40 and wl == 0:
                sig = f'WR 0x{req:02x} wVal=0x{wv:04x} wIdx=0x{wi:04x}'
                if sig != last_write:
                    print(f'#{rec} {sig}')
                    last_write = sig
        n += 1

if __name__ == '__main__':
    parse(sys.argv[1])
