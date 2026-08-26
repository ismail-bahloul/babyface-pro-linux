#!/usr/bin/env python3
"""kepalive_test.py — replicate TotalMix's streaming keepalive cycle:
trio (0x10 0x8000, 0x1D, 0x14 0xC000) -> 1.2s -> 0x13 0xC000 -> 0.25s -> ...
Usage: kepalive_test.py <bus/dev> <seconds>"""
import subprocess, sys, time

dev = sys.argv[1]
dur = float(sys.argv[2])

def w(req, val, idx):
    subprocess.run(['sudo', '-n', '/tmp/usbwrite', dev, 'w',
                    '%02x' % req, '%04x' % val, '%04x' % idx],
                   capture_output=True)

def trio():
    w(0x10, 0x0000, 0x8000)
    w(0x1d, 0x0000, 0x0000)
    w(0x14, 0x0000, 0xc000)

t0 = time.monotonic()
trio()
n = 0
while True:
    now = time.monotonic() - t0
    if now >= dur:
        break
    # 0x13 at 1.2s after the last trio, then next trio 0.25s later
    time.sleep(max(0.05, 1.2 - (time.monotonic() - t0 - n * 1.45)))
    if time.monotonic() - t0 >= dur:
        break
    w(0x13, 0x0000, 0xc000)
    time.sleep(0.25)
    if time.monotonic() - t0 >= dur:
        break
    trio()
    n += 1
    print('cycle %d @ %.1fs' % (n, time.monotonic() - t0))
print('done @ %.1fs' % (time.monotonic() - t0))
