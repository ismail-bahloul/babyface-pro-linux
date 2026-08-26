#!/usr/bin/env python3
"""eqorder.py — pin the biquad ordering + the '1 + delta' structure.

SUPERSEDED 2026-08-24: the storage is now fully decoded (see
`eq_biquad.md`) — the 5 stored words are the RBJ biquad in a
normalized-num/den split (c0=a1′, c1=a2′, c2=b1′/b0′, c3=b2′/b0′,
c4=b0′, word = value ×2²⁷), NOT a "1 + delta" form. Kept for the
historical exploration trail.

cap_eq8c is a fully-labeled gain sweep: g1..g6 = -20..-1 dB, g7 = 0 dB
(all-zero coefficients = identity filter -> the stored values are the
DELTA, actual = 1 + delta), g8..g13 = +1..+20 dB. For a peaking EQ the
response is 0 dB at DC and Nyquist for EVERY gain. Enumerate the
permutations of the 5 stored values into (b0,b1,b2,a1,a2) of the delta
biquad and check which makes |1+delta| = 0 dB at DC and Nyquist for all
13 states (the sign of the swap g6->g8 also fixes which pair is the
numerator). Also reports the peak gain + freq (should equal the labeled
gain at the fixed center freq).
"""
import struct
import math
import itertools
import sys

import eq_extract as ex

FS = 48000.0
LABELED = [-20, -15, -10, -6, -3, -1, 0, 1, 3, 6, 10, 15, 20]


def H(c, f):
    b0, b1, b2, a1, a2 = c
    w = 2 * math.pi * f / FS
    re, im = math.cos(w), math.sin(w)
    num = b0 + b1 * re + b2 * (re * re - im * im)
    nim = b1 * im + b2 * 2 * re * im
    den = 1 + a1 * re + a2 * (re * re - im * im)
    dim = a1 * im + a2 * 2 * re * im
    return complex(num, nim) / complex(den, dim)


def settled_slots(path):
    blocks = ex.eq_blocks(path)
    groups, cur = [], [blocks[0]]
    for b in blocks[1:]:
        if b[0] - cur[-1][0] > 1.0:
            groups.append(cur)
            cur = []
        cur.append(b)
    groups.append(cur)
    out = []
    for grp in groups:
        t, p = grp[-1]
        words = [struct.unpack_from("<i", p, 0x04 + 4 * k)[0] for k in range(4)]
        a5 = struct.unpack_from("<i", p, 0x34)[0]
        out.append([w / (1 << 31) for w in words] + [a5 / (1 << 31)])
    return out


def db(x):
    return 20 * math.log10(abs(x) + 1e-9)


def main():
    states = settled_slots(sys.argv[1] if len(sys.argv) > 1 else "cap_eq8c.pcap")
    print(f"{len(states)} states (expect 13)")
    results = []
    for perm in itertools.permutations(range(5)):
        dcs, nyqs = [], []
        for v in states:
            c = [v[perm[k]] for k in range(5)]
            dcs.append(db(1 + H(c, 1.0)))
            nyqs.append(db(1 + H(c, FS / 2)))
        dc_err = sum(abs(d) for d in dcs)
        nyq_err = sum(abs(n) for n in nyqs)
        results.append((dc_err, nyq_err, perm, dcs, nyqs))
    results.sort(key=lambda r: (r[0] + r[1]))
    print("permutations ranked by |DC| + |Nyquist| deviation from 0 dB:")
    for dc_err, nyq_err, perm, dcs, nyqs in results[:6]:
        print(f"  perm={perm} dc_err={dc_err:.3f} nyq_err={nyq_err:.3f}")
        print(f"    DC : " + " ".join(f"{d:+.2f}" for d in dcs))
        print(f"    Nyq: " + " ".join(f"{n:+.2f}" for n in nyqs))


if __name__ == "__main__":
    main()
