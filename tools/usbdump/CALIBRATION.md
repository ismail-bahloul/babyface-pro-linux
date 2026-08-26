# Calibration  cap_calib.pcap (2026-08-22)

Source: `tools/usbdump/cap_calib.pcap`  a 12-minute USBPcap session where
the user swept every control in TotalMix with pauses:
gain 0&#8594;65 dB (1-dB steps), fader AN1&#8594;AN1/2 -inf&#8594;+6 dB, master -inf&#8594;+6 dB,
mute, 48V on/off. A follow-up `cap_padpan.pcap` added the PAD toggles.
**PAD and pan were NOT captured** in the first session (user skipped);
PAD was captured in the follow-up; pan on a mono source emits no writes
(see "Pan" below).

## Mic gain raw range (extracted 2026-08-25 from cap_calib, req 0x1a)

| wIdx (BF_REG_GAIN+mic) | raw range | dB range | notes |
|---|---|---|---|
| 0x0000 = AN1 | **0-20** (21 values) | 0-65 dB | 3.25 dB/step (cap_calib + cap_gain12) |
| 0x0001 = AN2 | **0-20** (21 values) | 0-65 dB | 3.25 dB/step (cap_gain12) |
| 0x0002 = AN3 | **0-18** (19 values) | 0-9 dB | Hi-Z instrument, 0.5 dB/step (cap_gain34, manual §10) |
| 0x0003 = AN4 | **0-18** (19 values) | 0-9 dB | Hi-Z instrument, 0.5 dB/step (cap_gain34, manual §10) |
| 0x0004/5 = master-0 8-bit (BF_REG_MASTER_8) | 0-31 seen | — | the master sweep's 8-bit companion |

Kernel driver: `bf_gain_*` now uses per-mic scales (mics 0/1: db = raw·13/4,
0-65; mics 2/3: db = raw/2, 0-9).

Each raw value is written 3 times with the transaction counter
(0x00/0x20/0x40 OR'd in, low 5 bits = raw) — matches the driver's
`bf_gain_put` counter cycle.  The exact displayed-dB &#8594; raw rounding is
still open: TotalMix shows 0-65 dB in 1-dB steps but raw only has 21
levels, so the display must round.  Anchors to note down on Windows:
the displayed dB at raw 0 / 17 / 23 / 31 (the 23/31 only exist on the
8-bit master side — for the gains, raw 0/20 are the only full anchors,
plus whatever TotalMix shows between 3.25-dB steps).

## Crosspoint fader (AN1&#8594;AN1/2) — measured curve

The fader wrote the **low map** (`0x0000` L / `0x001A` R, both sides, same
value — TotalMix keeps them mirrored) because the AN1 input strip was
selected. A clean ~1-dB stepped descent from +6 to -inf gives 70 points:

```
raw 0x0000 = -inf   (digital mute; the display shows -65 dB)
raw 0x0003 = -62 dB ...  raw 0x16A0 = 0 dB ...  raw 0x2D41 = +6 dB
```

- **User-confirmed anchors**: top of the fader = +6 dB (0x2D41), bottom
  = -65 dB displayed = -inf (0x0000).
- **Independently cross-checked**: the scene-load capture wrote 0x0243
  for the AN1&#8594;AN1/2 fader at -20 dB — matches this table exactly.
- The old doc points ("-40 dB = 0x0317, -20 dB = 0x139E") were position
  estimates and are WRONG (0x0317 &#8776; -21 dB, 0x139E &#8776; -1.5 dB here).
- Bottom is ±1 dB uncertain (-62 vs -63 for 0x0003).
- Full table: see the `FADER_CURVE` constant in `tuxmix-core/src/usb.rs`
  (69 points, -62..+6 dB) and the raw list below.

## Output master (AN1/2) — exponential fit

The master sweep reached 0x4000 at +6 dB and 0x2000 at 0 dB (also the
scene-load value). Fit: **raw = 0x2000 · 2^(dB/6)** (exact doubling per
+6 dB), 0x0000 = -inf. `master_db_to_raw` in `tuxmix-core/src/usb.rs`.

Note: the crosspoint fader and the output master have DIFFERENT curves
(+6 dB = 0x2D41 vs 0x4000) — the `set_volume(Output)` path uses the
master curve, crosspoints/playbacks use the fader curve.

### The 8-bit master register is the REAL volume — HARDWARE-VERIFIED 2026-08-24

Live test (kernel driver): the 16-bit master (0x03E0+2·out) does NOT
change the output level (0x2000 &#8594; 0x0103 = no change); the 8-bit
companion (0x0004+2·out) DOES (0xF3 = full level, 0x73 = silence).
TotalMix writes both together; the 16-bit is a shadow (mute = 0x0000).

**8-bit scale: 0.5 dB per step, 0xF3 = 0 dB.** Derived from the AS1/2
master sweep (ctlout_as_test.txt): the 8-bit descends 1 per ~0.5 dB
(0xF3 &#8594; 0xDE = -10.2 dB over 21 steps). Bottom 0x73 = -64 dB
(silence), top 0xFF = +6 dB, mute code 0x3B. Formula:
`8bit = 0xF3 + 2·dB = 0xF3 + 12·log2(v16/0x2000)`.

`tuxmix-core/src/usb.rs` `set_volume(Output)` currently writes a
CONSTANT 0xF3 to the 8-bit register — the master volume is therefore
BROKEN in the user-space stack too (only mute works). Fix: derive the
8-bit code from the dB (see the kernel driver's `bf_master_8bit()`).

## Hardware output-level switch (+19 dBu / +4 dBu) — NOT in the protocol

A recessed slide switch on the UNDERSIDE of the unit sets the maximum
XLR output level to **+19 dBu (default) or +4 dBu** (manual §5.1,
2026-08-24 user-confirmed). This is a pure HARDWARE switch — no USB
writes, invisible to the protocol — but it shifts the ABSOLUTE output
level by **15 dB** between positions (the +4 dBu position is 15 dB
lower: recommended for sensitive amps/monitors, and lets the fader run
higher). It does NOT change the fader/master curves (those are
relative dB); it only matters when comparing absolute output levels
(dBFS vs dBu) across setups. Record which position the unit is in when
doing absolute-level measurements.

## Mic preamp gain — linear fit

The sweep 0&#8594;65 dB produced raw values **0..20** (5-bit field, top held).
Fit: **dB = raw · 3.25** (65/20), `gain_db_to_raw` rounds. The earlier
"2 dB/step (0-31 &#8776; 0-62 dB)" note was an unmeasured inference from the
field width — wrong. Raw 21-31 are unused (the knob maxes at 65 dB).

**Granularity re-verified 2026-08-23 (write-timing analysis)**: the
user's 1-dB steps each produce ONE 0x1A write, and consecutive steps
rewrite the **same raw 3-4 times** before it moves (raw 4 written at
t+0/2.5/5.0 s, raw 5 at +7.7 s — i.e. ~3.25 steps × 2.5 s pause per raw
change). So TotalMix's 1-dB display granularity is cosmetic: the
hardware has 21 real levels (3.25 dB each), and stepping 35&#8594;36 dB
rewrites raw 11. The exact displayed-dB &#8594; raw table (rounding vs
truncation at each raw) is part of the "perfect calibration"
(WINDOWS-CAPTURE-PLAN.md Capture 1) — TotalMix's display value for each
raw still needs one correlated spot-check.

## 48V / PAD

- `0x17 0x000D 0x003F` + `0x21` = 48V ON, `0x000C` = OFF (re-confirmed).
- **PAD confirmed on hardware (cap_padpan.pcap)**: bit `0x10` =
  PAD Mic 1 — `0x001D` = 48V ON + PAD vs `0x000D` = 48V alone. The
  PAD-only state (0x0010, 48V off) wasn't captured (the user toggled
  PAD while 48V was on) but the bit is proven by the 0x001D/0x000D
  pair.

## Pan

- **Mono inputs (AN1-4) emit NO pan writes** — confirmed again
  (cap_padpan.pcap: a full pan sweep on AN1 produced zero writes).
  `tuxmix-core`'s `set_pan` already skips mono inputs for this reason.
- **Pan law = linear in RAW (cap_pan_stereo.pcap, 2026-08-22)**: on a
  stereo pair, pan attenuates ONE side linearly from the current raw
  value to 0x0000 at 100%: `varied = fixed_raw · (1 &#8722; |pan|)` —
  exactly what `set_crosspoint_balance` implements (the code is
  already correct, no change needed). Captured on the AN1/2 output
  balance (0x03E0/0x03E1 one-sided sweeps, step 0x9C from 0x3F5C to
  0x0000). Full pan mutes the far side (0x0000 = -inf).
- **Bonus — Mute/CUE pattern (same capture)**: pressing MUTE/CUE
  writes the playback crosspoints (0x0042-0x0065 = PB2-6 &#8594; AN1/2)
  with 0x0000 (muted) or 0x2000 (active) — the 0x2000 flag on
  crosspoints is the per-channel active/mute marker. PB1 was the
  cued/source channel (kept).
- **CUE on a hardware OUTPUT (cap_cue.pcap, 2026-08-22, HARDWARE-
  VERIFIED 2026-08-23 via cuetest.c)**: pressing CUE on an output
  strip writes the GLOBAL CUE-bus crosspoint map (0x0040-0x0065 =
  PB1-6 L/R — same addresses for EVERY output, the CUE bus = the
  AN1/2 monitor path) toggling 0x0000 (CUE ON, playbacks muted) /
  0x2000 (CUE OFF, restored). One channel is EXCLUDED per output —
  the output's dedicated CUE source, kept at 0x2000: AN1/2&#8594;PB1,
  PH3/4&#8594;PB2, AS1/2&#8594;PB3, ADAT3/4&#8594;PB4, ADAT7/8&#8594;PB6 verified
  (capture order: AN1/2, PH3/4, AS1/2, ADAT3/4, ADAT7/8 = 5 cycles ×
  2 presses); ADAT5/6&#8594;PB5 predicted (source PB = out_idx+1). No
  hardware-INPUT CUE exists (user-confirmed). The CUE writes carry NO
  0xC000 transaction flag (bare wIdx). Note: the 0x10 0x05CF
  keepalive is suspended while TotalMix toggles (zero 0x10 writes in
  the capture).

## Fader raw values (L side, in capture order, first occurrence)

```
0x0000 = -inf
0x0003 0x0004 0x0005 0x0006 0x0007 0x0008 0x0009 0x000A 0x000B 0x000D
0x000E 0x0010 0x0012 0x0014 0x0017 0x0019 0x001D 0x0020 0x0024 0x0029
0x002E 0x0033 0x003A 0x0041 0x0049 0x0051 0x005B 0x0067 0x0073 0x0081
0x0091 0x00A3 0x00B7 0x00CD 0x00E6 0x0102 0x0122 0x0145 0x016D 0x019A
0x01CC 0x0204 0x0243 0x028A 0x02D9 0x0332 0x0396 0x0406 0x0483 0x0510
0x05AF 0x0660 0x0727 0x0807 0x0902 0x0A1B 0x0B57 0x0CB9 0x0E47 0x1004
0x11F9 0x142A 0x16A0 0x1963 0x1C7C 0x1FF6 0x23DC 0x283D 0x2D41 = +6 dB
```

## Verification on hardware (TODO on Linux)

Set a few known values with `probe vol`/`probe gain` and compare the
audible/measured level: e.g. fader 0x0243 should read -20 dB, raw 20
gain = 65 dB. Re-run `gainsweep_full.c` with the new gain table.
