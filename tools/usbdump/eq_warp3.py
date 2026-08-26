#!/usr/bin/env python3
"""eq_warp3.py — solve the IMPLIED (f, alpha) from the stored words.

For a peaking bell, the stored words give exact equations:
  r  = c1/c4 = a2n/b0n = (1 - α/A)/(1 + αA)
  α  = (1 - r) / (r·A + 1/A)
  c0 = a1n = -2·cos(w0)/a0  →  cos(w0) = -c0·(1 + αA)/(2·c4)
  f  = acos(cos(w0))·fs/(2π)

So each settled state yields the device's IMPLIED α and frequency —
compare against the RBJ predictions to pin the warping.
"""
import struct
import math
import sys
import eq_extract as ex

FS = 48000.0
LABELS = [1000, 2000, 4000, 8000, 10000, 12000, 16000, 20000]
Q = 0.7
GAIN = 6.0


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


def implied(stored):
    c0, c1, c2, c3, c4 = stored
    A = 10 ** (GAIN / 40.0)
    r = c1 / c4
    alpha = (1 - r) / (r * A + 1 / A)
    cosw = -c0 * (1 + alpha * A) / (2 * c4)
    cosw = max(-1.0, min(1.0, cosw))
    f = math.acos(cosw) * FS / (2 * math.pi)
    return alpha, f, c0, c1, c2, c3, c4


def main():
    states = settled_slot1(sys.argv[1] if len(sys.argv) > 1 else "cap_eq10.pcap")
    print("Q=0.7 sweep (states 1-8):")
    print("label   implied f    alpha(impl)   sin(w0)/2Q   alpha/(sin/2Q)")
    for i, lab in enumerate(LABELS):
        alpha, f, *_ = implied(states[i + 1])
        w0 = 2 * math.pi * lab / FS
        a_rbj = math.sin(w0) / (2 * Q)
        print(f"{lab:6d}  {f:9.1f}   {alpha:.6f}   {a_rbj:.6f}   {alpha / a_rbj:.5f}")
    # state 0 = the setup before the sweep (unknown label) — show it too
    alpha, f, *_ = implied(states[0])
    print(f"state0 (setup): implied f={f:.0f} Hz  alpha={alpha:.6f}")


if __name__ == "__main__":
    main()
