#!/usr/bin/env python3
"""extract_controls.py — pull every control-transfer SETUP from a USBPcap
capture using tshark -V. Prints vendor requests in time order with frame
times (from a parallel tshark field pass)."""
import subprocess, re, sys

path = sys.argv[1]
verbose = subprocess.run(['tshark', '-r', path, '-V'],
                         capture_output=True, text=True).stdout
# frame number -> relative time
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

rows = []
for f in re.split(r'Frame Number: ', verbose)[1:]:
    m = re.match(r'(\d+)', f)
    if not m:
        continue
    fnum = int(m.group(1))
    if 'Control transfer stage: Setup (0)' not in f:
        continue
    bm = re.search(r'bmRequestType: 0x([0-9a-f]+)', f)
    br = re.search(r'bRequest: (\d+)', f)
    wv = re.search(r'wValue: 0x([0-9a-f]+)', f)
    wi = re.search(r'wIndex: (\d+) \(0x([0-9a-f]+)\)', f)
    wl = re.search(r'wLength: (\d+)', f)
    if not (bm and br and wv and wi):
        continue
    bmv = int(bm.group(1), 16)
    if (bmv >> 5) & 3 != 2:
        continue
    rows.append((times.get(fnum, -1), fnum,
                 'IN' if bmv & 0x80 else 'OUT',
                 int(br.group(1)), int(wv.group(1), 16),
                 int(wi.group(2), 16), int(wl.group(1)) if wl else 0))

rows.sort()
for r in rows:
    print('t=%9.4f f=%4d %s req=0x%02x val=0x%04x idx=0x%04x len=%d'
          % (r[0], r[1], r[2], r[3], r[4], r[5], r[6]))
print('--- %d vendor setups ---' % len(rows))
