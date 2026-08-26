#!/usr/bin/env python3
"""Emit a C replay program from a USBPcap capture: every vendor OUT write
in order, plus optional 0x17 readbacks at named points.

Usage: gen_replay.py in.pcap out.c [--v17-after N ...]
"""
import sys, struct

def writes(fn):
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
        func = struct.unpack_from('<H', d, 14)[0]
        if func != 0x17: continue
        if len(d) <= 27: continue
        stage = d[27]
        pay = max(hlen, 27)
        if stage == 0 and pay + 8 <= len(d):
            bm, req = d[pay], d[pay + 1]
            if (bm & 0x80) == 0:
                wv, wi, wl = struct.unpack_from('<HHH', d, pay + 2)
                out.append((rec, req, wv, wi))
    return out

def main():
    fn, ofn = sys.argv[1], sys.argv[2]
    ws = writes(fn)
    lines = [
        '// Auto-generated replay of vendor writes from ' + fn,
        '#include <stdio.h>',
        '#include <string.h>',
        '#include <errno.h>',
        '#include <fcntl.h>',
        '#include <unistd.h>',
        '#include <sys/ioctl.h>',
        '#include <linux/usbdevice_fs.h>',
        '#define DEV "/dev/bus/usb/003/002"',
        'static int fd;',
        'static void ctl(int req, int val, int idx) {',
        '    struct usbdevfs_ctrltransfer c; memset(&c,0,sizeof(c));',
        '    c.bRequestType=0x40; c.bRequest=req; c.wValue=val; c.wIndex=idx; c.timeout=1000;',
        '    if (ioctl(fd, USBDEVFS_CONTROL, &c) < 0) printf("  ctl req=%02x errno=%d\\n", req, errno);',
        '}',
        'static void v17(const char *s) {',
        '    unsigned char b[4]={0}; struct usbdevfs_ctrltransfer c; memset(&c,0,sizeof(c));',
        '    c.bRequestType=0xC0; c.bRequest=0x17; c.wLength=4; c.timeout=1000; c.data=b;',
        '    if (ioctl(fd, USBDEVFS_CONTROL, &c) == 4)',
        '        printf("  %-40s 0x17=%02X %02X %02X %02X\\n", s, b[0], b[1], b[2], b[3]);',
        '}',
        'int main(void) {',
        '    fd = open(DEV, O_RDWR);',
        '    int iface = 5; ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface);',
        '    struct usbdevfs_setinterface si = {5, 1}; ioctl(fd, USBDEVFS_SETINTERFACE, &si);',
        '    v17("start");',
    ]
    for rec, req, wv, wi in ws:
        lines.append(f'    ctl(0x{req:02x}, 0x{wv:04x}, 0x{wi:04x}); /* #{rec} */')
    lines += [
        '    v17("after full replay");',
        '    close(fd); return 0;',
        '}',
    ]
    open(ofn, 'w').write('\n'.join(lines) + '\n')
    print(f'{len(ws)} writes -> {ofn}')

if __name__ == '__main__':
    main()
