#!/usr/bin/env python3
"""lbcap_levels.py — correlate the interrupt OUT/IN audio streams of a USBPcap
file and print the 440 Hz level of word 0 in each (OUT = playback, IN =
record), per time window.

Usage: lbcap_levels.py <file.pcap>
cap_lbcal.pcap result: IN == OUT at every master step (Windows records
the loopback bus 1:1 — the reference for the kernel loopback staging).
"""
import struct, sys, math

path = sys.argv[1]
data = open(path, 'rb').read()
off = 24
frames_out = []   # (t, word0_sample)  — OUT stream
frames_in = []    # (t, word0_sample)  — IN stream
t0 = None

while off + 16 <= len(data):
    ts_sec, ts_usec, cap_len, orig_len = struct.unpack_from('<IIII', data, off)
    off += 16
    pkt = data[off:off + cap_len]
    off += cap_len
    if cap_len < 36:
        continue
    ep = struct.unpack_from('<H', pkt, 20)[0]
    # USBPcap header_len (u16@0) varies: 28 for control records, 27 for
    # audio URBs — the payload always starts at pkt[hlen:] (fixed
    # 2026-08-26: pkt[28:] misaligns every audio frame by 1 byte).
    hlen = struct.unpack_from('<H', pkt, 0)[0]
    payload = pkt[hlen:]
    # OUT interrupt (playback): ep 0x0100
    if ep == 0x0100 and len(payload) >= 64:
        w0 = struct.unpack_from('<I', payload, 0)[0]
        s = (w0 & 0x00ffffff)  # left-justified sample (bits 8-31)
        s = (w0 >> 8) & 0xffffff
        if s & 0x800000:
            s -= 1 << 24
        t = ts_sec + ts_usec / 1e6
        if t0 is None:
            t0 = t
        frames_out.append((t - t0, s))
    # IN interrupt (record): ep 0x8200
    elif ep == 0x8200 and len(payload) >= 64:
        w0 = struct.unpack_from('<I', payload, 0)[0]
        s = (w0 >> 8) & 0xffffff
        if s & 0x800000:
            s -= 1 << 24
        t = ts_sec + ts_usec / 1e6
        if t0 is None:
            t0 = t
        frames_in.append((t - t0, s))

print("OUT frames: %d, IN frames: %d" % (len(frames_out), len(frames_in)))

def goertzel_amp(samples, freq, rate=48000):
    win = min(rate, len(samples))
    cw = 2 * math.cos(2 * math.pi * freq / rate)
    s0 = s1 = 0.0
    for x in samples[:win]:
        t = x + cw * s0 - s1
        s1, s0 = s0, t
    p = s1 * s1 + s0 * s0 - cw * s1 * s0
    return math.sqrt(p * 2) / win

def measure(stream, t_lo, t_hi):
    xs = [s for (t, s) in stream if t_lo <= t < t_hi]
    if not xs:
        return None
    amp = goertzel_amp(xs, 440)
    return 20 * math.log10(amp / (1 << 23))

# Windows where the master is ~0 dB (0x2000): t=14.28 has 0x1f17 (-0.5 dB).
# The sweep then goes DOWN; find where it returns near 0x2000, and the end.
for label, lo, hi in [("master~-0.5dB", 14.2, 14.4),
                      ("master~-3dB(0x16a0)", 15.0, 15.2),
                      ("master~-10dB", 20.0, 21.0),
                      ("late", 30.0, 32.0)]:
    out_l = measure(frames_out, lo, hi)
    in_l = measure(frames_in, lo, hi)
    print("%-22s OUT=%7.1f dBFS  IN=%7.1f dBFS  (IN-OUT=%+.1f)" %
          (label, out_l if out_l else -200, in_l if in_l else -200,
           (in_l - out_l) if (in_l and out_l) else 0))
