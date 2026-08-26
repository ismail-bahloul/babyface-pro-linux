#!/usr/bin/env python3
"""eq_warp4.py — full 3-parameter fit (f, Q, gain) per settled state.

Scans f/Q/gain and reports the best RBJ match + residual. If only f
drifts (Q≈0.7, gain≈6 constant), the warping is a frequency mapping.
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


def fit(obs):
    best = None
    for fi in range(2000):
        f = 50 + (20000 - 50) * fi / 2000
        for qi in range(90):
            q = 0.3 + 6.0 * qi / 90
            for gi in range(41):
                g = -20 + 40 * gi / 41
                st = rbj_stored(f, q, g)
                err = sum((st[k] - obs[k]) ** 2 for k in range(5))
                if best is None or err < best[0]:
                    best = (err, f, q, g)
    return best


def main():
    states = settled_slot1(sys.argv[1] if len(sys.argv) > 1 else "cap_eq10.pcap")
    print("idx   resid      f        Q     gain")
    for i, obs in enumerate(states):
        err, f, q, g = fit(obs)
        print(f"{i:3d}  {err:.3e}  {f:7.0f}  {q:.2f}  {g:+.2f}")


if __name__ == "__main__":
    main()
