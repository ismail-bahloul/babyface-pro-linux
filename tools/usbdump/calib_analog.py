#!/usr/bin/env python3
"""calib_analog.py — correlate the mic-gain + master writes of cap_calib with
the recorded IN levels (bulk ep 0x8000: ch0 = word 0 = AN1).  Determines
whether the ANALOG input record path carries a fixed staging like the
loopback tap.  Usage: calib_analog.py <cap_calib.pcap>
"""
import struct, sys, math, collections

path = sys.argv[1]
data = open(path, 'rb').read()
off = 24
t0 = None
ctl = []
samples_in = collections.defaultdict(list)
while off + 16 <= len(data):
    ts_sec, ts_usec, cap_len, orig_len = struct.unpack_from('<IIII', data, off)
    off += 16
    pkt = data[off:off + cap_len]
    off += cap_len
    if cap_len < 36:
        continue
    ep = struct.unpack_from('<H', pkt, 20)[0]
    # USBPcap header_len (u16@0): 28 for control records, 27 for audio
    # URBs — setup follows the 28-byte ctrl header, audio payload at
    # pkt[hlen:] (fixed 2026-08-26: pkt[28:] misaligned the IN frames).
    hlen = struct.unpack_from('<H', pkt, 0)[0]
    payload = pkt[hlen:]
    t = ts_sec + ts_usec / 1e6
    if t0 is None:
        t0 = t
    t -= t0
    if ep == 0x0000:
        setup = pkt[28:36]
        if setup[0] == 0x40:
            breq, wval, widx, wlen = struct.unpack_from('<BHHH', setup, 1)
            if wlen == 0:
                ctl.append((t, breq, wval, widx))
    elif ep in (0x8200, 0x8000) and len(payload) >= 64:
        step = 64
        for k in range(0, len(payload) - 63, step):
            w0 = struct.unpack_from('<I', payload, k)[0]
            s = (w0 >> 8) & 0xffffff
            if s & 0x800000:
                s -= 1 << 24
            samples_in[0].append((t, s))

def rms_db(stream, lo, hi):
    xs = [s for t, s in stream if lo <= t < hi]
    if not xs:
        return None
    p = sum(x * x for x in xs) / len(xs)
    return 20 * math.log10(math.sqrt(p) / (1 << 23)) if p > 0 else -200

print("control writes (mic gain 0x1a idx 0x0000, master, preamp):")
last = {}
for t, breq, wval, widx in ctl:
    if breq == 0x1a and widx == 0x0000:
        tag = "mic"
    elif breq == 0x1a and widx in (0x0004, 0x0005):
        tag = "mst8"
    elif breq == 0x12 and widx in (0x03e0, 0x03e1):
        tag = "mst16"
    elif breq == 0x17 and widx == 0x003f:
        tag = "pre"
    else:
        continue
    if tag in last and abs(t - last[tag]) < 0.05 and last.get(tag + "_v") == wval:
        continue
    last[tag] = t
    last[tag + "_v"] = wval
    print("  t=%7.2f  %-5s val=%04x" % (t, tag, wval))

print("\nIN ch0 (AN1) RMS dBFS per second:")
for sec in range(0, int(max([t for t, _ in samples_in[0]] or [0])) + 1):
    l = rms_db(samples_in[0], sec, sec + 1)
    if l:
        print("  %4d : %8.1f" % (sec, l))
