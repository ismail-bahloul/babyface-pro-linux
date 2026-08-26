#!/usr/bin/env python3
"""eqpeak.py — crack the RME biquad by matching the PEAK of the
frequency response against the LABELED freq sweep (cap_eq8a: Low bell,

SUPERSEDED 2026-08-24: the band biquad is fully decoded (see
`eq_biquad.md`) — normalized-num/den split of the RBJ cookbook; the
peak-of-permutation search below was the pre-decoding exploration.
Q=0.7, +6 dB, freq 50..10000 Hz). For each candidate interpretation
(permutation of the 5 coeffs into (b0,b1,b2,a1,a2), optional "+1" on
one numerator slot, and the 1+H / 1/(1+H) wrappers), compute the
response peak of every settled state; the correct interpretation makes
the peaks a monotonic freq sequence ending near 10000 Hz with a ~+6 dB
gain.
"""
import struct
import itertools
import math
import sys

import eq_extract as ex

FS = 48000.0


def H(coeffs, f):
    b0, b1, b2, a1, a2 = coeffs
    w = 2 * math.pi * f / FS
    re, im = math.cos(w), math.sin(w)
    num = b0 + b1 * re + b2 * (re * re - im * im)
    nim = b1 * im + b2 * 2 * re * im
    den = 1 + a1 * re + a2 * (re * re - im * im)
    dim = a1 * im + a2 * 2 * re * im
    return complex(num, nim) / complex(den, dim)


def peak_of(fun, lo=10.0, hi=20000.0):
    best = None
    f = lo
    while f <= hi:
        g = abs(fun(f))
        if best is None or g > best[1]:
            best = (f, g)
        f *= 1.02
    return best


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
        if any(w != 0 for w in words):
            out.append([w / (1 << 31) for w in words] + [a5 / (1 << 31)])
    return out


def main():
    states = settled_slots(sys.argv[1] if len(sys.argv) > 1 else "cap_eq8a.pcap")
    print(f"{len(states)} settled states")
    results = []
    for perm in itertools.permutations(range(5)):
        for one in (-1, 0, 1, 2):  # -1 = no +1, else numerator slot
            for wrap in ("id", "1+", "inv"):
                freqs, gains = [], []
                for v in states:
                    c = [v[perm[k]] for k in range(5)]
                    if one >= 0:
                        c[one] += 1.0
                    if wrap == "id":
                        f, g = peak_of(lambda f, c=c: H(c, f))
                    elif wrap == "1+":
                        f, g = peak_of(lambda f, c=c: 1.0 + H(c, f))
                    else:
                        f, g = peak_of(lambda f, c=c: 1.0 / (1.0 + H(c, f)))
                    freqs.append(f)
                    gains.append(20 * math.log10(max(g, 1e-9)))
                monot = all(freqs[i] <= freqs[i + 1] * 1.05 for i in range(len(freqs) - 1))
                spread = max(freqs) / max(min(freqs), 10)
                gain_err = sum(abs(g - 6.0) for g in gains) / len(gains)
                results.append((monot, spread, gain_err, perm, one, wrap, freqs, gains))
    good = [r for r in results if r[0]]
    good.sort(key=lambda r: (-r[1], r[2]))
    print("monotonic interpretations (sorted by freq spread, then gain error):")
    for r in good[:6]:
        print(f"  perm={r[3]} +1@{r[4]} wrap={r[5]} spread={r[1]:.1f} gain_err={r[2]:.2f}")
        print(f"    freqs: {', '.join(f'{f:.0f}' for f in r[6])}")
        print(f"    gains: {', '.join(f'{g:+.1f}' for g in r[7])}")


if __name__ == "__main__":
    main()
