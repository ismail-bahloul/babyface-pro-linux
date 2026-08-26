#!/usr/bin/env python3
"""Minimal USBPcap parser (linktype 249) — dump control transfers with
their data + URB submit/complete events.

Format per ParsePcap.cs / Wireshark packet-usb.c:
  u16 header_len @0; u64 irp_id @2; u32 usbd_status @10;
  u16 function @14 (0x17 vendor, 0x09 bulk/intr, 0x0a iso, 0xfe irp);
  u8 info @16 (bit0: 0=host->dev, 1=dev->host);
  u16 bus @17; u16 dev @19; u8 ep @21; u8 transfer_type @22;
  u32 data_len @23; [u8 control_stage @27]; payload at header_len(>=27).

Usage: parse_usb.py file.pcap [first_rec] [last_rec] [--ctrl] [--urbs]
"""
import struct, sys

def parse(fn, lo, hi, ctrl_only, show_urbs):
    b = open(fn, 'rb').read()
    magic = struct.unpack_from('<I', b, 0)[0]
    assert magic == 0xA1B2C3D4, 'not LE pcap'
    lt = struct.unpack_from('<I', b, 20)[0]
    print(f'# pcap linktype={lt} (USBPcap)' if lt == 249 else f'linktype={lt}')
    pos = 24
    rec = 0
    funcs = {0x17: 'CTL', 0x09: 'BULK/INTR', 0x0a: 'ISO', 0xfe: 'IRP'}
    while pos + 16 <= len(b):
        ts_sec, ts_usec, incl, _ = struct.unpack_from('<IIII', b, pos)
        pos += 16
        rec += 1
        if rec < lo: pos += incl; continue
        if rec > hi: break
        if pos + incl > len(b): break
        d = b[pos:pos + incl]
        pos += incl
        if len(d) < 27: continue
        hlen = struct.unpack_from('<H', d, 0)[0]
        func = struct.unpack_from('<H', d, 14)[0]
        info = d[16]
        ep = d[21]
        ttype = d[22]
        dlen = struct.unpack_from('<I', d, 23)[0]
        stage = ''
        if func == 0x17 and len(d) > 27:
            stage = ['SETUP', 'DATA', 'STATUS', 'COMPLETE'][d[27]] if d[27] < 4 else '?'
        pay = max(hlen, 27)
        line = f'#{rec} t={ts_sec}.{ts_usec:06d} {funcs.get(func, hex(func))} ' \
               f'ep=0x{ep:02x} {"IN" if ep & 0x80 else "OUT"} type={ttype} len={dlen}'
        if func == 0x17 and stage == 'SETUP' and pay + 8 <= len(d):
            bm, req = d[pay], d[pay + 1]
            wv, wi, wl = struct.unpack_from('<HHH', d, pay + 2)
            if ctrl_only or (bm & 0x80) == 0:
                print(f'{line} bm=0x{bm:02x} req=0x{req:02x} wVal=0x{wv:04x} wIdx=0x{wi:04x} wLen={wl}')
            elif (bm & 0x80) and dlen and stage == 'SETUP':
                print(f'{line} bm=0x{bm:02x} req=0x{req:02x} wVal=0x{wv:04x} wIdx=0x{wi:04x} wLen={wl}')
        if func == 0x17 and stage == 'COMPLETE' and (ep & 0x80) and dlen and pay < len(d):
            print(f'{line} DATA={d[pay:pay + dlen].hex(" ")}')
        elif func == 0x8 and (ep & 0x80) and dlen and pay < len(d):
            print(f'{line} DATA={d[pay:pay + dlen].hex(" ")}')
        elif func == 0x8 and (ep & 0x80) and dlen:
            print(f'{line} (data missing, dlen={dlen})')
        elif func == 0xb and (ep & 0x80) and dlen and pay < len(d):
            print(f'{line} DATA={d[pay:pay + dlen].hex(" ")}')
        elif show_urbs and (func != 0x17):
            label = 'SUBMIT' if info & 1 == 0 else 'DONE'
            print(f'{line} [{label}]')

if __name__ == '__main__':
    fn = sys.argv[1]
    lo = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    hi = int(sys.argv[3]) if len(sys.argv) > 3 else 10**9
    ctrl_only = '--ctrl' in sys.argv
    urbs = '--urbs' in sys.argv
    parse(fn, lo, hi, ctrl_only, urbs)
