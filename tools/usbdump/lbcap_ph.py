#!/usr/bin/env python3
"""lbcap_ph.py — measure the IN-stream words 0/2 (AN1/2 + PH3/4 loopback
targets) vs the PH3/4 master over time, from a USBPcap capture.  One
line per 1 s bucket: t, master8, IN ch0 RMS, IN ch2 RMS.

Usage: python lbcap_ph.py cap_lbph.pcap
"""
import struct, sys

def iter_pkts(path):
    data = open(path, 'rb').read()
    off = 24
    while off + 16 <= len(data):
        ts_sec, ts_usec, cap_len, orig = struct.unpack_from('<IIII', data, off)
        off += 16
        pkt = data[off:off + cap_len]
        off += cap_len
        if cap_len < 36:
            continue
        yield ts_sec + ts_usec / 1e6, pkt

def ch_rms(payload, ch):
    """RMS of one channel across a URB's frames.  Both the OUT and the IN
    frames carry the 24-bit sample in bytes 1-3 (byte 0 = aux 0x00);
    verified at the aligned payload offset (hlen=27) cap_lbph 2026-08-26.
    The earlier "IN in bytes 0-2" reading was the misaligned grid."""
    n = len(payload) // 56
    if n == 0:
        return 0.0
    s = 0.0
    for f in range(n):
        base = f * 56 + ch * 4
        word = struct.unpack_from('<I', payload, base)[0]
        v = (word >> 8) & 0xFFFFFF
        if v >= 0x800000:
            v -= 0x1000000
        v /= 8388608.0
        s += v * v
    return (s / n) ** 0.5

def main():
    path = sys.argv[1]
    master = None
    acc = {}   # bucket -> [n_in0, n_in2, sum0, sum2, n_ctrl]
    t0 = None
    for t, pkt in iter_pkts(path):
        ep = struct.unpack_from('<H', pkt, 20)[0]
        # USBPcap header_len (u16@0): 28 for control records, 27 for
        # audio URBs.  Setup follows the 28-byte control header;
        # audio payload starts at pkt[27:].
        hlen = struct.unpack_from('<H', pkt, 0)[0]
        if ep == 0x0000:
            setup = pkt[28:36]
            if setup[0] == 0x40:
                breq, wval, widx, wlen = struct.unpack_from('<BHHH', setup, 1)
                if breq == 0x1A and widx in (0x0006, 0x0007):
                    master = wval & 0xFF
                if t0 is None:
                    t0 = t
                b = acc.setdefault(int((t - t0) / 1.0), [0, 0, 0.0, 0.0, 0])
                b[4] += 1
                if master is not None:
                    b.append(master)
            continue
        if ep == 0x8200 and len(pkt) >= 27 + 56:
            payload = pkt[hlen:]
            if len(payload) >= 56:
                r0 = ch_rms(payload, 0)
                r2 = ch_rms(payload, 2)
                if t0 is None:
                    t0 = t
                b = acc.setdefault(int((t - t0) / 1.0), [0, 0, 0.0, 0.0, 0])
                b[0] += 1
                b[1] += 1
                b[2] += r0 * r0
                b[3] += r2 * r2
    print(f"{'t':>5} {'m8':>4} {'IN ch0':>9} {'IN ch2':>9} {'ctrls':>6}")
    for b in sorted(acc):
        n0, n2, s0, s2, c = acc[b][:5]
        m = acc[b][5] if len(acc[b]) > 5 else -1
        print(f"{b:5d} {m:4d} {((s0/n0)**0.5 if n0 else 0):9.5f} {((s2/n2)**0.5 if n2 else 0):9.5f} {c:6d}")

if __name__ == '__main__':
    main()
