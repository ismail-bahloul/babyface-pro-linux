#!/usr/bin/env python3
"""raw_panel.py — log the raw 0x17 panel readback (4 bytes) at high rate.

Reads 0x17 wIdx=0 via usbfs in a tight loop and prints every CHANGE
with a millisecond timestamp, so a too-quick SELECT flash that the
kernel's 50 Hz poll misses is still visible here.

Usage: raw_panel.py [bus/dev]      (default 003/002)
"""
import sys, time, os, fcntl, struct

DEV = sys.argv[1] if len(sys.argv) > 1 else "003/002"
path = f"/dev/bus/usb/{DEV}"
fd = os.open(path, os.O_RDWR)

USB_DIR_IN = 0x80
USB_TYPE_VENDOR = 0x40
USB_RECIP_DEVICE = 0x00
USBCFS_CTRL = struct.pack("BBHI", USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE, 0x17, 0x0000, 0x0000)

def read_panel():
    req = USBCFS_CTRL + struct.pack("I", 0)  # timeout
    # usbdevfs_ctrltransfer: bRequestType bRequest wValue wIndex wLength data timeout
    buf = bytearray(4)
    req = struct.pack("BBHI", USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE, 0x17, 0x0000, 4) + struct.pack("I", 0)
    try:
        n = fcntl.ioctl(fd, 0xC0185500, req + bytes(4))  # _IOWR('U', 0, struct usbdevfs_ctrltransfer)
        return buf
    except OSError:
        return None

# simpler: reuse usbwrite via subprocess? too slow. Use the raw ioctl properly.
import ctypes
libc = ctypes.CDLL(None, use_errno=True)
USBDEVFS_CTRL = 0xC0185500
class Ctrl(ctypes.Structure):
    _fields_ = [("bRequestType", ctypes.c_uint8), ("bRequest", ctypes.c_uint8),
                ("wValue", ctypes.c_uint16), ("wIndex", ctypes.c_uint16),
                ("wLength", ctypes.c_uint16), ("timeout", ctypes.c_uint32),
                ("data", ctypes.c_void_p)]
data = (ctypes.c_ubyte * 4)()
c = Ctrl(USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE, 0x17, 0x0000, 0x0000, 4, 1000, ctypes.cast(data, ctypes.c_void_p))
prev = None
t0 = time.time()
while True:
    r = libc.ioctl(fd, USBDEVFS_CTRL, ctypes.byref(c))
    if r < 0:
        time.sleep(0.005)
        continue
    b = bytes(data)
    if b != prev:
        print(f"+{1000*(time.time()-t0):7.0f} ms  {b[0]:02x} {b[1]:02x} {b[2]:02x} {b[3]:02x}", flush=True)
        prev = b
