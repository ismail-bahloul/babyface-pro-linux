#!/usr/bin/env python3
"""readback_trace.py — dump the CONTROL read responses (0x17/0x11/...) over
time from a USBPcap capture. Uses tshark -V."""
import subprocess, re, sys

path = sys.argv[1]
verbose = subprocess.run(['tshark', '-r', path, '-V'],
                         capture_output=True, text=True).stdout
times = {}
tpass = subprocess.run(['tshark', '-r', path, '-T', 'fields',
                        '-e', 'frame.number', '-e', 'frame.time_relative'],
                       capture_output=True, text=True).stdout
for line in tpass.splitlines():
    p = line.split('\t')
    if len(p) == 2:
        try:
            times[int(p[0])] = float(p[1])
        except ValueError:
            pass

# walk frames; track pending requests (setup stage) and match the
# response (stage Complete with response data)
pending = {}  # frame number of setup -> (time, bRequest)
rows = []
for f in re.split(r'Frame Number: ', verbose)[1:]:
    m = re.match(r'(\d+)', f)
    if not m:
        continue
    fnum = int(m.group(1))
    stage = re.search(r'Control transfer stage: (\w+)', f)
    if not stage:
        continue
    st = stage.group(1)
    if st == 'Setup':
        bm = re.search(r'bmRequestType: 0x([0-9a-f]+)', f)
        br = re.search(r'bRequest: (\d+)', f)
        if bm and br and ((int(bm.group(1), 16) >> 5) & 3) == 2 \
           and (int(bm.group(1), 16) & 0x80):
            pending[fnum] = (times.get(fnum, -1), int(br.group(1)))
    elif st == 'Complete':
        rd = re.search(r'CONTROL response data: ([0-9a-f]+)', f)
        if rd and pending:
            # match the oldest pending
            k = min(pending)
            t, br = pending.pop(k)
            rows.append((t, br, rd.group(1)))

rows.sort()
for t, br, r in rows:
    if br in (23, 17, 30, 31, 28, 20):  # 0x17, 0x11, 0x1e, 0x1f, 0x1c, 0x14?
        print('t=%9.4f req=0x%02x resp=%s' % (t, br, r))
