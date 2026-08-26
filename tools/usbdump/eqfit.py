#!/usr/bin/env python3
"""eqfit.py — identify the RME EQ biquad parameterization.

The settled 64-byte blocks carry 5 Q1.31-ish coefficients per band
(slot @0x04/0x14/0x24 + a 5th @0x34). We KNOW the parameters of a few
states from the capture actions (cap_eq3: Low band +6 dB @ 200 Hz,
then -6 dB @ 200 Hz), so we can test candidate parameterizations:
for each (formula, ordering, sign, implicit-constant) hypothesis, scan
(freq, Q, gain) and keep the best fit. A correct hypothesis must also
explain the OTHER settled states consistently.

Run: python3 eqfit.py
"""
import struct
import itertools
import math

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


def rbj_lowshelf(f, q, gain_db, fs=FS):
    # CORRECTED 2026-08-24 (cap_eq8d pin): the low-shelf cos signs + the
    # Q-driven α (was: high-shelf signs + S=1 α — ~4e-4 off).
    A = 10 ** (gain_db / 40.0)
    w0 = 2 * math.pi * f / fs
    c = math.cos(w0)
    s = math.sin(w0)
    alpha = s / (2 * q)
    b0 = A * ((A + 1) - (A - 1) * c + 2 * math.sqrt(A) * alpha)
    b1 = 2 * A * ((A - 1) - (A + 1) * c)
    b2 = A * ((A + 1) - (A - 1) * c - 2 * math.sqrt(A) * alpha)
    a0 = (A + 1) + (A - 1) * c + 2 * math.sqrt(A) * alpha
    a1 = -2 * ((A - 1) + (A + 1) * c)
    a2 = (A + 1) + (A - 1) * c - 2 * math.sqrt(A) * alpha
    return [b0, b1, b2, a1, a2], a0


def rbj_highshelf(f, q, gain_db, fs=FS):
    A = 10 ** (gain_db / 40.0)
    w0 = 2 * math.pi * f / fs
    c = math.cos(w0)
    s = math.sin(w0)
    alpha = s / (2 * q)
    b0 = A * ((A + 1) + (A - 1) * c + 2 * math.sqrt(A) * alpha)
    b1 = -2 * A * ((A - 1) + (A + 1) * c)
    b2 = A * ((A + 1) + (A - 1) * c - 2 * math.sqrt(A) * alpha)
    a0 = (A + 1) - (A - 1) * c + 2 * math.sqrt(A) * alpha
    a1 = -2 * ((A - 1) - (A + 1) * c)
    a2 = (A + 1) - (A - 1) * c - 2 * math.sqrt(A) * alpha
    return [b0, b1, b2, a1, a2], a0


def q31(v):
    return v / (1 << 31)


def load_settled(path):
    """Re-run the extraction logic (mirrors eq_extract.py) and return
    [(t, header, {slot: [5 coeffs Q1.31 floats]})] for the settled states."""
    import eq_extract as ex
    out = []
    for t, payload in ex.eq_blocks(path):
        out.append((t, payload))
    groups = []
    cur = [out[0]]
    for b in out[1:]:
        if b[0] - cur[-1][0] > 1.0:
            groups.append(cur)
            cur = []
        cur.append(b)
    groups.append(cur)
    states = []
    for grp in groups:
        t, payload = grp[-1]
        slots = {}
        for slot, off in enumerate((0x04, 0x14, 0x24)):
            words = [struct.unpack_from("<i", payload, off + 4 * k)[0] for k in range(4)]
            a5 = struct.unpack_from("<i", payload, 0x34)[0]
            if any(w != 0 for w in words):
                slots[slot] = [q31(w) for w in words] + [q31(a5)]
        states.append((t, payload[0:16], slots))
    return states


def response(coeffs, f, fs=FS):
    """|H(e^jw)| for a biquad (b0,b1,b2,a1,a2)."""
    b0, b1, b2, a1, a2 = coeffs
    w = 2 * math.pi * f / fs
    # z = e^jw
    re = math.cos(w)
    im = math.sin(w)
    num = b0 + b1 * re + b2 * (re * re - im * im)
    nim = b1 * im + b2 * 2 * re * im
    den = 1 + a1 * re + a2 * (re * re - im * im)
    dim = a1 * im + a2 * 2 * re * im
    m = math.hypot(num, nim) / math.hypot(den, dim)
    return m


def fit_state(obs, formula, f_lo, f_hi, q_lo, q_hi, g_lo, g_hi, steps):
    """Scan the parameter grid; return (err, f, q, gain_db, scale)."""
    best = None
    for f in [f_lo + (f_hi - f_lo) * i / steps for i in range(steps + 1)]:
        for q in [q_lo + (q_hi - q_lo) * i / steps for i in range(steps + 1)]:
            for g in [g_lo + (g_hi - g_lo) * i / steps for i in range(steps + 1)]:
                coeffs, a0 = formula(f, q, g)
                norm = [c / a0 for c in coeffs]
                # scale the normalized coeffs to match obs' magnitude,
                # then compare shapes
                for scale in (1.0,):
                    for perm in itertools.permutations(range(5)):
                        sel = [norm[p] for p in perm]
                        err = sum((sel[k] * scale - obs[k]) ** 2 for k in range(5))
                        if best is None or err < best[0]:
                            best = (err, f, q, g, scale, perm)
    return best


def main():
    states = load_settled("cap_eq3.pcap")
    # known: group 2 (index 1) = Low band +6 dB @ 200 Hz; group 3 = -6 dB
    print("settled states:", len(states))
    for i, (t, hdr, slots) in enumerate(states):
        if slots:
            print(f"  state {i}: {slots}")
    obs_p6 = states[1][2][0]  # slot1 coeffs (5 values), +6 dB @200 Hz
    print("\nfit +6 dB @ 200 Hz:", obs_p6)
    for name, fn in (("peaking", rbj_peaking), ("lowshelf", rbj_lowshelf), ("highshelf", rbj_highshelf)):
        err, f, q, g, sc, perm = fit_state(obs_p6, fn, 20, 20000, 0.7, 5.0, -20, 20, 24)
        print(f"  {name}: err={err:.3e} f={f:.0f} Q={q:.2f} gain={g:.1f} perm={perm}")


if __name__ == "__main__":
    main()
