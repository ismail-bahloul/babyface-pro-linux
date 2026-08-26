#!/usr/bin/env python3
"""eq_warp6.py — fine-grained fit on the Q=0.7 sweep states.

Refines (f, Q, gain) around the coarse best to find the residual floor
per state — a floor ~1e-6 = the labels are just slightly off; a floor
~1e-3 = a structural high-freq deviation.
"""
import struct
import math
import sys
import eq_extract as ex

FS = 48000.0


def q27(v):
    return v / (1 << 27)


def settled_slot1(path):
    blocks = ex.eq_blocks(path)
    groups = []
    cur = [blocks[0]]
    for b in blocks[1:]:
        if b[0] - cur[-1][0] > 1.0:
            groups.append(cur)
            cur = []
        cur.append(b)
    groups.append(cur)
    out = []
    for grp in groups:
        t, payload = grp[-1]
        words = [struct.unpack_from("<i", payload, 0x04 + 4 * k)[0] for k in range(4)]
        if all(w == 0 for w in words):
            continue
        a5 = struct.unpack_from("<i", payload, 0x34)[0]
        out.append([q27(w) for w in words] + [q27(a5)])
    return out


def rbj_stored(f, q, gain_db):
    A = 10 ** (gain_db / 40.0)
    w0 = 2 * math.pi * f / FS
    alpha = math.sin(w0) / (2 * q)
    c = math.cos(w0)
    b0 = 1 + alpha * A
    b1 = -2 * c
    b2 = 1 - alpha * A
    a0 = 1 + alpha / A
    a1 = -2 * c
    a2 = 1 - alpha / A
    b0n, b1n, b2n, a1n, a2n = [x / a0 for x in (b0, b1, b2, a1, a2)]
    return [a1n, a2n, b1n / b0n, b2n / b0n, b0n]


def refine(obs, f0, q0, g0):
    """Simple coordinate descent around (f0,q0,g0)."""
    f, q, g = f0, q0, g0
    for _ in range(200):
        best = (sum((rbj_stored(f, q, g)[k] - obs[k]) ** 2 for k in range(5)), f, q, g)
        for df in (-5, 5):
            for dq in (-0.005, 0.005):
                for dg in (-0.05, 0.05):
                    e = sum((rbj_stored(f + df, q + dq, g + dg)[k] - obs[k]) ** 2 for k in range(5))
                    if e < best[0]:
                        best = (e, f + df, q + dq, g + dg)
        if best[0] >= (sum((rbj_stored(f, q, g)[k] - obs[k]) ** 2 for k in range(5))):
            break
        f, q, g = best[1], best[2], best[3]
    return best


def main():
    states = settled_slot1(sys.argv[1] if len(sys.argv) > 1 else "cap_eq10.pcap")
    print("Q=0.7 sweep fine fit (states 1-8):")
    print("idx   resid     f       Q      gain")
    # coarse guesses from eq_warp4
    coarse = [(998, 0.70, 6.34), (1995, 0.70, 6.34), (4050, 0.70, 6.34),
              (7990, 0.63, 6.34), (10065, 0.63, 6.34), (12000, 0.57, 6.34),
              (16010, 0.43, 6.34), (19561, 0.30, 6.34)]
    for i, obs in enumerate(states[1:9]):
        err, f, q, g = refine(obs, *coarse[i])
        print(f"{i+1:3d}  {err:.3e}  {f:7.1f}  {q:.3f}  {g:+.3f}")


if __name__ == "__main__":
    main()
