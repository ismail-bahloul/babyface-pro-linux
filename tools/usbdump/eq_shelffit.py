#!/usr/bin/env python3
"""eq_shelffit.py — pin the EXACT shelf formula from cap_eq8d.

Settled states = the last 64-byte block of each >1 s gap (TotalMix
uploads ONLY during ramps; the plateau holds the target). The RESPONSE
of each stored filter (b0=c4, b1=c2·c4, b2=c3·c4, a1=c0, a2=c1) gives
the LABEL: DC gain = shelf gain, the midpoint point = the corner.

FINDING (2026-08-24): the RBJ shelf formulas used in eq_biquad.md /
eqfit.py had the LOW/HIGH shelf sign mixup (cos-term signs swapped) —
with the CORRECT RBJ low-shelf formulas + α = sin(w0)/(2Q) the stored
coefficients match to ~1e-5 (vs ~4e-4 with the wrong-sign formulas).

Usage: python3 eq_shelffit.py [cap_eq8d.pcap]
"""
import math
import struct
import sys

import eq_extract as ex

FS = 48000.0


def stored_filter(payload):
    c0 = struct.unpack_from("<i", payload, 0x04)[0] / (1 << 27)
    c1 = struct.unpack_from("<i", payload, 0x08)[0] / (1 << 27)
    c2 = struct.unpack_from("<i", payload, 0x0C)[0] / (1 << 27)
    c3 = struct.unpack_from("<i", payload, 0x10)[0] / (1 << 27)
    c4 = struct.unpack_from("<i", payload, 0x34)[0] / (1 << 27)
    return [c4, c2 * c4, c3 * c4, c0, c1]


def resp(cf, f, fs=FS):
    b0, b1, b2, a1, a2 = cf
    w = 2 * math.pi * f / fs
    re, im = math.cos(w), math.sin(w)
    num = b0 + b1 * re + b2 * (re * re - im * im)
    nim = b1 * im + b2 * 2 * re * im
    den = 1 + a1 * re + a2 * (re * re - im * im)
    dim = a1 * im + a2 * 2 * re * im
    return math.hypot(num, nim) / math.hypot(den, dim)


def low_shelf_norm(f, q, gain_db, alpha_mode):
    """Correct RBJ LOW SHELF (cos signs: b0/a0 use -(A-1)c, b1/a1 use
    +(A+1)c), normalized by a0, in the stored (a1', a2', b1'/b0',
    b2'/b0', b0') order."""
    A = 10 ** (gain_db / 40.0)
    w0 = 2 * math.pi * f / FS
    c = math.cos(w0)
    s = math.sin(w0)
    if alpha_mode == "q":
        alpha = s / (2 * q)
    elif alpha_mode == "s1":
        alpha = s / 2 * math.sqrt(2)
    else:
        raise ValueError(alpha_mode)
    b0 = A * ((A + 1) - (A - 1) * c + 2 * math.sqrt(A) * alpha)
    b1 = 2 * A * ((A - 1) - (A + 1) * c)
    b2 = A * ((A + 1) - (A - 1) * c - 2 * math.sqrt(A) * alpha)
    a0 = (A + 1) + (A - 1) * c + 2 * math.sqrt(A) * alpha
    a1 = -2 * ((A - 1) + (A + 1) * c)
    a2 = (A + 1) + (A - 1) * c - 2 * math.sqrt(A) * alpha
    b0n, b1n, b2n = b0 / a0, b1 / a0, b2 / a0
    return [a1 / a0, a2 / a0, b1n / b0n, b2n / b0n, b0n]


def settled(path):
    blocks = list(ex.eq_blocks(path))
    groups = []
    cur = [blocks[0]]
    for b in blocks[1:]:
        if b[0] - cur[-1][0] > 1.0:
            groups.append(cur)
            cur = []
        cur.append(b)
    groups.append(cur)
    return [(grp[-1][0], stored_filter(grp[-1][1])) for grp in groups]


def label(cf):
    """(gain_db, corner_hz) from the response: DC gain and the
    geometric-mid response point (the corner)."""
    r_dc = resp(cf, 1.0)
    r_hi = resp(cf, 15000.0)
    g = 20 * math.log10(r_dc)
    target = (r_dc * r_hi) ** 0.5
    falling = r_dc > r_hi  # low shelf: DC is the boosted/cut side
    lo, hi_f = 5.0, 20000.0
    for _ in range(60):
        mid = math.sqrt(lo * hi_f)
        below = resp(cf, mid) > target if falling else resp(cf, mid) < target
        if below:
            lo = mid
        else:
            hi_f = mid
    return g, math.sqrt(lo * hi_f)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "cap_eq8d.pcap"
    states = settled(path)
    print(f"{path}: {len(states)} settled states")
    ident = lambda cf: all(abs(x) < 1e-6 for x in cf)
    for i, (t, cf) in enumerate(states):
        if ident(cf):
            print(f"  state {i}: IDENTITY")
            continue
        g, corner = label(cf)
        print(f"  state {i}: gain={g:+.2f} dB  corner={corner:.0f} Hz")

    print("\n=== coefficient comparison (storage order c0..c4) ===")
    for mode in ("q", "s1"):
        print(f"-- alpha={mode} --")
        for i, (t, cf) in enumerate(states):
            if ident(cf):
                continue
            # storage order: c0=a1', c1=a2', c2=b1'/b0', c3=b2'/b0', c4=b0'
            stored = [cf[3], cf[4], cf[1] / cf[0], cf[2] / cf[0], cf[0]]
            g, corner = label(cf)
            g = round(g / 3.0) * 3.0  # snap to the ±3 dB sweep grid
            best = None
            for f in [freq for freq in
                      (corner / 4, corner / 2, corner, corner * 2, corner * 4)
                      if 5 < freq < 22000]:
                for q in (0.7, 1.0, 1.5, 2.0, 3.0, 4.0, 5.0):
                    pred = low_shelf_norm(f, q, g, mode)
                    e = max(abs(o - p) for o, p in zip(stored, pred))
                    if best is None or e < best[0]:
                        best = (e, f, q)
            e, f, q = best
            print(f"  state {i}: err={e:.2e}  f={f:.0f} Hz  Q={q:.2f}  gain={g:+.0f} dB")
            print(f"           stored={' '.join(f'{x:+.6f}' for x in stored)}")
            print(f"           pred  ={' '.join(f'{x:+.6f}' for x in low_shelf_norm(f, q, g, mode))}")


if __name__ == "__main__":
    main()
