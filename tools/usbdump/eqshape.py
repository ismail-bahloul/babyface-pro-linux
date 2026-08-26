#!/usr/bin/env python3
"""eqshape.py — find the coefficient ordering of the RME biquad by its
RESPONSE SHAPE. Known state: cap_eq3 group 2 = Low band, +6 dB @ 200 Hz
(bell). So the correct interpretation must give |H(0)| ≈ +6 dB and
|H(Nyquist)| ≈ 0 dB. Enumerate: 5! permutations of the observed values
into (b0,b1,b2,a1,a2) x sign flips x which position gets the implicit
"1" (identity) offset.
"""
import itertools
import math

FS = 48000.0


def resp(b0, b1, b2, a1, a2, f):
    w = 2 * math.pi * f / FS
    re, im = math.cos(w), math.sin(w)
    num = b0 + b1 * re + b2 * (re * re - im * im)
    nim = b1 * im + b2 * 2 * re * im
    den = 1 + a1 * re + a2 * (re * re - im * im)
    dim = a1 * im + a2 * 2 * re * im
    return math.hypot(num, nim) / math.hypot(den, dim)


def db(v):
    return 20 * math.log10(max(v, 1e-12))


def main():
    # cap_eq3 state 1 (group 2): Low +6 dB @ 200 Hz, slot1 + 0x34 value
    v = [-0.12360824411734939, 0.06113823922351003,
         -0.12302572280168533, 0.0605852953158319, 0.06278835190460086]
    results = []
    for perm in itertools.permutations(range(5)):
        for signs in itertools.product((1, -1), repeat=5):
            for one_pos in range(5):  # which position gets the "+1"
                c = [signs[k] * v[perm[k]] for k in range(5)]
                c[one_pos] += 1.0
                b0, b1, b2, a1, a2 = c
                # skip degenerate/unstable denominators
                dc = resp(b0, b1, b2, a1, a2, 1.0)
                ny = resp(b0, b1, b2, a1, a2, FS / 2)
                h200 = resp(b0, b1, b2, a1, a2, 200.0)
                score = abs(db(dc) - 6.0) + abs(db(ny) - 0.0) + abs(db(h200) - 6.0)
                results.append((score, c, db(dc), db(h200), db(ny)))
    results.sort(key=lambda r: r[0])
    print("top interpretations for +6 dB @ 200 Hz (want DC≈+6, 200Hz≈+6, Nyq≈0):")
    for score, c, dcdb, h200db, nydb in results[:8]:
        print(f"  score={score:7.3f}  coeffs=[{', '.join(f'{x:+.4f}' for x in c)}]  "
              f"DC={dcdb:+.2f} 200Hz={h200db:+.2f} Nyq={nydb:+.2f}")


if __name__ == "__main__":
    main()
