# Windows capture plan — TuxMix (2026-08-22, revised)

## 🖥️ SESSION GUIDE — next Windows session (2026-08-26, the RE tail)

Setup: USBPcap on the Babyface hub, TotalMix open, slow deliberate
moves with 2-3 s pauses, one pcap per step in `tools/usbdump/`.
Everything below is OPEN — each pins a real protocol question from the
Linux driver work.

### Step 0 (PRIORITY, ~15 min) — the loopback 1:1 mechanism → `cap_lbsess.pcap` + `cap_lbph.pcap`
**2026-08-26 Linux finding this closes**: the kernel driver's loopback
record sits at a fixed −30.1 dB (2^-5) below the playback × master law,
while the Windows session (cap_lbcal.pcap) recorded the bus **1:1 at the
USB level**.  EVERY register comparable between the two sessions is
identical (0x15 map, 16+8-bit masters, init burst, crosspoints, preamp,
width, 0x05CF brute-forced) — so the difference is a state NOT captured
before.  Two captures:
1. **`cap_lbsess.pcap` — the FULL session start**: from a COLD TotalMix
   start (fresh launch), let it upload everything, then start playback
   with a steady tone and toggle the AN1/2 loopback on/off slowly.
   This captures the COMPLETE mixer state TotalMix writes at session
   start — the kernel driver's “factory default” is a hypothesis, not a
   capture; diffing the full upload against `babyface_write_default_mixer`
   may reveal the missing register (an input trim, a record gain, …).
2. **`cap_lbph.pcap` — the Phones loopback calibration**: PH3/4 loopback
   ON, Phones master at the TotalMix “0 dB” fader (0x1F17), tone at a
   known level (note it).  Pins the Phones record offset (±3 dB session
   drift + the −2.7 dB residual the kernel ×32 leaves).
Also note (no capture): whether the words 12/13 tap appears in the OUT
stream (the Windows OUT words 12/13 carried a sine — the WDM writes it;
the kernel writes 0).

### Step 1 (~5 min) — preamp readback index → `cap_preampread.pcap`
The kernel driver polls 0x17 at **wIdx=0x0000** and the probe syncs the
48V state from a **wIdx=0x003F** read.  On the live unit with 48V Mic1
ON: wIdx=0x0000 returns byte0 = **0x01** (raw bits), wIdx=0x003F
returns byte0 = **0x00** — and interleaving the two reads made BOTH
read 0x00 for a whole run (p48watch.c).  The doc's old "byte0 mirrors
the 0x0C-base write state" doesn't match.  Capture: toggle 48V AN1/AN2
on/off slowly, note the 0x17 readback bytes (index + value) in the
poll stream.  Pins the probe's fresh-boot preamp sync (phantom controls
would show OFF on a cold boot with no saved state).

### Step 2 (~5 min) — OUT selection base-mode encoding → `cap_outbase.pcap`
On the live unit the byte1 OUT field reads **0x00 at idle** (Front
Panel Out = Unknown until the first OUT press), then 0x04 Ch1/2 / 0x05
Phones / 0x06 Opt (gain-display mode, cap_dim).  An older capture
(cap_buttons) showed 0x01/0x02/0x00.  Capture: from a COLD TotalMix
start, press OUT 3 times slowly — write down the readback byte1 at
each step AND what the panel shows (the base-mode encoding is still
ambiguous).

### Step 3 (~8 min) — width strip ownership tail → `cap_width8.pcap` — **RESOLVED 2026-08-26 (no capture needed)**
cap_width3-7 mapped width registers per strip (AN1/2 0x0034/35,
AN3/4 0x0036/37, AS1/2 0x0038/39, ADAT7/8 0x003E/3F — all on the
Phones block-0 std map + the strip's low map).  The two open items:
1. **cap_width7's 0x011A/0x011B + 0x0134/0x0135 = PB6 (ADAT 7/8
   playback) width → out 4** (0x0104+22/23 L, 0x011E+22/23 R) — the
   playback pattern is PB_n → out(n−2) (PB1 special: out 0 + low map).
2. **The targets are FIXED per strip, NOT the viewed output**
   (user-confirmed: the submix view was unchanged during cap_width6,
   each strip wrote a different target).  AN3/4's extra block-4 write
   (cap_width4) did NOT reproduce in cap_width7 (same strip, hands
   off) → routing/state-dependent, not a fixed target.

### Step 4 (~5 min) — the 0x000A/0x000B gain pair → `cap_adcgain.pcap`
The front-panel wheel in gain mode writes **0x1A wIdx=0x000A** (a
SECOND gain address next to the 0x0000-0x0003 mic gains — registry
"ADC Gain"?) with values 0x0000→0x0005.  Capture: wheel up the AN1
gain via the panel, then via TotalMix — compare 0x1A writes to
0x000A/0x000B vs 0x0000.  Identifies what the panel wheel actually
controls.

### Step 5 (~5 min) — the panel-state echo writes → `cap_panelsstate.pcap`
TotalMix echoes the panel state as **0x17 wVal=<state> wIdx=0x2000**
(DIM ON = 0x2400, OFF = 0x0400, idle poll = 0x0000).  The wVal bit
layout (OUT position + DIM LED flag 0x2000) is not documented.  With
OUT cycling Ch1/2 → Phones → Opt and DIM toggling, write down each
0x17 0x2000 echo wVal — TuxMix needs it to replicate the panel LEDs.

### Step 6 (~10 min, NEW 2026-08-26) — the MIX-mode VU display law → `cap_mixdisp.pcap`
**Linux finding**: the MIX-mode input VU display is HOST-driven via the
front-panel display family — TotalMix mirrors the monitoring level as
`0x1A wVal=<disp> wIdx=0x000A+mic` (cap_mix.pcap: 0..5 while the fader
ran 0x0003..0x0117) and the card lights the input VU segments
accordingly (verified live on Linux with raw 0x1A writes).  The kernel
driver now writes a PROVISIONAL law (disp = (dB+62)/6, one step per
6 dB) — the capture pins the EXACT law:
1. **`cap_mixdisp.pcap`** — IN = Ch1/2, OUT = Ch1/2, SELECT = Left,
   press MIX, then sweep the wheel from the bottom (−inf) to the top
   (+6 dB) SLOWLY (1-2 s per click, ~30 clicks), pause, sweep back
   down, press SELECT (exit).  Parse: the `0x1A 0x000A` display values
   vs the `0x12` fader raws at each step → the fader→display mapping
   across the FULL range (the 6-point sample in cap_mix only covered
   the bottom ~34 dB).
2. Same pcap, repeat with SELECT = Right (0x000B writes) and Both
   (both), and with IN = Ch3/4 (0x000C/0x000D) — pins the per-channel
   register map.  Opt (AS1/2) if cheap: does MIX on Opt write a
   display value at all?
Also on Linux (no capture needed): a live segment-count calibration —
write 0x1A values (0..0x1F) and note how many VU segments light → the
display-value → segment mapping (the capture gives TotalMix's
fader→display choice; together they give the full law).

### Step 7 (optional, ~10 min) — the other open protocol items
- **EQ shelf HF warp** (`cap_eqshelf2.pcap`): the shelf sweep still
  diverges at HF (eq_warp*.py) — one slow labeled shelf gain sweep at
  20 kHz would pin it.
- **MIX B full write sequence** (`cap_mixb.pcap`): the MIX press burst
  (0x17 0x8480 0x8C80 + 0x1A on 0x000A/0x000B) per state.
- **Calibration spot-check**: the gain display at raw 0/17/23/31 (set
  the AN1 gain knob) — pins the exact raw→dB rounding.

### After the session: copy the pcaps INTO `SHARED/TuxMix/tools/usbdump/`
(the pcaps are gitignored and ONLY live on SHARED), then the Linux
pass (parse_usb.py → PROTOCOL.md) does the analysis.

⚠️ SYNC RULE (2026-08-26): push the repo with `tools/sync-shared.sh` —
NEVER `rsync --delete` (it erased 15 captures once).

---

## 🖥️ SESSION GUIDE — previous session (2026-08-25, ~35 min) — DONE

Setup: USBPcap on the Babyface hub, TotalMix open, tone on PB1 (or
whatever is needed per step).  Slow deliberate moves, 2-3 s pauses.
One pcap per step, saved in `tools/usbdump/`.  The gain table and the
fader/master curves are ALREADY captured (cap_calib) — no need to
redo the calibration sweeps.

### Step 1 (~8 min) — SELECT cycle + trim → `cap_select2.pcap`
1. Press **IN** → Ch1/2 selected.  Press **SELECT** ×4 slowly (~2 s
   apart, no wheel) — the readback shows L/R/both/none.
2. SELECT + wheel: press SELECT once, 3 wheel clicks, pause; repeat
   for the 4 SELECT states — the gain writes reveal the target.
3. Same with **MIX B** on (crosspoint targets per SELECT state).
4. **Trim**: AN1 fader fixed at 0 dB, drag the trim slowly -inf → +6
   dB in 1-dB steps; repeat at fader -14 dB and -16 dB.  This pins
   f(fader, trim) — currently a ×27/256 placeholder.

### Step 2 (~6 min) — DIM semantics → `cap_dim2.pcap`
(DIM behaved wrong on Linux: it didn't dim — this capture pins it.)
1. Phones master at 0 dB, -20 dB, -40 dB: press **DIM** each time
   (2-3 s apart) — read the written value (fixed floor vs X dB).
2. With OUT = Ch1/2, Phones, Opt selected: press DIM each time
   (per-output dim?).
3. Press **DIM** 2 s (Recall) — does it restore the pre-DIM volume?
4. Write down the physical LED behavior at each press.

### Step 3 (~5 min) — Windows system volume → `cap_sysvol.pcap`
1. TotalMix on the AN1/2 view, tone on PB1; move the Windows volume
   slider (**Speakers**) down ~10 dB slowly, back up.
2. Switch the view to PH3/4 WITHOUT touching anything: does PB1 show
   the same -10 dB (all submixes written) or 0 dB (only AN1/2)?
3. Move the slider again on the PH3/4 view — diff the 0x12 writes.

### Step 4 (~5 min) — front-panel ground truth (cap_buttons3.pcap)
For each: write down what you SEE (LEDs, VU, display):
1. **IN** cycling through the inputs; **OUT** cycling (Ch1/2 →
   Phones → Opt) — which LED + readback changes.
2. **SELECT** ×4 (L/R/both/none LEDs), **A SET** short, **A SET** 2 s
   (Recall), **MIX B** on/off.
3. **Wheel** in gain / fader / pan states.

### Write down (no capture)
- The gain display at raw **0 / 17 / 23 / 31** (set via the T-panel
  trim or the gain knob if it takes raw values — else note the
  displayed dB at the raw steps the knob lands on).
- Rate + clock source shown in the TotalMix toolbar.
- Whether this Windows unit is the SAME physical device as the Linux
  unit.

### Optional (~5 min) — AN3/4 gain range → `cap_gain34.pcap`
Sweep the **AN3** and **AN4** (Instrument) gains 0 &#8594; max in 1-dB
steps — cap_calib never touched them (raw range 0-20 vs 0-31
unconfirmed), and the kernel driver currently assumes 0-20 for all
four mics.

### Optional (~20 min) — the RE long tail (P6), one pcap each
MS proc with real signal (cap_ms2), Width strip ownership
(cap_width3), MIX B button (cap_mixb), submix + scene save/load
(cap_scene2), mute/solo per strip (cap_mutesolo2), AN3/4 48V+PAD
(cap_preamp34), low-cut (cap_lowcut), ADAT routing (cap_adat),
telemetry with audio (cap_telemetry).  Only to fully unlock
everything.

### After the session: rsync the pcaps to SHARED (`tools/usbdump/`),
and the Linux pass (parse_usb.py → PROTOCOL.md) does the analysis.

⚠️ SYNC RULE (learned the hard way, 2026-08-26): push the repo to
SHARED with `tools/sync-shared.sh` — it must NEVER use `rsync --delete`:
the pcaps only live on SHARED (gitignored), and a --delete push erased
15 fresh captures.  When re-copying captures from the Windows machine,
copy them INTO `SHARED/TuxMix/tools/usbdump/` — never rely on a push
for them.

---

## 🔴 MASTER TODO — what to capture on Windows next (2026-08-26)

**Session 2026-08-26**: see the SESSION GUIDE at the top (preamp
readback index, OUT base-mode, width tail, 0x000A/0x000B, panel-state
echo — the RE tail for the kernel driver).  Status of the items below:
cap_dim2 (P4), cap_sysvol (P5), cap_ms2, cap_width3-7 = DONE;
cap_select2 (P2 trim formula), cap_buttons2 (P3), P1 calibration
spot-checks, P6 long tail = partially open.

Everything below is still OPEN unless marked.  Each capture: USBPcap
running on the Babyface hub, TotalMix open, slow deliberate moves with
2-3 s pauses, save as its own pcap in `tools/usbdump/`.  A ~45 min
session covers priorities 1-3; add 4-5 for another ~30 min; 6 is the
long tail (do it only to fully unlock everything).

### P1 — PERFECT CALIBRATION (the #1 ask, ~12 min) — Capture 1 (cap_calib.pcap)
1. **Gain table**: set the AN1 mic gain 0 → 65 dB in 1-dB steps
   (pause 2-3 s each), down 65 → 0 (hysteresis check).  Note the
   displayed dB at raw 0 / 17 / 23 / 31.  (The raw→dB is ~raw·3.25
   but the exact displayed-dB → raw rounding table is open.)
2. **Input fader**: drag AN1 → AN1/2 from -inf, then -60 → -20 in
   5-dB steps, then -20 → +6 in 1-dB steps (pause 2 s each).  Keep
   both the standard map (0x0034/0x004E) and low map (0x0000+)
   writes.
3. **Output master AN1/2**: same sweep on the AN1/2 master, then
   MUTE on → pause → off (the mute pattern).
4. **Pan with a STEREO source**: route a playback (PB1) → AN1/2 and
   drag its pan -100 → +100 % in 10 % steps (mono-input pan writes
   nothing).

### P2 — SELECT cycle + trim formula (~8 min) — Capture 13 (cap_select2.pcap)
1. Press IN → Ch1/2 selected.  Press SELECT ×4 slowly (~2 s apart,
   no wheel) — the readback shows L/R/both/none.
2. SELECT + wheel: press SELECT once, 3 wheel clicks, pause, repeat
   for 4 states — the gain writes reveal the target per state.
3. Same in MIX mode (crosspoint targets per SELECT state).
4. **Trim formula**: fix the AN1 fader at 0 dB, drag the T-panel trim
   slowly -inf → +6 in 1-dB steps; repeat at fader -14 dB and -16 dB.
   Pins f(fader, trim) (currently a ×27/256 placeholder).

### P3 — FRONT-PANEL buttons + wheel (~10 min) — Capture 9 (cap_buttons2.pcap)
For EACH action, write down what you SEE (LEDs, VU-meters, display) —
the visible state is the ground truth:
1. Press **IN** cycling through the inputs; **OUT** cycling
   (Ch1/2 → Phones → Opt); note which LED lights + the readback
   changes.
2. Press **SELECT** a few times (the L/R/both selection LEDs).
3. Press **A SET** once; press it **2 s** (the Recall).
4. Press **MIX B** on and off (LED + which fader the wheel then
   controls).
5. Turn the **wheel** in every state (gain / fader / pan per state).
6. Press **DIM** on/off (see P4 for the detailed DIM capture).

### P4 — DIM semantics (~6 min) — Capture 14 (cap_dim2.pcap)
1. Phones master at 0 dB / -20 dB / -40 dB: press DIM each time —
   read the written value (fixed floor vs current-X dB).
2. The 0x17 0x2400/0x0400 0x2000 write: which readback byte/bit it
   sets (the DIM LED?).
3. DIM with OUT = Ch1/2, Phones, Opt selected (per-output dim?).
4. DIM 2 s hold (Recall) — the restore = pre-DIM volume?

### P5 — SYSTEM VOLUME: which crosspoints the slider moves (~5 min) — Capture 16 (cap_sysvol.pcap)
1. TotalMix on the AN1/2 view, tone on PB1; move the Windows volume
   slider for **Speakers** down ~10 dB slowly, back up.
2. Switch the view to PH3/4 WITHOUT touching anything: does the PB1
   fader show the same -10 dB (all submixes written) or 0 dB (only
   AN1/2)?
3. Move the slider again on the PH3/4 view — diff the 0x12 writes.

### P6 — the RE long tail (Capture 5+ checklist, ~30 min)
Only to fully unlock everything.  One pcap each:
- **MS proc with real signal** (cap_ms2.pcap): AN2 mic connected +
  routed to the phones, toggle MS — what the mute cuts.
- **Width on the 0x00AE strip — RESOLVED 2026-08-26 (cap_width3-7)**:
  the 0x00AE/0x00AF family = PB4 (ADAT 3/4 playback) width into out 2;
  every strip has a fixed width target (HW inputs/PB1 → out 0 + low
  map; PB2-6 → out n−2).  Full table in PROTOCOL.md.
- **MIX B button** (cap_mixb.pcap): the host writes on toggle.
- **Submix mode + scene save/load** (cap_scene2.pcap).
- **Mute/solo per strip type** (cap_mutesolo2.pcap).
- **AN3/4 48V + PAD + gain** (cap_preamp34.pcap).
- **Low-cut toggle** (cap_lowcut.pcap).
- **SPDIF / ADAT routing** (cap_spdif.pcap / cap_adat.pcap).
- **Telemetry with audio** (cap_telemetry.pcap): 480-B ep 0x85 with
  audio flowing vs idle — find the output meter fields.
- **Unmapped registers**: the 0x17 wVal=0 wIdx=0x8080/0xF040/0x2000
  writes, the 0x10 0x3000/0x0800 family, the byte-2 counter
  (0x41/0x43/0x4F/0x80) — one capture while toggling things.

### Write down (no capture):
- The rate + clock source shown in the TotalMix toolbar.
- Whether the Windows unit is the SAME physical device as the Linux
  unit.
- The gain display at raw 0 / 17 / 23 / 31.

### After each capture: rsync the pcap to SHARED (tools/usbdump/),
then a Linux pass with parse_usb.py updates PROTOCOL.md.

---

**Status:** the 48V/PAD/gain/audio-chain work is DONE and hardware-
verified on Linux (see HANDOFF.md). The Windows captures that remain
are for **calibration** (raw → dB tables), **EQ/FX coefficients**, the
**higher sample rates**, and the (now non-blocking) session-start
curiosity. The old "0x41 → 0x80 blocks 48V" framing is obsolete — 48V
engages with or without a valid session.

## Capture 13 (PRIORITIZED, 2026-08-24): the LAST RE gaps — SELECT cycle + trim

These are the only two protocol unknowns left. Two short captures
(~2 min each), slow + paused so the analysis can segment the states.

### 13a. SELECT cycle (L/R/both/none) — what the wheel targets per state

On Linux the SELECT state is NOT readable in the `0x17` readback
(confirmed: byte2 = the wheel counter only), so TuxMix keeps a
host-side counter that can drift from the card's LED display. This
capture pins the REAL cycle + the per-state wheel targets.

1. Press **IN** until Ch1/2 is selected (top LED).
2. Press **SELECT ×4 slowly** (~2 s between each press, NO wheel
   between) — the readback sequence shows whether L/R/both/none is
   encoded anywhere (byte2 low nibble? byte0? byte1? a write?).
3. **SELECT + wheel**: press SELECT once, then turn the wheel ~3
   clicks, pause, repeat for 4 SELECT states. The `0x1A` gain writes
   reveal the target per state: AN1 = wIdx 0x0000, AN2 = 0x0001, both
   = both regs, deselected = no write (or something else?).
4. Same in **MIX mode** (press MIX, then SELECT + wheel): the `0x12`
   crosspoint targets per SELECT state (L/R/both → which of
   AN1/AN2→out get written).

Analysis: label each `0x1A`/`0x12` write with the SELECT state that
produced it → align the TuxMix `SelectState` cycle + the
`select_targets` mapping to the hardware. Save as
`cap_select2.pcap`.

### 13b. Trim — pin f(fader, trim) with a controlled sweep

The trim (T panel) writes the SAME crosspoint registers as the fader
on the linked AN1/2 strip (all 8 regs; display "0" = low 0x2000).
The standard-map value depends on the FADER position (×0.1055 at -16 dB
fader vs ×0.14-0.24 at -14 dB) — `set_trim` uses ×27/256 as a
placeholder. A controlled capture pins the exact relation:

1. **Fix the AN1 fader** at a known position (e.g. -16 dB — write it
   down, do NOT touch it during the sweep).
2. Drag the **trim** slowly in ~1 dB steps (2 s pauses), full range
   up then down.
3. Repeat with the fader at **0 dB** (one more sweep).

Analysis: correlate the low-map value (0x2000·2^(dB/6), the trim
curve) vs the standard-map value at each step, per fader position →
f(fader, trim). Save as `cap_trim3.pcap`.

## Capture 1 (PRIORITIZED): PERFECT CALIBRATION — cap_calib.pcap

Pins every register value to the TotalMix dB display. **Take your time —
slow, deliberate moves with pauses; the analysis reads the write
sequence back against the values you set.** ~10 min total.

Setup: USBPcap running on the Babyface's hub, TotalMix open, the AN1
input strip selected, nothing else touched.

### A. Mic gain, 1-dB steps (0x1A → wIndex 0x0000)
1. Set the mic gain knob to **0 dB**, pause 3 s.
2. Step **+1 dB at a time up to 65 dB**, pausing ~2-3 s at each
   (66 settings, ~3 min). This catches EVERY raw code incl. rounding
   (if TotalMix shows 1-dB resolution on a 2-dB/step register, we'll
   see which raw each displayed value maps to).
3. Step back down 65 → 0 (same cadence) to check for hysteresis.
4. Watch for the preamp block: every change rewrites `0x17` (preamp
   state) + `0x21` + 4× `0x1A` (gains mic 1-4).

**Display scale (user-confirmed 2026-08-22):** the gain knob shows
0 → 65 dB; the raw field is 5 bits (0-31) so the step is ≈ 2.03 dB
(the sweep pins the exact table incl. rounding). The fader shows
**-65 dB (= -inf, digital mute) → +6 dB max** — so fader raw 0x0000 =
mute/-inf, and the +6 dB end is the curve's top (likely not 0xFFFF).

**RESOLVED 2026-08-23**: the sweep produced raw 0-20 only (21 levels),
fit dB = raw·3.25. The 1-dB display steps rewrite the same raw 3-4×
(write-timing verified) — TotalMix's 1-dB granularity is cosmetic. The
exact displayed-dB → raw table (rounding vs truncation per raw) is the
one thing still open; correlate by setting a few known display values
and reading the raw (or the persisted state after reboot).

### B. Crosspoint fader AN1 → AN1/2 (0x12 → 0x0034/0x004E, low map 0x0000+)
1. Drag the AN1 → AN1/2 fader to **-inf** (bottom), pause 3 s.
2. Up in **5-dB steps**: -60, -55, ..., -25, pause 2 s each.
3. Then **1-dB steps** -20 → +6: -20, -19, ..., 0, +1, ..., +6,
   pause 2 s each (this nails the tapered top of the curve).
4. Note: TotalMix may also write the low-map mirror
   (`0x0000+idx` / `0x001A+idx`) — both get captured, keep both.

### C. Output master AN1/2 (0x12 → 0x03E0/0x03E1 + 0x1A → 0x0004/0x0005)
1. Same sweep as B on the AN1/2 output master fader (-inf → +6).
2. Then click **MUTE** on the master, pause 3 s, un-mute (captures
   the mute pattern: 0x1A 0x003B + 0x12 0x0000).

### D. Pan (bonus)
On the AN1 → AN1/2 crosspoint, drag the pan knob -100% → +100% in
~10% steps, pause 2 s each (maps the "vary one side" writes).

### E. 48V / PAD confirmation
Toggle 48V on/off and PAD on/off once each (confirms the state bytes
match our hardware findings: 0x0D/0x0C 48V, 0x1D/0x1C PAD).

**DONE (cap_padpan.pcap, 2026-08-22)**: PAD = bit 0x10 confirmed
(0x001D = 48V+PAD vs 0x000D = 48V alone). Pan on a MONO input emits
no writes — pan calibration needs a stereo source (Playback x → AN1/2
or AS1/2 → AN1/2).

Save as `tools/usbdump/cap_calib.pcap`.

## Capture 2 (PRIORITIZED): EQ/FX coefficients — cap_eq2.pcap

The EQ transport is known (bulk ep 0x0A, 64-byte blocks, 5 × 32-bit
values + footer) but the coefficient format is NOT decoded.

1. USBPcap running, TotalMix open, AN1 selected.
2. Toggle the channel EQ **on** — pause 3 s.
3. Drag the **low gain** knob in small steps (0, +3, +6, -3, -6, 0 dB),
   pausing ~2-3 s at each.
4. Drag the **low frequency** knob (60, 100, 200, 400, 800 Hz),
   pausing ~2 s each.
5. Drag the **high gain** and **high frequency** similarly.
6. If there is a parametric band, drag **gain / frequency / Q** each in
   small steps.
7. Toggle the EQ off. Save as `tools/usbdump/cap_eq2.pcap`.

Analysis: diff the 0x0A bulk payloads (64-byte blocks) between EQ
states — the changed words are the coefficients; the sweeps give
value→coefficient curves. (`eqcal_bulk.txt`/`eqcal2_bulk.txt` are
prior extractions.)

## Capture 2b (PRIORITIZED, 2026-08-23): EQ SYSTEMATIC LABELED SWEEP — cap_eq8.pcap

**This is the capture that finally unlocks the EQ coefficient format.**
The existing cap_eq3-7 captures lack per-state parameter labels, so the
fit cannot correlate the 5 coefficients with (gain, freq, Q, type). The
Linux analysis (eq_extract.py / eqfit.py / eqshape.py) established:

- The 64-byte block = header (byte1 = low-cut slope 0x01/03/07/0F) + 3
  band slots (5 × 32-bit at 0x04/0x14/0x24 + a shared value @0x34) +
  the low-cut freq @0x38. Coeffs ramp then settle (last block of each
  >1 s group = the settled state).
- The +6/-6 dB swap proves the storage order (b0, b1, a1, a2, b2) and
  that the GAIN is applied separately (the DC response doesn't move
  between +6/-6). The gain→coefficient mapping is NOT the RBJ cookbook.

**Procedure — ONE band at a time, announce or note EVERY value, pause
≥2 s after each change (the pauses delimit the settled states):**

1. EQ on, band 1 (Low) type = **bell**. Set Q = 0.7, gain = +6 dB.
   Sweep freq: 50, 100, 200, 400, 800, 1600, 3200, 6400, 10000 Hz —
   pause 2-3 s each. (`cap_eq8a.pcap`)
2. Same band, freq = 200 Hz, gain = +6. Sweep Q: 0.7, 1, 1.5, 2, 3, 4,
   5 — pause each. (`cap_eq8b.pcap`)
3. Same band, freq = 200 Hz, Q = 0.7. Sweep gain: -20, -15, -10, -6,
   -3, -1, 0, +1, +3, +6, +10, +15, +20 dB — pause each. (`cap_eq8c.pcap`)
4. Band 1 type = **shelf**: repeat steps 1-3 (freq/Q/gain sweeps).
   (`cap_eq8d.pcap`)
5. Band 3 (High) bell: repeat steps 1-3. Band 2 (Mid, bell-only):
   repeat. (`cap_eq8e.pcap`)
6. TWO bands active (Low bell 200 Hz Q1 +6 AND High bell 2 kHz Q2 -6):
   confirms slot independence. (`cap_eq8f.pcap`)
7. Write down the TotalMix toolbar state (sample rate!) before each
   sweep — the coefficients are sample-rate dependent.

Analysis: for each settled state, the (gain, freq, Q, type) is KNOWN;
a straightforward nonlinear fit then identifies the exact formula.

## Capture 2c (2026-08-23): low-cut freq→0x38 constant

cap_eq7 showed 0x38 ≈ the individual pole frequency (slope ratios
match the cascaded-pole compensation model within ~3%). The last
unknown is the exact freq→0x38 constant (fs 44.1 vs 48 k? ω vs tan?).
Labeled sweep: low cut on, 12 dB/oct, set the freq to 20, 30, 50, 75,
100, 150, 200, 300, 500 Hz (pause 2 s each), note the values.
(`cap_eq9.pcap`)

## Capture 11 (2026-08-23): Linux-session open items — one pcap each

These controls were implemented from the Windows session but could not
be FULLY validated on Linux (see LINUX-VALIDATION.md §9). Each needs a
focused TotalMix capture:

1. **Loopback OFF sequence** (`cap_loopback_off.pcap`): ON ch 0/1 then
   OFF — the Linux probe showed `0x15 0x0000` on ch 0/1 alone does NOT
   always disengage (only a full 30-channel clear works). Capture the
   exact OFF write TotalMix sends (same channel? all 30? a commit?).
   ✅ DONE (2026-08-23) + set_loopback fixed to clear all 30.
2. **MS proc** (`cap_ms2.pcap`): toggle MS proc with the AN2 mic
   connected and routed to the phones — confirm what the mute actually
   does (and whether it cuts the AN1>2-routed signal or only the
   physical AN2). The Linux ear test failed because the AN2 analog
   input delivered no signal.
3. **Width strip mapping** (`cap_width2.pcap`): drag the Width knob on
   DIFFERENT strips (PB1, PB2, AN1, AS1/2...) and see which writes the
   4 register pairs (0x00AE/0x00AF, 0x00C8/0x00C9, 0x0046/0x0047,
   0x0060/0x0061). On Linux the 4 pairs did NOT affect PB1 — they
   belong to another strip/function.
   ✅ PARTIAL (2026-08-24): AN1/2-family strip = the LOW MAP 0x0000-
   0x001B + standard 0x0034/0x0035 (at ~1/10 scale — see PROTOCOL.md);
   **RESOLVED 2026-08-26 (cap_width3-7): the 0x00AE family = PB4
   (ADAT 3/4 playback) width into out 2** — the playback pattern is
   PB_n → out(n−2) (full table in PROTOCOL.md).
4. **FX send dB curve** (`cap_fx3.pcap`): drag the FX send slider in
   1 dB steps and correlate the 0x12 0x0138/0x0153 values (the
   observed ramp was 0x000C → 0x1000; the dB curve is uncalibrated).
   ✅ DONE (2026-08-24): fader-curve shape at 0.5-dB steps, top
   clamped to 0x1000 = 0 dB display; set_fx_send fixed (was writing
   beyond the register max).
5. **Ref level 3-state bit map** (`cap_reflevel.pcap`): on Instr 3/4,
   cycle +4dBu → -10dBV → Boost (pause 2 s each) and capture the 0x17
   0x003F values — only 0x0000 ↔ 0x000C were observed (2 of the 3
   states); the third value + the exact bit map are missing.
   ✅ DONE (2026-08-24, labeled re-capture `cap_reflevel2.pcap`):
   +4dBu = (0x17 0x000F, 0x21 0x0000), -10dBV = (0x0003, 0x0000),
   Boost = (0x0003, 0x0003). `set_ref_level` updated to the labeled
   pairs (REF_PLUS_4DBU/REF_MINUS_10DBV/REF_BOOST codes).
6. **Stereo split bus pattern** (`cap_split2.pcap`): enable Stereo
   split on PB1 — the Linux test confirmed 0x2000/0x0000 into out0,
   but the "alternating per mono channel" description suggests the
   split channels go to DIFFERENT buses; capture which crosspoints
   (outputs) the split rewrites.
   ✅ DONE (2026-08-24): the split TOGGLE writes NOTHING (zero USB
   writes across split/re-link ×3, user-observed pan 100/100 →
   center). The cap_ctrl3 rewrite happens on the next fader drag.
   Split = host-side state; set_stereo_split is consistent.
7. **Trim (T) slider** (`cap_trim2.pcap`): drag the T-panel trim in
   1 dB steps and correlate with the crosspoint values (cap_trim.pcap
   exists but unlabeled) — same curve as the master fader?
   ✅ DONE (2026-08-24, labeled): on the LINKED AN1/2 strip the trim
   writes ALL 8 registers (low map 4 + standard 4), the display "0"
   = low 0x2000 (master curve), bottom = 0x0000. ⚠️ The standard-map
   value depends on the FADER position too (×0.1055 at -16 dB fader,
   ×0.14-0.24 at -14 dB) — formula f(fader, trim) OPEN; `set_trim`
   uses ×27/256 as a documented placeholder. A controlled capture
   (fixed fader, slow sweep) would pin it.

## Capture 12 (2026-08-23): AN2 analog input check

On the Linux unit the AN2 XLR delivered NO signal (48V + gain read
correctly, PAD relay clicks, but the mic — which works on AN1 — is
silent on AN2). On Windows: plug the mic into AN2, set 48V + gain in
TotalMix, speak — if it works there, the Linux test was a bad
connection; if it is also silent, the AN2 preamp/jack is suspect
(hardware, not protocol). Note the result.

## Capture 3: sample-rate change — cap_rates.pcap — **SUPERSEDED by cap_rates2**

**RESOLVED (2026-08-22)**: rate changes are PURELY `SET_INTERFACE(5,
alt N)` with NO vendor writes. The old per-change table below was based
on the user's reported order, which the clean sweep (cap_rates2.pcap)
proved wrong — the alts are a BANDWIDTH CLASS, not a rate code, and
only 3 alt transitions fire across the whole 9-rate sweep:

| Alt | Packet | Rates |
|---|---|---|
| 1 | 448 B | 32 / 44.1 / 48 / 64 / 88.2 kHz |
| 2 | 640 B | 96 / 128 kHz |
| 3 | 1024 B | 176.4 / 192 kHz |
| 0 | 64 B | unused |

See PROTOCOL.md "Sample rate / clock" for the full analysis incl. the
`0x11` alt-verification hook (byte 3 = 2^alt). The `0x1B` block stays
for cold start only; mid-session rate change = SET_INTERFACE only.
The `0x10 0x0030` cycle + `0x10 0x05CF` keepalive are periodic, not
rate-specific. The 480-B telemetry carries only meter fields.

## Capture 4 (RESOLVED, 2026-08-22): the 0x41 → 0x80 transition

The long-sought `0x17` byte 2 = 0x80 state was captured in
cap_clk.pcap: **it is the CLOCK NO-LOCK state** (clock source = Optical
with no lock; Internal = 0x40). Not a session/stream flag. See
PROTOCOL.md "Clock source / no-lock state".

## Capture 6 (DONE, 2026-08-22): Fireface USB Settings settings — cap_fus.pcap

Captured (300 s). The app behavior is now clear:

- **Rate change = SET_INTERFACE only** (48→96→48 in this capture:
  96 kHz → alt 2, 48 kHz → alt 1 — the two ground-truth anchors).
- **Buffer size / WDM Devices = host-side only** (zero USB traffic).
- **Settings changes re-upload the register file**: bursts of `0x1B`
  writes (the 4-bank DSP register map). Two big bursts (~5604 s:
  120 writes, ~5645-5660 s: 1200 writes, ~5664 s: 244 writes) +
  several small 4-write "clock quads" at ~5620-5638 s.
- **The `0x1B` quad structure (NEW)**: writes come in groups of 4 with
  `wIdx = (address << 8) | bank` (bank 0-3, low byte of wIdx). The
  48 kHz clock quad (identical to the coldplug init block!):
  `0x1B 0xC350 0x0000`, `0x1B 0x8DB8 0xD201`, `0x1B 0x8234 0xD302`,
  `0x1B 0x7CFF 0xF803`. The last (bank 3, addr 0xF8 = 0x7CFF) is the
  constant terminator of every quad. The full dumps are the complete
  banked register file (100→337 regs per dump, growing across bursts).
- **The quads = PITCH/varispeed (DDS)**: the bank-0 value is a
  **divisor**: DDS ≈ 50000 / (1 + pitch/100) (hardware-verified
  2026-08-23: rate × DDS = const; the old labels were sign-flipped —
  0xBBCC = +4.00% (tone UP, rate +4.1%), 0xCB72 = −4.05% (tone DOWN,
  rate −3.9%)). The quad must be followed by the clock keepalive
  `0x10 0x0001 0x05CF` (cap_fus2.pcap does this after every quad).
- **Clock source = NO writes** (cap_clk.pcap): only the keepalive
  `0x10 0x05CF` wVal 0x0001→0x0004 + 0x17 byte 2 0x40→0x80 (no lock).

**Open**: the big dumps at ~5604/5645-5660/5664 s (the app's full
state re-sync — when it opens/OKs — not needed to change a setting)
and the SPDIF-In TMS keepalive bit (probably 0x0020, never captured).
The keepalive flags 0x0401 (optical SPDIF) and 0x0041 (EQ for Record)
are CONFIRMED (cap_opt.pcap / cap_eqr.pcap).
**RESOLVED (2026-08-24, cap_fus3/4/5)**: the one-setting-at-a-time
campaign proved the settings write the KEEPALIVE FLAG WORD only
(0x0004 clock / 0x0401 SPDIF / 0x0041 EQ-record; **TMS = no USB write
in driver v1.276** — host-side or defunct). A fresh app launch does
NOT re-upload the dump (driver caches the state); the big dumps are a
cold-start re-sync snapshot, redundant with the decoded writes. No
per-setting 0x1B registers exist. Remaining: the snapshot register
semantics (completeness only).

**Fader absolute-anchor question (2026-08-23, user report — VERIFY
ON WINDOWS)**: on Linux the user reports the fader at the **0 dB
display position (raw 0x16A0) sounds ~6 dB lower than the mic's
natural level**, while +6 dB (0x2D41) sounds like unity. The relative
law is verified (0x16A0 → 0x2D41 = exactly +6.02 dB; 1-dB steps
clean) and the -20 dB anchor (0x0243) cross-checks with the scene
capture — but the ABSOLUTE 0 dB anchor needs a dB reference.
Verification: in TotalMix set the AN1→AN1/2 fader to exactly 0 dB
and capture the raw (expect 0x16A0 per cap_calib); then A/B by ear
against the source. If TotalMix's 0 dB really is 0x16A0 AND sounds
like unity there, the anchor is right and the Linux discrepancy is a
monitoring-path offset (master/headphone), not the curve.

## Capture 7 (DONE, 2026-08-22): clean rate sweep — cap_rates2.pcap

The sweep (48→32→44.1→64→88.2→96→128→176.4→192→48, user-confirmed
order) produced only 3 SET_IFACE — proving the alt is a bandwidth class,
not a rate code. **COMPLETE alt table**: alt 1 (448 B) = 32/44.1/48/64/
88.2 kHz, alt 2 (640 B) = 96/128 kHz, alt 3 (1024 B) = 176.4/192 kHz.
The 0x11 readback encodes the alt (2^alt in byte 3) — usable as a Linux
verification hook. See PROTOCOL.md "Sample rate / clock".

**Still open**: the per-setting registers (clock source, pitch, optical
out, EQ-for-Record, SPDIF TMS) — the 0x1B quads/dumps seen in
cap_fus.pcap / cap_fus2.pcap were not yet attributed to specific
settings. A one-setting-at-a-time capture (each action announced or well
paused) is the plan when the user is up for it.

## Capture 8 (DONE, 2026-08-23): PITCH SLIDER SWEEP — cap_pitch.pcap

A full drag (0 → +5 → -5 → 0 %, 454 quads) decoded the quad: the DDS
is **16.8 fixed point** (wVal = int, wIdx high byte = fraction — NOT
an address), bank1 = round(DDS16 × 0.72562), bank2 = round(DDS16 ×
2/3) — exact on all 454 quads; bank3 is quantized (lookup from the
sweep). The wIdx high bytes of banks 1-3 (their fractions) are not
yet derived — Linux test: send exact 16-bit parts with frac = 0 and
verify the pitch applies. **Pitch shifts the sample rate dynamically**
(the DDS drives the device clock: +5% = 50400 Hz at nominal 48k). See
PROTOCOL.md "Pitch / varispeed".

## Capture 9 (PRIORITIZED): FRONT-PANEL BUTTONS + WHEEL — cap_buttons.pcap

Physical controls on the Babyface Pro FS front panel (user-confirmed
layout): buttons **IN, OUT, A SET, MIX B** (top row) + **SELECT, DIM**
(bottom) + the **wheel** (rotary encoder, push = select in the device
menu). The MIX B / A SET / IN / OUT buttons control TotalMix-level
features, so they almost certainly send host events; the wheel drives
the device's own menu (possibly host-visible too, e.g. volume mode).
Currently ZERO front-panel captures exist — this is a blind spot.

USBPcap running on the Babyface's hub, TotalMix open and idle, ~5 min:

1. **Each button, one press**: IN, OUT, A SET, MIX B, SELECT, DIM —
   press once, pause 3 s, press again (toggle). Note the front-panel
   LED/menu changes for each.
2. **Holds**: hold each of IN/OUT/A SET/MIX B ~2 s (some buttons
   behave differently on long-press).
3. **Wheel, slow**: one click at a time CW, pause 1 s ×10, then CCW ×10
   (note: in the default menu state, does it change anything visible?
   what does the display show?).
4. **Wheel, continuous**: one fast CW rotation, then one fast CCW.
5. **Wheel push**: press the wheel (menu enter), rotate in the menu,
   press again (back).
6. **Combos**: IN+OUT together, SELECT+DIM together (if the firmware
   has key combos).

What to watch (Linux analysis): any device→host traffic on the status
endpoints (the 9-byte ep 0x82 stream that flows on the Windows unit but
never on ours — see PROTOCOL.md), the 480-B telemetry (ep 0x85) for
button/encoder fields, or new control reads/writes. Negative result is
informative too: if the buttons produce NO traffic, they are handled
entirely device-side and TuxMix just needs to replicate the resulting
state (e.g. MIX B = a settings bit). Also check: does the wheel/menu
state survive in a register read (0x17/0x11/0x1C/0x1E/0x1F)?

## Capture 10 (DONE, 2026-08-23): session restore — cap_restore2.pcap

A TotalMix relaunch (device NOT replugged) re-uploads the MIXER from
its last SAVED snapshot (slot Mix 1; unsaved changes lost on kill):
0x15 clears → 0x17 0x8080 → per-output blocks (master 0x2000 + unmute
0x00F3 + all crosspoints; default = PB1-6 → AN1/2 at 0x2000, inputs
0x0000) → 0x17 0x000D 0x003F (48V, from the card's EEPROM) → 0x17
0xF040. NO gain writes, NO 0x1B clock quad (clock only at coldplug),
NO keepalives. See PROTOCOL.md "Session persistence".

## Capture 5+: COMPLETE RE CHECKLIST (the goal is to RE everything)

Systematic captures to fully unlock the device. Each item: the TotalMix
action + the writes to watch. Do them with USBPcap running and pause
2-3 s after every move. Save each as its own pcap in tools/usbdump/.

### Routing / mixer
- **Every output bus**: AN1/2, **PH3/4 (phones)**, **AS1/2**, SPDIF,
  ADAT — drag an input fader into each output and read the
  crosspoint address (0x34 + 0x34·out + idx formula must be verified
  for EVERY out). `cap_buses.pcap`.
- **Playback strips**: drag playback 1/2 → AN1/2 (and → each bus) —
  the playback crosspoint map. `cap_playback.pcap`.
- **Low map** (0x0000+idx / 0x001A+idx): which fader moves write it vs
  the standard map — click different OUTPUT strips and drag the same
  fader (the trigger for low→standard flip). `cap_lowmap2.pcap`.
- **Submix mode** + **scenes**: switch submix on/off; save/load
  several TotalMix scenes (the full mixer-state dump writes).
  `cap_scene2.pcap`.
- **MIX B button** (front panel): toggle it — what host writes
  follow? `cap_mixb.pcap`.
- **Front-panel buttons** (IN, OUT, A SET, MIX B, SELECT, DIM, the
  wheel): press each — the device may send button events (a status
  stream or register) to the host. `cap_buttons.pcap`.
- **Mute/solo per channel** on every strip type (input, playback,
  output). `cap_mutesolo2.pcap`.
- **Stereo/split button** (split AN1/AN2 into two mono strips).
  `cap_split.pcap`.
- **Loopback**: the output → record-path loopback toggle.
  `cap_loopback.pcap`.

### Preamp
- **AN3/AN4 48V + PAD**: toggle 48V/PAD on inputs 3 and 4 (bits 2-3 /
  6-7 of the 0x17 state byte are presumed — confirm). `cap_preamp34.pcap`.
- **Mic 3/4 gain** knobs (the 0x1A wIdx 0x0002/0x0003).

### EQ / FX
- The EQ capture (Capture 2 above) + the per-channel EQ **on/off**
  flags and the **low-cut** toggle. `cap_eq2.pcap` / `cap_lowcut.pcap`.

### Clock / rates
- The rate-change capture (Capture 3) + **clock source** switch
  (Internal/SPDIF/ADAT). `cap_rates.pcap` / `cap_clocksrc.pcap`.

### Digital I/O
- **SPDIF**: plug a source/sink, change the SPDIF routing, toggle
  Pro/consumer. `cap_spdif.pcap`.
- **ADAT**: 8-channel routing. `cap_adat.pcap`.
- The bulk endpoints 0x0A (EQ/FX out), 0x85 (telemetry in), and the
  unknown 0x86/0x88/0x09/0x8B probes — note when they're used.

### Status / telemetry
- The 480-B telemetry (ep 0x85) with audio flowing vs idle — find the
  **output meter** fields (the current candidate words 24-27/56-59/80/
  92-93/108-109 from cap_audio). `cap_telemetry.pcap`.
- The 9-byte status stream on ep 0x82 (flows always on Windows' unit,
  never on the Linux unit — why?).

### Unmapped registers
- `0x17` wVal=0 wIdx=0x8080 / 0xF040 / 0x2000 (around preamp/state
  writes) — role unknown.
- The 0x10 family (0x3000, 0x0800 0x0800) and the byte-2 counter
  (0x41/0x43/0x4F/0x80) meanings.
- The 0x1C/0x1E/0x1F/0x11 status registers beyond the poll cycle.

Each capture gets one focused analysis pass on Linux (parse_usb.py)
and the findings land in PROTOCOL.md + the code. Mark the checklist
items [x] as they're decoded.

## No-capture data points (write them down)

- The sample rate **and clock source** shown in the TotalMix toolbar
  (e.g. "48.0 kHz / Internal").
- The mic gain value TotalMix displays at the RAW values 0, 17, 23, 31
  if easy to reach (a direct spot-check of the gain table).
- Whether the Windows unit is the SAME physical device as the Linux
  test unit.
- **Front-panel behavior (Capture 9)**: what each button/wheel action
  does VISIBLY (LEDs, menu, display) — e.g. does MIX B light an LED?
  does the wheel in the menu change anything the host could see? Write
  it down per step; the visible state is the ground truth for whether
  a host write/readback maps to the control.

## After the captures (Linux side)

1. Copy the new `.pcap`s into `tools/usbdump/`.
2. **Calibration analysis** (`cap_calib.pcap`):
   - Gain: collect the ordered `0x1A` writes to `wIndex=0x0000`
     (`parse_usb.py --ctrl`), `raw = wVal & 0x1F` (ignore the
     0x20/0x00/0x40 counter). Correlate with the dB sequence the user
     set → build the raw→dB table (note rounding/hysteresis).
   - Fader: `0x12` writes to `0x0034/0x004E` (+ the low-map
     `0x0000-0x001F`/`0x001A-0x0033`) → dB→16-bit table. Mask the
     transaction flag: `address = wIndex & 0x3FFF`.
   - Master: `0x12` to `0x03E0/0x03E1` + `0x1A` to `0x0004/0x0005`.
   - Pan: the `0x12` writes that vary one side of the pair.
   - Update PROTOCOL.md tables + `protocol.rs` (the probe's
     `gain`/`vol`/`master` conversions and `set_crosspoint_balance`).
3. EQ analysis (`cap_eq2.pcap`): diff the 0x0A 64-byte blocks.
4. Rates (`cap_rates.pcap`): extract `0x1B` values per rate.
5. Verify on hardware: set a few gains/faders from the new tables and
   re-measure with `gainsweep_full.c` / `listentest.c`.
6. Update PROTOCOL.md / HANDOFF.md, then sync both copies
   (`rsync -a --exclude target --exclude .git --exclude .claude
   <src>/ <dst>/).

## Capture 14 (2026-08-24): DIM semantics — for the kernel driver panel work

Background (cap_dim.pcap): front-panel DIM is HOST-DRIVEN — the host
sees the DIM press (0x17 readback byte3 flash 0x60 + byte1 bit 0x20
sticky) and writes the DIMMED volume to the selected output's master
(`0x1A 0x0006/7` + `0x12 0x03E2/3` = PH3/4) + a state write
`0x17 0x2400 0x2000` (ON) / `0x17 0x0400 0x2000` (OFF). Nothing
implements it yet (TuxMix panel_tick logs it; the kernel driver has no
panel polling).

1. **Dim amount**: with the Phones master at a few known values (0 dB,
   -20 dB, -40 dB), press DIM and read the written `0x12` value — is it
   a FIXED floor (0x001A ≈ -50 dB) or current−X dB? (The single capture
   showed 0x0089 → 0x001A ≈ -14.4 dB — inconclusive.)
2. **The 0x17 0x2400/0x0400 0x2000 write**: which readback byte/bit it
   sets (byte1 0x20?) and whether it controls the DIM LED.
3. **Per output**: DIM with OUT = Ch1/2, Phones, Opt selected — same
   dim value on all three? (Opt = which register pair?)
4. **2 s hold** (recall): confirm the restore value = the pre-DIM volume.

Save as `tools/usbdump/cap_dim2.pcap`.

## Capture 15 (2026-08-25): loopback — kernel driver vs userspace RE discrepancy — DONE (cap_loopback2.pcap)

Background: the userspace RE (loopback3.c, 2026-08-23) HARDWARE-VERIFIED
that `0x15 0x0001 wIdx=0/1` (loopback on the AN1/2 output pair) makes the
AN1/2 OUTPUT bus appear on the record IN words 0/1 (a 440 Hz tone on PB1
routed to the AN1/2 output showed up on IN ch0/1 at ≈ −24 dBFS).  The
KERNEL driver does the same write (`bf_loopback_put`, out 0 → wIdx 0/1)
but on Linux the observable effect is DIFFERENT:

- loopback OFF + tone playing: **IN ch0/1 = the mic**, and IN ch10/11
  (device words 12/13) carry the OUTPUT MONITOR bus (the playback at
  master volume) — regardless of the loopback registers (a full
  30-channel `0x15 0x0000` clear at stream start changes nothing).
- loopback ON (wIdx 0/1): IN ch0/1 go to **digital zero** (the direct
  record is MUTED) and the AN1/2 output bus does NOT appear on words
  0/1 (nor 4/5, nor 6-13 — probed by remapping the driver channel map).

Open questions only a TotalMix capture can answer:

1. **The OFF sequence**: what EXACTLY does TotalMix write when the
   loopback button is clicked off? (Full 30-channel clear? per-pair?
   with a flag/keepalive after?) — the current kernel driver writes
   only wIdx 0-11.
2. **The record target**: with loopback ON on the AN1/2 output pair,
   WHERE does the AN1/2 output bus land in the record stream (words
   0/1, 4/5, 12/13, or nowhere)?  Play a tone through the AN1/2 output
   in TotalMix with loopback on and capture the IN URBs.
3. **What the loopback button maps to**: in TotalMix, the Loopback
   button is per-output — click it on the AN1/2 output, then on the
   Phones output, and diff the `0x15` wIdx values (which channel
   numbers each output pair maps to).
4. **The monitor words**: confirm words 12/13 = output monitor (they
   carry the playback at master volume on Linux even with loopback
   off) — does TotalMix's record path show the same?  (This also
   means the kernel driver comment "app ch4-11 = words 6-13
   (ADAT/SPDIF)" is WRONG for words 12/13 — likely ADAT/SPDIF is
   words 6-11 only.)

Suggested capture script (slow, ~2 s between clicks):

1. TotalMix up, a 440 Hz tone routed to PB1 → AN1/2 output at 0 dB.
2. Click **Loopback ON** on the AN1/2 output, wait 2 s, click **OFF**,
   wait 2 s, repeat once.
3. Click **Loopback ON** on the Phones output, wait 2 s, click OFF.
4. Leave it OFF, wait 3 s (the baseline with the tone still playing —
   the monitor words).

Save as `tools/usbdump/cap_loopback2.pcap`.

**CAPTURED 2026-08-25 (live music, all four questions answered — see
PROTOCOL.md "Loopback"):**
1. **OFF = the FULL 30-channel clear** (wIdx 0-29 → 0x0000); ON =
   the pair at 0x0001 + the other 28 at 0x0000. TotalMix writes all
   30 on EVERY toggle — the kernel driver (wIdx 0-11 only) must do
   the same.
2. **Record target = IN words 0/1** (the AN1/2 output bus, full
   level; 0 when OFF).
3. **Button → wIdx = 2×out_index**: AN1/2 = 0/1, PH3/4 = 2/3, …
4. **Words 12/13 = 0 in this Windows session** (the Linux "always
   echoed" output-monitor behavior was NOT reproduced — state-
   dependent).

## Capture 16 (2026-08-25): the WINDOWS SYSTEM VOLUME — which crosspoints it moves

Prerequisite for the kernel system-volume model step 1 (KERNEL-DRIVER.md
"System-volume model"): expose the PB playback faders as ALSA volume
controls so the OS volume attenuates the source, TotalMix-style.  But
we do NOT know which crosspoints the Windows volume slider writes —
the PB1 fader is per-submix (PB1→out0 … PB1→out5), and TotalMix may
move it in the current submix view, in ALL submixes, or only the
WDM-default output.  A short capture settles it:

1. TotalMix on the **AN1/2** output view; play the 440 Hz tone on
   PB1.  Move the Windows volume slider for **Speakers** down ~10 dB
   (slowly, 2 s pause at each step), then back up.
2. **Without touching anything else**, switch the TotalMix view to
   **PH3/4**: does the PB1 fader there show the SAME -10 dB (one
   global fader = all submixes written) or 0 dB (only the AN1/2
   submix was written)?
3. Move the slider again on the PH3/4 view — diff the `0x12` writes:
   which crosspoint addresses (`0x0034+0x0034·out+12…23`) appear?
4. Optional: set TotalMix's fader GROUP (the "!" link) on the PB1
   strips and repeat — does the group change the write set?

Also measure the written raw vs the slider % at 2-3 points (the fader
law for the PB crosspoints — feeds the ALSA dB TLV for the new
controls).  Save as `tools/usbdump/cap_sysvol.pcap`.

## Capture 17 (2026-08-25): is the Phones output stereo on Windows TotalMix? — RESOLVED, NO CAPTURE NEEDED

**RESOLVED 2026-08-25 (no capture needed): the mono was a KERNEL DRIVER
BUG, not a device behavior.**  The "cross" crosspoint registers
(L-reg odd / R-reg even of each block) were never zeroed → PB1 R fed
the L side and PB1 L the R side → L+R on both Phones channels =
mono.  Fixed in the driver (`bf_crosspoint_clear_cross`); the
Windows side was always stereo (user-confirmed with a stereo test
video; cap_stereo.pcap shows the toggle is host-side, zero writes).
The capture steps below are kept for reference only.

Linux finding (ear + usbfs, KERNEL-DRIVER.md): the PH3/4 (Phones)
output mixes L+R into both channels — a hard-panned tone arrives
centred on both ears — while AN1/2 is stereo.  The kernel routing is
correct (verified); the device sums L+R on the Phones.  The manual
says the outputs can be mono or stereo (channel mode) and the Phones
is "identical to the XLR line outputs" — so TotalMix may well produce
stereo Phones.  **The user's hint (2026-08-25): every Bus/channel
strip has a STEREO TOGGLE that switches a pair (e.g. AN 1/2) into two
separate mono channels (AN 1 + AN 2).**  That toggle = the channel
mono/stereo mode — if the PH3/4 strip is in "two mono channels" mode
(not the stereo pair), it could be the mono source.  First the
2-minute ear check, then capture the toggle:

1. **Ear check**: play a hard-panned tone (or any stereo music with
   wide panning) in Windows through the Babyface; headphones on the
   Phones jack.  Stereo → the RE session init is missing a write → do
   steps 2-4.  Mono → check step 2 anyway (the toggle state is the
   prime suspect), then document.
2. **The stereo toggle, captured**: start the capture, then on the
   **PH3/4 (Phones) bus strip** click the Stereo button to switch
   between the stereo pair (PH 3/4) and the two mono channels
   (PH 3 + PH 4) — click it a few times, slowly, and diff the USB
   writes per state.  Do the same on an **input** strip (AN 1/2 ↔
   AN 1 + AN 2) and on a **playback** strip (PB1/2 ↔ PB1 + PB2) to
   see the generic mechanism + which register each row uses.  This
   reveals the channel-mode register (if any).
3. If the Phones strip was in the two-mono state on Linux, re-probe
   the device on Windows and capture the session-start writes
   TotalMix makes on its own — the stereo-enabling write may live in
   the session init.
4. Diff against the RE init (`loopback3.c` / `lr_test.c`) — find the
   missing write(s) that make the Phones stereo.

Save as `tools/usbdump/cap_phones_stereo.pcap`.

## Tools available

- `capture.ps1` / `usbdump.exe` / `ParsePcap.exe` — Windows capture +
  parse (USBPcap, linktype 249).
- `parse_usb.py` — Linux parser (vendor ctrl writes + reads + URB
  events).
- `gainsweep_full.c` / `listentest.c` / `vudemo.c` — Linux hardware
  verification.
- `PROTOCOL.md` — source of truth; keep it updated.
