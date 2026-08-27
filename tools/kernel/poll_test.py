#!/usr/bin/env python3
# requires /tmp/usbwrite (build: gcc usbwrite.c -o /tmp/usbwrite)
"""poll_test.py — replicate TotalMix's status polling: the 5-read cycle
(0x1c, 0x1e, 0x1f, 0x17, 0x11) at ~50 cycles/s, for `secs` seconds.
Usage: poll_test.py <bus/dev> <secs>"""
import subprocess, sys, time

dev = sys.argv[1]
secs = float(sys.argv[2])

def r(req):
    subprocess.run(['sudo', '-n', '/tmp/usbwrite', dev, 'r',
                    '%02x' % req, '0000'], capture_output=True)

t0 = time.monotonic()
n = 0
while time.monotonic() - t0 < secs:
    for req in (0x1c, 0x1e, 0x1f, 0x17, 0x11):
        r(req)
    n += 1
    if n % 100 == 0:
        print('cycles %d @ %.1fs' % (n, time.monotonic() - t0))
print('done: %d cycles in %.1fs (%.0f/s)' % (n, time.monotonic() - t0, n / (time.monotonic() - t0)))
