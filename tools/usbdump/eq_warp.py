#!/usr/bin/env python3
"""eq_warp.py — pin the high-frequency warping from a labeled bell sweep.

For each settled slot-1 state of a +6 dB bell freq sweep (cap_eq10.pcap),
scan the frequency at fixed (Q, +6 dB) and find the best RBJ match in
the device's stored format [a1', a2', b1'/b0', b2'/b0', b0'] (×2^31 Q31).
Prints the implied frequency + the residual — a small residual with the
implied f == the label means no warping; a growing residual or an
implied f != label means the device maps the frequency differently.

Usage: python3 eq_warp.py cap_eq10.pcap
"""
import struct
import math
import sys
import eq_extract as ex

FS = 48000.0


def rbj_peaking(f, q, gain_db, fs=FS):
    A = 10 ** (gain_db / 40.0)
    w0 = 2 * math.pi * f / fs
    alpha = math.sin(w0) / (2 * q)
    c = math.cos(w0)
    b0 = 1 + alpha * A
    b1 = -2 * c
    b2 = 1 - alpha * A
    a0 = 1 + alpha / A
    a1 = -2 * c
    a2 = 1 - alpha / A
    return [b0, b1, b2, a1, a2], a0


def stored_form(coeffs, a0):
    """RBJ coeffs -> the device's 5 stored words (Q31 floats)."""
    b0, b1, b2, a1, a2 = [c / a0 for c in coeffs]
    return [a1, a2, b1 / b0, b2 / b0, b0]


def q27(v):
    return v / (1 << 27)  # the device stores value ×2^27 (eq_biquad.md)


def settled_slot1(path):
    """Return [(t, [5 stored Q31 floats of slot1])] for settled groups."""
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
        out.append((t, [q27(w) for w in words] + [q27(a5)]))
    return out


def best_f(obs, q, gain_db):
    """Scan f (20..20k) for the best RBJ match; return (resid, f)."""
    best = None
    for i in range(4000):
        f = 20 + (20000 - 20) * i / 4000
        coeffs, a0 = rbj_peaking(f, q, gain_db)
        st = stored_form(coeffs, a0)
        err = sum((st[k] - obs[k]) ** 2 for k in range(5))
        if best is None or err < best[0]:
            best = (err, f)
    return best


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "cap_eq10.pcap"
    states = settled_slot1(path)
    print(f"{len(states)} settled slot1 states — scanning RBJ bell (+6 dB):")
    # assume the sweep order: Q=0.7 then Q=5 (user plan)
    # print states with their best-fit freq at Q=0.7 and Q=5, plus the
    # residual — the user's labeled sequence is monotonic, so the Q that
    # gives clean residuals + monotonic freqs is the right one.
    print("idx   t        fit@Q0.7 resid    f       fit@Q5 resid     f")
    for i, (t, obs) in enumerate(states):
        r1, f1 = best_f(obs, 0.7, 6.0)
        r5, f5 = best_f(obs, 5.0, 6.0)
        print(f"{i:3d} {t:12.3f}  {r1:.3e} {f1:8.0f}   {r5:.3e} {f5:8.0f}")


if __name__ == "__main__":
    main()
