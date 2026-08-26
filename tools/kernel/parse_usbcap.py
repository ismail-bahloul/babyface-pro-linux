#!/usr/bin/env python3
"""parse_usbcap.py — extract vendor control transfers from a USBPcap pcap.
Usage: parse_usbcap.py <file.pcap> [min_time]
Prints: t=... req=0x.. val=0x.... idx=0x.... (dir=out/in)

The USBPcap pseudoheader is 28 bytes; the 8-byte USB setup packet follows
it (corrected 2026-08-26 — the earlier 27-byte offset mis-parsed every
setup).  Vendor OUT transfers: bmRequestType = 0x40.
"""
import struct, sys

def main():
    path = sys.argv[1]
    t0 = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
    data = open(path, 'rb').read()
    # pcap global header
    if data[:4] != b'\xd4\xc3\xb2\xa1' and data[:4] != b'\xa1\xb2\xc3\xd4':
        sys.exit('not a little-endian pcap')
    off = 24
    n = 0
    while off + 16 <= len(data):
        ts_sec, ts_usec, cap_len, orig_len = struct.unpack_from('<IIII', data, off)
        off += 16
        pkt = data[off:off + cap_len]
        off += cap_len
        t = ts_sec + ts_usec / 1e6
        # USBPcap pseudoheader: 28 bytes (irp 8, status 4, function 2,
        # info 2, bus 2, device 2, endpoint 2, xfer 1, pad 1, len 4); the
        # 8-byte USB setup packet follows.
        if cap_len < 36:
            continue
        ep = struct.unpack_from('<H', pkt, 20)[0]
        if ep != 0x0000:  # control transfers live on the default pipe
            continue
        setup = pkt[28:36]
        bm, breq, wval, widx, wlen = struct.unpack_from('<BBHHH', setup, 0)
        if bm == 0x40:
            d = 'out'
        elif bm == 0xc0:
            d = 'in '
        else:
            continue
        n += 1
        if t >= t0:
            print('t=%9.4f  %s req=0x%02x val=0x%04x idx=0x%04x len=%d' %
                  (t, d, breq, wval, widx, wlen))
    print('--- total vendor transfers: %d ---' % n)

if __name__ == '__main__':
    main()
