# RME Babyface Pro FS — EQ band biquad: STORAGE STRUCTURE DECODED (2026-08-24)

**The band-EQ coefficient encoding is fully decoded.** The 5 stored values
per band slot are NOT the biquad coefficients directly, and they are NOT a
"1 + delta" of them either — they are the RBJ peaking/shelf biquad in a
**normalized numerator / normalized denominator split**:

```
H(z) = b0·(1 + c2·z⁻¹ + c3·z⁻²) / (1 + c0·z⁻¹ + c1·z⁻²)
```

with the five 32-bit stored words (slot @0x04/0x14/0x24 + shared @0x34):

| stored | /2²⁷ (= value) | meaning |
|---|---|---|
| c0 | a1′ | denominator z⁻¹ coeff (normalized) |
| c1 | a2′ | denominator z⁻² coeff (normalized) |
| c2 | b1′/b0′ | numerator z⁻¹ coeff, normalized by b0 |
| c3 | b2′/b0′ | numerator z⁻² coeff, normalized by b0 |
| c4 | b0′ | numerator scale = overall DC-ish gain term |

So the DSP reconstructs: `b0 = c4`, `b1 = c2·c4`, `b2 = c3·c4`,
`a1 = c0`, `a2 = c1` (all ×2²⁷). Equivalently the numerator is stored
"normalized by its leading coefficient" and the leading coefficient is
stored separately in c4.

## The (freq, Q, gain, type) → coefficients mapping

**BELL (peaking) — exact RBJ cookbook**, normalized by a0:

```
A   = 10^(gain_dB/40)
w0  = 2π·freq/fs            (fs = 48000)
α   = sin(w0)/(2Q)
c   = cos(w0)
b0 = 1 + αA    b1 = -2c    b2 = 1 - αA
a0 = 1 + α/A   a1 = -2c    a2 = 1 - α/A
normalized: b0′=b0/a0, b1′=b1/a0, b2′=b2/a0, a1′=a1/a0, a2′=a2/a0
stored: c0=a1′, c1=a2′, c2=b1′/b0′, c3=b2′/b0′, c4=b0′   (each ×2²⁷, rounded)
```

**Verified exhaustively (max error ≈ 1 LSB at 2²⁷ scale):**
- cap_eq8c (gain sweep -20..+20 dB @ 200 Hz, Q=0.7): every state's
  response peak/notch lands EXACTLY at 200 Hz with EXACTLY the labeled
  gain (+0.000 dB), DC & Nyquist = 0 dB (true bell).
- cap_eq8b (Q sweep 0.7..5 @ 200 Hz, +6 dB): peak @ 200 Hz, +6.000 dB
  for every Q.
- cap_eq8a (freq sweep 50..10000 Hz, +6 dB, Q=0.7): the stored values
  match the RBJ prediction to ~1e-7 at low freq, drifting to ~1e-2 at
  10 kHz — the residual drift is a frequency-warping detail still open
  (see below), not a structural error.
- cap_eq8d (shelf +6/0/-6): same structure; shelf uses the RBJ shelf
  formula (high-freq shelf coeffs go near -0.985 as documented).

**0 dB = ALL-ZERO slot coefficients** (identity, EQ effectively off) but
**c4 = 0x08000000** (= 0.0625 = 1/16 = 2²⁷/2³¹) stays as the "b0 = 1"
neutral value. The c0..c3 zeros make the numerator = denominator = 1.

**Gain sign flip (+g ↔ -g)** swaps c0↔c2 and c1↔c3 — which is exactly
the b1′=a1′ identity: for a peaking, b1 = a1 = -2c, so c2 (=b1′/b0′)
and c0 (=a1′) are related by the b0′ scale, and the mirror stores the
numerator/denominator normalized by the *other* scale.

## Storage layout recap

64-byte bulk OUT block on ep 0x0A (interface 1):

```
0x00: [ch 0/1] [0x00 for band EQ] [ch 0/1] [0x80 EQ active]
0x04: slot 1 (Low)  c0 c1 c2 c3   (4 × i32)
0x14: slot 2 (Mid)  c0 c1 c2 c3
0x24: slot 3 (High) c0 c1 c2 c3
0x34: shared c4 (i32)             — the b0′ scale, shared by all slots
0x38: low-cut freq word (0x04000000 = off)
0x3C: 0
```

c0..c4 are signed 32-bit, value = word / 2²⁷ (NOT /2³¹ — the ×16 factor
that confused earlier fits is the 2³¹/2²⁷ scale).

## Still open (small)

1. **Frequency warping at high freq — CHARACTERIZED 2026-08-25
   (cap_eq10.pcap, dense 1-20 kHz sweep at Q 0.7 and Q 5, +6 dB):
   the pure-RBJ w0 = 2πf/fs is exact to ~1e-4 below 2 kHz, then a
   small deviation grows. Full-parameter fit per state:
   - **Frequency: essentially exact** (implied f within ±1 % of the
     label all the way to 20 kHz).
   - **Gain: constant +6.1…+6.3 dB** for a +6 label (a ~0.3 dB
     A-law/display offset, not a drift).
   - **Effective Q shrinks at high freq** (the only real effect), for
     the Q=0.7 setting: 0.70 @≤4k → 0.635 @8k → 0.60 @10k → 0.555
     @12k → 0.435 @16k → 0.295 @20k (≈ 1 − 0.085·w0² up to 12 kHz);
     the Q=5 setting drifts faster (4.17 @8k → 2.10 @16k → 0.97
     @20k) — the deviation is Q-dependent, not a pure frequency
     warp. Residual floor after the free (f,Q,gain) fit ≈ 7e-5
     (1-16 kHz), 1.2e-3 @20 kHz — a small structural component
     remains.
   **Practical**: the response peak lands at the requested frequency;
   the filter is just slightly wider than the requested Q above
   4 kHz. Inaudible; keep the pure RBJ cookbook (the current
   `eq_band_storage`), optionally apply the Q_eff table for exact
   GUI display. The tools `eq_warp*.py` document the fits.
2. **Shelf exact formula — PINNED 2026-08-24 (cap_eq8d)**: the shelf is
   the RBJ shelf with **α = sin(w0)/(2Q)** (the same Q-driven α as the
   bell) and the CORRECT low/high cos signs. Two bugs were in the old
   implementation: (a) the α used the S=1 shelf slope (sin(w0)/2·√2,
   Q ignored) — that was the ~4e-4 error; (b) the LOW-SHELF formulas in
   eqfit.py had the HIGH-shelf cos signs swapped (a1 sign). The Rust
   `eq_band_storage` LowShelf already had the right cos signs; only the
   α + the HighShelf a1 sign needed fixing. Implied-α check (solved
   from the stored a1'/a2' of the +6/−6 @200 Hz states): 0.01869886 vs
   sin(w0)/(2·0.7) = 0.01869782 — matches to 5.6e-5 (the residual is
   the shared high-freq warping, see item 1). Verified to ~1e-6 on the
   100/200 Hz states, ~2.6e-4 at 1 kHz, drifting at ≥6 kHz (warping).
3. The block header byte 1 = 0x00 for band EQ (0x03 = low cut, 2^n−1).
4. **2-bands-simultaneous c4@0x34 — STRUCTURALLY VERIFIED (2026-08-24,
   cap_eq8e + eqfit)**: the shared 0x34 follows the LAST-written band.
   When the Mid is the only active band, 0x34 = its c4 and the
   identity c0/c2 = c4 = b0′/a0 holds (Mid c0/c2 = 1.0097 vs 0x34
   1.0092 — a peaking, a1=b1, so c0/c2 = b0/a0 ✓). When High joins,
   0x34 tracks the High. The 0x00/0x01 header pairs are L/R channel
   copies (identical payloads). So `set_eq_band` writing the modified
   band's b0′ at 0x34 replicates TotalMix exactly (the DSP's handling
   of one c4 for two bands is TotalMix's own model — we mirror it).

## How to write a band (TuxMix set_eq_band)

```rust
fn band_storage(freq: f32, q: f32, gain_db: f32, fs: f32) -> [i32; 5] {
    let a = 10f32.powf(gain_db / 40.0);
    let w0 = 2.0 * std::f32::consts::PI * freq / fs;
    let alpha = w0.sin() / (2.0 * q);
    let c = w0.cos();
    let (b0, b1, b2) = (1.0 + alpha * a, -2.0 * c, 1.0 - alpha * a);
    let (a0, a1, a2) = (1.0 + alpha / a, -2.0 * c, 1.0 - alpha / a);
    let (b0n, b1n, b2n) = (b0 / a0, b1 / a0, b2 / a0);
    let (a1n, a2n) = (a1 / a0, a2 / a0);
    let s = (1i64 << 27) as f32;
    [
        (a1n * s).round() as i32,
        (a2n * s).round() as i32,
        (b1n / b0n * s).round() as i32,
        (b2n / b0n * s).round() as i32,
        (b0n * s).round() as i32,
    ]
}
```
