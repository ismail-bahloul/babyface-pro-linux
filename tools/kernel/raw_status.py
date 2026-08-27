#!/usr/bin/env python3
"""raw_status.py — log the 0x17/0x1C/0x1E/0x1F status registers at high rate.

Reads each register via usbfs in a tight loop and prints every CHANGE
with a millisecond timestamp, to correlate the panel SELECTION state
(which the 0x17/0x0000 panel readback does not expose) with any other
readable byte.

Usage: raw_status.py [bus/dev]      (default 003/002)
"""
import sys, time, os, ctypes

DEV = sys.argv[1] if len(sys.argv) > 1 else "003/002"
fd = os.open(f"/dev/bus/usb/{DEV}", os.O_RDWR)
libc = ctypes.CDLL(None, use_errno=True)
USBDEVFS_CTRL = 0xC0185500
USB_DIR_IN = 0x80
USB_TYPE_VENDOR = 0x40
USB_RECIP_DEVICE = 0x00

class Ctrl(ctypes.Structure):
    _fields_ = [("bRequestType", ctypes.c_uint8), ("bRequest", ctypes.c_uint8),
                ("wValue", ctypes.c_uint16), ("wIndex", ctypes.c_uint16),
                ("wLength", ctypes.c_uint16), ("timeout", ctypes.c_uint32),
                ("data", ctypes.c_void_p)]

# (req, idx) pairs to poll: 0x17 panel, 0x1C status, 0x1E status, 0x1F status
REGS = [(0x17, 0x0000), (0x1C, 0x0000), (0x1E, 0x0000), (0x1F, 0x0000)]
N = len(REGS)
data = [(ctypes.c_ubyte * 4)() for _ in range(N)]
ctrls = []
for req, idx in REGS:
    c = Ctrl(USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE, req, 0, idx, 4, 1000,
             ctypes.cast(data[len(ctrls)], ctypes.c_void_p))
    ctrls.append(c)

prev = [None] * N
t0 = time.time()
while True:
    for i in range(N):
        r = libc.ioctl(fd, USBDEVFS_CTRL, ctypes.byref(ctrls[i]))
        if r < 0:
            continue
        b = bytes(data[i])
        if b != prev[i]:
            print(f"+{1000*(time.time()-t0):7.0f} ms  0x{REGS[i][0]:02x}: {b[0]:02x} {b[1]:02x} {b[2]:02x} {b[3]:02x}", flush=True)
            prev[i] = b
