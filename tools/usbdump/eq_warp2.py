#!/usr/bin/env python3
"""eq_warp2.py — test frequency-warping hypotheses against cap_eq10.

Known-labeled states (from the user's plan): Q=0.7 sweep then Q=5 sweep,
each 1k,2k,4k,8k,10k,12k,16k,20k Hz at +6 dB bell. For each hypothesis
of how the device maps the frequency, compute the residual vs the stored
words. The hypothesis that collapses the residual at ALL freqs wins.
"""
import struct
import math
import sys
import eq_extract as ex

FS = 48000.0
LABELS_Q7 = [1000, 2000, 4000, 8000, 10000, 12000, 16000, 20000]
LABELS_Q5 = [1000, 2000, 4000, 8000, 10000, 12000, 16000, 20000]


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


def rbj(f, q, gain_db, w0map):
    A = 10 ** (gain_db / 40.0)
    w0 = 2 * math.pi * f / FS
    w0e = w0map(w0)
    alpha = math.sin(w0e) / (2 * q)
    c = math.cos(w0e)
    b0 = 1 + alpha * A
    b1 = -2 * c
    b2 = 1 - alpha * A
    a0 = 1 + alpha / A
    a1 = -2 * c
    a2 = 1 - alpha / A
    b0n, b1n, b2n, a1n, a2n = [x / a0 for x in (b0, b1, b2, a1, a2)]
    return [a1n, a2n, b1n / b0n, b2n / b0n, b0n]


def resid(obs, f, q, w0map):
    st = rbj(f, q, 6.0, w0map)
    return sum((st[k] - obs[k]) ** 2 for k in range(5))


def main():
    states = settled_slot1(sys.argv[1] if len(sys.argv) > 1 else "cap_eq10.pcap")
    # states 1-8 = Q0.7 sweep, 9-16 = Q5 sweep (state 0 = the setup)
    q7 = states[1:9]
    q5 = states[9:17]
    hyps = {
        "plain w0": lambda w: w,
        "tan prewarp 2*atan(tan(w/2)*?)": None,
        "2*atan(w/2)": lambda w: 2 * math.atan(w / 2),
        "2*atan(tan(w/2))": lambda w: 2 * math.atan(math.tan(w / 2)),
        "w*(1+w/4)": lambda w: w * (1 + w / 4),
        "w/(1-w/8)": lambda w: w / (1 - w / 8),
    }
    print("Q=0.7 sweep residuals per hypothesis (sum over 8 labeled states):")
    for name, m in hyps.items():
        if m is None:
            continue
        s = sum(resid(obs, f, 0.7, m) for obs, f in zip(q7, LABELS_Q7))
        print(f"  {name:28s} {s:.6e}")
    print("Q=5 sweep:")
    for name, m in hyps.items():
        if m is None:
            continue
        s = sum(resid(obs, f, 5.0, m) for obs, f in zip(q5, LABELS_Q5))
        print(f"  {name:28s} {s:.6e}")


if __name__ == "__main__":
    main()
