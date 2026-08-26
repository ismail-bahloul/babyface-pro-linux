# Linux validation — Babyface Pro FS proprietary mode (2026-08-22)

Purpose: **validate on real hardware** everything reverse-engineered
from Windows USBPcap captures (this session). The captures prove what
the Windows driver SENDS; this checklist proves TuxMix WORKS. Do these
on CachyOS with the Babyface Pro FS plugged in proprietary mode
(VID `2a39`, PID `3fc0`).

The protocol facts referenced below are in
`tools/usbdump/PROTOCOL.md` (source of truth), `CALIBRATION.md`
and `HANDOFF.md`.

## 0. Get the repo + build

The Windows repo lives on the **SHARED** partition (`D:\TuxMix`, NTFS
volume labelled SHARED). Copy it to the native Linux filesystem
(building on NTFS is very slow), excluding the 26 GB of Windows build
artifacts:

```bash
mkdir -p ~/tuxmix
rsync -a --exclude target --exclude .git --exclude .claude \
  /run/media/$USER/SHARED/TuxMix/ ~/tuxmix/
cd ~/tuxmix

# USB backend + examples
cargo build -p tuxmix-usb --features driver --examples
# TUI (USB backend; ALSA optional)
cargo build --release -p tuxmix-tui
# GUI (if wanted)
cargo build --release -p tuxmix-gui
```

Hardware access needs root or udev rules. Unload `snd-usb-audio` if it
grabs the device.

## 1. TUI / GUI basic validation (the headline test)

```bash
sudo ./target/release/tuxmix-tui
```

`open_real()` falls back ALSA → USB, so with the device in proprietary
mode the USB backend is used. Verify:

- The AN1-4 strips show **live VU meters** when speaking into the mic
  (the USB `meters()` drains `MeterAccum` from the IN stream).
- Faders / 48V / gain / pan / master **reach the hardware** (audible +
  the front-panel P48 LED for 48V).
- **State get (DONE 2026-08-23)**: `open()` reads the 0x17 status
  (byte 0 = preamp state) and syncs the strips — if the front-panel
  P48 LED is on before launching (persisted state), the strip shows
  `48V` immediately. Gains have no readback (start at 0).

## 2. Calibration spot-checks (was TODO)

```bash
sudo target/debug/examples/probe vol 0243    # expect ~ -20 dB
sudo target/debug/examples/probe gain 65     # expect raw 20 (5-bit field)
sudo target/debug/examples/probe master 2000 # expect 0 dB (master curve 0x2000)
```

## 3. Sample-rate change (DECODED + HARDWARE-VERIFIED 2026-08-22 — ratetest.c)

Mid-session rate change = **`SET_INTERFACE(5, alt N)` ONLY** — no
vendor writes, no 0x1B. Alt table (cap_rates2.pcap, clean 9-rate sweep):

| Alt | Packet | Rates |
|---|---|---|
| 1 | 448 B | 32 / 44.1 / 48 / 64 / 88.2 kHz |
| 2 | 640 B | 96 / 128 kHz |
| 3 | 1024 B | 176.4 / 192 kHz |

Test on Linux:

1. Start the stream at 48 kHz (alt 1) — the known-working path.
2. Send `SET_INTERFACE(5, alt 2)` mid-session (reuse the existing
   alt-setting code; there is no vendor rate write).
3. Verify with the **`0x11` readback**: byte 3 should be `0x04`
   (2^alt: alt 1 → 0x02, alt 2 → 0x04, alt 3 → 0x08) — the readback
   carries a poll counter in the low bits, check `byte3 & 0x0F`.
4. **Measure the real stream rate**: the device is the clock master,
   so count the IN bytes over a fixed wall-clock window (2-5 s) and
   divide by the frame size → frames/s. At 96 kHz expect ~2× the 48 kHz
   frame rate. NOTE: the frame size may change at high rates (the
   bandwidth model says 10 ch = 40 B/frame at 96k, 8 ch = 32 B at
   176.4/192k) — determine the actual frame layout empirically from the
   stream before dividing.
5. If audio plays, confirm the pitch is right (play a tone).

Do the same for alt 3 (176.4/192) and back to alt 1 (48). **This
validates the whole rate table.**

## 4. Clock source (DECODED + HARDWARE-VERIFIED 2026-08-22 — clktest.c)

The keepalive `0x10 wVal wIdx=0x05CF` doubles as the host settings
word (bit 2 = clock source Optical):

| wVal | Meaning | 0x17 byte 2 |
|---|---|---|
| 0x0001 | clock Internal, locked | 0x40 |
| 0x0004 | clock source = Optical, NO lock | 0x80 |

Test: send `0x10 0x0004 0x05CF` (instead of the 0x0001 keepalive) and
read `0x17` → expect byte 2 = 0x80 (no lock, since nothing is on the
optical input). Then back to `0x0001` → 0x40. (0x80 is the clock
no-lock state, NOT "streaming active" — the old notes had it wrong.)

## 5. Pitch / varispeed (DECODED + HARDWARE-VERIFIED 2026-08-23 — pitchest.c / tonepitch.c)

Pitch writes a **0x1B quad** (4 banks, wIdx low byte = bank 0-3). The
quads shift the **device clock**: the IN-stream byte rate moves with
the pitch (rate × bank0 = const), and a steady 440 Hz host tone
(PB1 → AN1/2) detunes with it — the audio pitch follows the clock.
The quad MUST be followed by the clock keepalive `0x10 0x0001
0x05CF` (cap_fus2.pcap sends it after every quad).

**FULL DECODE (cap_pitch.pcap, 454-quad sweep, 2026-08-23) — the quad
is 16.8 FIXED POINT**: bank0 `wVal` = DDS integer, `wIdx` high byte =
8-bit fraction (NOT an address!); DDS_24 = round(50000×256/(1+p/100))
at 0% = 0xC35000. bank1 = round(DDS16 × 0.72562), bank2 = round(DDS16
× 2/3) — exact on all 454 quads. bank3 = QUANTIZED (needs a lookup;
the "0x7CFF 0xF803 constant terminator" is only the 0% value).

The old labels for the two 4% quads were **sign-flipped**: 0xBBCC is
+4.00% (tone UP), 0xCB72 is −4.05% (tone DOWN). **Pitch shifts the
sample rate dynamically** (DDS drives the device clock: +5% = 50400
Hz at nominal 48k, NO SET_INTERFACE — the effective rate must be
recomputed).

**Linux test still open**: the fraction bytes (wIdx high) of banks
1-3 are not derived. Send the exact 16-bit parts with frac = 0 and
verify the pitch still applies (tone detunes correctly); if not, the
fractions matter and need a fit. Then implement `set_pitch(p)` in
tuxmix-core: DDS_24 = round(50000×256/(1+p/100)), bank1 = round(DDS16
× 0.72562), bank2 = round(DDS16 × 2/3), bank3 = lookup table from the
sweep (or the 16-bit wVal + frac=0 if the test passes).

**RESOLVED (2026-08-23, pitchformula.c + pitchsweep.c)**: the
fractions do NOT matter — a DERIVED quad (all fracs 0) produced the
identical IN rate (2795520 B/s) as the captured verbatim +4% quad,
and bank3 = 0x7CFF frac 0 works (the 0% value; no lookup needed).
`set_pitch(p)` is IMPLEMENTED in `tuxmix-usb/src/protocol.rs`
(DDS_24 = round(50000×256/(1+p/100)) split 16.8, bank1 =
round(DDS16×0.72562), bank2 = round(DDS16×2/3), bank3 = 0x7CFF frac
0, + keepalive 0x10 0x0001 0x05CF), wired through
`tuxmix-core::RmeDevice::set_pitch` (USB backend; ALSA backend errors
— no varispeed in class-compliant mode; the field is
`#[serde(skip_serializing)]` so the auto-scene keeps 0). TUI: `y`/`h`
= ±0.1%, `Y`/`H` = ±1% (shown in the Overview line).

**pitchsweep.c sweep (2026-08-23, live card)**: rates move with the
pitch — +4% → +4.09%, -2% → -1.91%, +5% → +5.16%, -5% → -4.98%
over baseline; back to 0% → nominal restored. The Rust f32 formula
matches the hardware across the full ±5% range.

## 6. CUE on hardware outputs (DECODED + HARDWARE-VERIFIED 2026-08-23 — cuetest.c)

CUE on an output strip toggles a **GLOBAL** crosspoint map
(0x0040-0x0065 = PB1-6 L/R into AN1/2; the CUE bus = the AN1/2 monitor
path): `0x0000` = CUE ON (playbacks muted), `0x2000` = CUE OFF. Each
output has a dedicated source PB kept at 0x2000: **source PB =
out_idx+1** (verified: AN1/2→PB1, PH3/4→PB2, AS1/2→PB3, ADAT3/4→PB4,
ADAT7/8→PB6; ADAT5/6→PB5 predicted — no capture yet). There is NO CUE
on hardware inputs.

Verified with `tools/usbdump/cuetest.c` (440 Hz tone on PB1 → AN1/2):
CUE ON the AN1/2 output (source PB1 kept) → tone keeps playing; CUE ON
PH3/4 (PB1 muted) → tone stops; restore (0x2000) → tone back.

Addresses: PB1-6 L = 0x0040/0x42/0x44/0x46/0x48/0x4A, R = 0x005B/0x5D/
0x5F/0x61/0x63/0x65 — the AN1/2 output's playback crosspoints
(L = 0x34+idx, R = 0x4E+idx, idx 12-23). **The CUE writes carry NO
0xC000 transaction flag** (bare wIdx — unlike fader writes).

Test: write the map with 0x0000 (except the source) and confirm the
routing/mute change; restore with 0x2000.

## 7. Keepalive flags (EQ for Record / Optical Out) — optional

- `0x10 0x0041 0x05CF` = EQ for Record ON (bit 6), `0x0001` = off.
- `0x10 0x0401 0x05CF` = Optical Out SPDIF (bit 10), `0x0001` = ADAT.
These are the same keepalive register as step 4 — set all bits at once
(e.g. clock Optical + EQ-record = `0x0045`).

## 8. Session persistence — does the CARD store the gains? (2026-08-23)

**ANSWER (2026-08-23, manual + UI): NO DSP memory.** The Babyface Pro
FS has no device-setup slots (TotalMix "Store current State into
Device → Setup 1-6" is GRAYED OUT — that's a UFX-series feature) and
the manual states the DSP functions have no memory
("neither controls nor memory for the DSP functions"). Only the
analog preamp state (48V/PAD) persists in the card's EEPROM (verified
earlier). The mixer/gains are host-side — TuxMix MUST re-apply its
auto.json scene after boot.

The empirical power-cycle test below is OPTIONAL confirmation (the
answer is expected: gains are lost):

```bash
# 1. start the stream + set a known gain (gaintest.c pattern):
#    gain raw 17 (≈ 55 dB), 48V on AN1
# 2. measure the AN1 IN peak with a steady source (or mic + voice)
# 3. POWER-CYCLE the card: unplug the USB cable ~10 s, replug
#    (this resets the byte-2 counter — the EEPROM state should survive)
# 4. start the stream AGAIN but DO NOT write any 0x1A gain
# 5. measure the AN1 IN peak again
```

- Same peak → the card stored the gain (unlikely per the above).
- Lower/different peak → the card does NOT store gains — TuxMix must
  re-apply its auto.json gains after boot (expected).

Adapt `tools/usbdump/gaintest.c` (it already measures the AN1 peak per
gain — split it into two runs around the unplug).

## 9. NEW controls decoded on Windows (2026-08-23) — implement in TuxMix

These were all captured on Windows (see PROTOCOL.md sections) and can
now be implemented + hardware-validated on Linux:

- **Phase Ø** = negate the crosspoint coefficient (Q15): write
  `0x12 -(val)` on BOTH maps (low 0x0000+idx + standard 0x0034+0x0034·out+idx).
- **Trim (T)** = a second gain stage through the crosspoints
  (range -∞..+6 dB, master curve) — same registers as the fader.
- **MS proc** = AN2 crosspoints (low 0x0001 + standard 0x0035) → 0x0000.
- **AN 1>2** = `0x17` wIdx=0x1000, bit 0x1000 (0x0400 ↔ 0x1400).
- **Ref level (Instr 3/4)** = `0x17` wIdx=0x003F + `0x21` commit
  (values 0x0000 ↔ 0x000C observed; exact 3-state bit map TBD).
- **Loopback = NEW bReq 0x15**: wIdx = channel 0-29, wVal 0x0001 ON /
  0x0000 OFF (AN1/2 = ch 0/1).
- **Stereo split** = rewrite the playback crosspoints: 0x1000 (-6 dB)
  stereo pair ↔ 0x2000/0x0000 (split mono).
- **FX send** = `0x12` wIdx=0x0138/0x0153 (L/R), ramp to 0x1000 max.
- **Width** = 4 balance-pair registers (0x00AE/0x00AF, 0x00C8/0x00C9,
  0x0046/0x0047, 0x0060/0x0061), L+R = 0x2000, bipolar -1..+1.
- **DIM (front panel)** = byte1 bit 0x20 in the 0x17 readback;
  DIM-2s restores the session volume (fader pair write).
- **Panel state in the 0x17 readback** (for a panel emulator): byte1 =
  OUT position (0x04/0x05/0x06) + DIM bit 0x20, byte2 = IN pair
  (0x4A/0x5A/0x6A) or wheel counter, byte3 = button flash
  (0x41=IN, 0x48=OUT, 0x42=SET, 0x44=MIX, 0x50=SELECT, 0x60=DIM).

**IMPLEMENTED 2026-08-23** (except Trim + the front-panel emulator):
all of the above except Trim (a second fader stage — deferred) and the
DIM/panel emulator (a GUI feature) are wired end-to-end:

- `tuxmix-usb/src/protocol.rs`: `set_loopback` (0x15), `set_an12`
  (0x17 0x1000), `set_ms_proc` (AN2 xpoints), `set_phase` (bitwise-NOT
  of the current fader value on both maps — 0x0EA0 → 0xF15F ✓),
  `set_fx_send` (0x12 0x0138/0x0153), `set_stereo_split` (0x2000/0x0000
  vs 0x1000/0x1000, both maps), `set_width` (4 balance pairs, L =
  0x2000·(1+w)/2 — the +0.75 example reproduces 0x1C00/0x0400 exactly),
  `set_ref_level` (0x17 0x003F + 0x21; the ref code is hypothesized in
  bits 2-3 of the preamp register, since 0x0C = also `PREAMP_BASE`).
- `tuxmix-core::RmeDevice`: the 8 methods (default impl = error),
  overridden on the USB backend; mock mirrors the state. New model
  fields: `InputChannel::phase/ref_level`, `PlaybackChannel::split`,
  `OutputChannel::loopback`, `DeviceSettings::ms_proc/an12/
  fx_send_db/width` — all `#[serde(default)]`, applied by
  `apply_scene` (loopback unconditionally; the others only when set,
  so fader values are never clobbered).
- Unit tests: 8 new protocol tests (20 total, all green).
- `tools/usbdump/ctrl9.c` smoke-tested live on the card: all 8 writes
  accepted, everything restored.

**Open items for the next Windows capture**: the ref-level 3-state bit
map (which bits encode +4dBu/-10dBV/Boost), the width register→strip
mapping, the FX-send dB curve (0x000C→0x1000 ramps), the
stereo-split "alternating" bus pattern (the implementation writes the
literal captured 0x2000/0x0000 into out0), and the loopback OFF
sequence (0x0000 on ch 0/1 alone sometimes leaves it engaged).

**Hardware results 2026-08-23 (ctrl9sound2.c / loopbacktest.c)**:
- Phase ✅ (stereo image flips when the L crosspoint is negated).
- Stereo split ✅ (splittest.c, 3 A/B cycles): the 660 Hz (R) vanished
  and the 440 Hz jumped to the left ear at ~+6 dB — the
  0x2000/0x0000 split-mono pattern is correct.
- Width: encoding confirmed (0.75 → 0x1C00/0x0400) but the 4
  registers do NOT affect PB1 — a stereo 440/660 tone was unchanged.
- Loopback: direction CONFIRMED output→input (recording path) — the
  440 Hz tone appeared on IN ch0/1 at -24 dBFS (source -18 dBFS).
- AN 1>2: CONFIRMED (an12probe.c) — IN ch1 (AN2) jumps from -103 to
  the AN1 mic level when ON. The write needs the 0x21 COMMIT (OFF
  without it left the route half-live at -41 dBFS) — protocol fixed.
- MS proc: NOT confirmed by ear — the AN2 physical input delivered no
  signal during the tests (the mic works on AN1; the AN2 XLR/connection
  is suspect). The loopback and the AN 1>2 route inject POST-crosspoint
  (the loopback tone ignores both the preamp gain and the crosspoint
  mute), so they can't be used to observe the crosspoint mute. The
  implementation MATCHES the cap_ctrl2.pcap sequence exactly (engage
  `0x12 0x0000 0x0001/0x0035`, restore flagged). To validate: a working
  signal on the PHYSICAL AN2 input, then MS proc ON should cut it.
  (Also: the AN2 analog input is worth a physical check — XLR contact,
  jack — since 48V/gain read correctly but no signal arrives.)
- FX send: no audible effect (reverb is host-side, as documented).
- Mic path (48V + gain 35 + AN1 → AN1/2) confirmed working.
- Ref level: not yet validated (needs an instrument on Instr 3/4).

## 10. EQ — FULLY DECODED (2026-08-24): biquad formula + low-cut formula

EQ coefficients are uploaded as 64-byte bulk ep 0x0A blocks (see
PROTOCOL.md "EQ coefficients" sections + `tools/usbdump/eq_biquad.md`;
full calibration data in cap_eq3-9.pcap). Structure:

- 3 band slots × 5 × 32-bit biquad coeffs (slot offsets 0x04/0x14/
  0x24 + one more at 0x34), header 00 00 00 80 (L) / 01 00 01 80 (R),
  constant 00 00 00 04 @ 0x38. Coeffs ramp (interpolation) then settle.
- Ranges (user-confirmed): gain -20..+20 dB, freq 20 Hz..20 kHz, Q
  0.7..5, bell ↔ shelf.

**BAND BIQUAD — FORMULA DECODED 2026-08-24 (eq_biquad.md).** The 5
stored words are the RBJ cookbook biquad in a normalized-numerator /
normalized-denominator split:

```
H(z) = c4·(1 + c2·z⁻¹ + c3·z⁻²) / (1 + c0·z⁻¹ + c1·z⁻²)
   c0 = a1′   c1 = a2′   c2 = b1′/b0′   c3 = b2′/b0′   c4 = b0′
```

′ = normalized by a0; stored word = value ×2²⁷ (signed i32). BELL =
exact RBJ peaking (A = 10^(dB/40), α = sin(w0)/(2Q), fs 48 k); 0 dB =
identity [0,0,0,0, 0x08000000]; gain sign flip mirrors c0↔c2, c1↔c3.
**Verified to ~1 LSB**: cap_eq8c gain sweep peaks/notches at EXACTLY
200 Hz with EXACTLY the labeled gain (±0.000 dB), DC/Nyquist = 0 dB;
cap_eq8b Q sweep +6.000 dB @ 200 Hz for every Q. cap_eq8a freq sweep
matches ~1e-7 at low freq, drifting to ~1e-2 at 10 kHz (mild warping
detail still open — see eq_biquad.md).

**LOW CUT — FORMULA DECODED 2026-08-24.** `0x38 = round(K·f/(1+c·f))`
with K = 11508, c = 1/11656 (all 9 cap_eq9 labeled points to ≤0.003%).
Slope compensation (pole freq scaled to keep composite −3 dB constant,
MEASURED factors — the theoretical 1.554/1/0.792/0.676 is 1.9-3.3%
off): 6 dB/oct ×1.5267, 12 ×1.0000, 18 ×0.8061, 24 ×0.6977. Header
byte1 = 2^n−1 (0x01/0x03/0x07/0x0F), OFF = byte1 0x00 + 0x38
0x04000000. cap_eq9 group 6 (NOT group 5) = 100 Hz (leftover group
5 = 0x00113CCC; corrected mapping confirmed by cap_eq6).

**Implemented in tuxmix-usb**: `protocol::eq_band_storage` (RBJ
bell/shelf → 5 stored words), `protocol::set_eq_band` + `set_low_cut`
(the L+R block pairs), `protocol::eq_block` (64-byte block builder),
and `BabyfaceUsb::write_eq_block` / `set_eq_band` / `set_low_cut`
(bulk OUT ep 0x0A, interface 1). Unit tests: cap_eq8c gain sweep
round-trip + response peak check (28 tests total).

**To hardware-validate on Linux**: `cargo run -p tuxmix-usb --features
driver --example eqtest` — streams, routes AN1 (48V + 35 dB) to the
phones, and walks 6 ear-check steps: baseline, +6/−6 dB Low bell @
200 Hz, +10 dB @ 4 kHz, low cut 100 Hz @ 12 dB/oct, low cut off. The
band states are the EXACT captured cap_eq8c values, so a correct
upload must reproduce the audibly verified TotalMix sound. Also: the
2-bands-simultaneous c4@0x34 sharing (cap_eq8e — the c4 follows the
last-written band; exact DSP behavior open) and the high-freq warping.

**✅ VALIDATED on hardware 2026-08-24** (user, by ear, mic on AN1 +
Fethead, masters at -30 dB): every step reproduced the expected sound
(+6 bell = low louder, −6 = notch, +10 @ 4 kHz = nasal, low cut =
boom gone, off = back). Note: eqtest now routes AN1 to PH3/4 too
(was AN1/2 only) and uses moderate levels (gain raw 11 ≈ 35 dB,
crosspoints 0 dB, masters −30 dB) — the original vudemo recipe
(55 dB gain, +6 dB faders) was too hot on open headphones.

## 11. AN2 mic / stereo-link — RESOLVED (2026-08-23)

The AN2 XLR works fine (confirmed on Windows: mic audible with 48V+
gain in split mode). The Linux silence was the STEREO-LINK handling:
- The input-strip stereo button = `0x17` wIdx=0x1000 (0x0000 split /
  0x0400 linked / 0x1400 linked+AN1>2). In STEREO mode TotalMix
  writes 48V for BOTH mics (0x000F ↔ 0x000C) and both gains
  (0x1A 0x0000 + 0x0001, independent values) — a one-mic-only write
  leaves AN2 silent.
- TuxMix already composes both mics' bits (`preamp_bits()` → 0x0F)
  and writes per-mic gains. On Linux: set AN1+AN2 phantom + gains +
  both crosspoints, then verify AN2 is audible (gaintest on AN2).
- Full protocol details: PROTOCOL.md "Stereo link / split on the
  AN1/2 INPUT strip".

## 12. LINUX TODO (2026-08-23, after the Windows captures)

0. **Front-panel emulator VALIDATED on hardware (2026-08-24) — the
   host is fully in the loop**: the `0x17` readback is polled by
   `panel_tick` and the events drive the mixer exactly like TotalMix.
   Learned/verified live (probe `tuxmix-usb/examples/panelprobe.rs` +
   user feedback):
   - **MIX is a TOGGLE, host-latched** (manual §5.3 "Monitoring –
     MIX"). The RAW press readback is `0D 0D 41 44` — byte3 flash
     `0x44`, NO engaged bit, byte2 still in the current mode. The
     driver acks the flash with `0x17 0x8480 0x8C80` → the device
     latches fader mode (byte0/1 gain 0x80, byte2 = `0x00+n` counter)
     and STAYS there after the physical release; the SECOND `0x44`
     flash exits it (`0x17 0x0400 0x8000` + `0x8080`). Pressing IN
     (or OUT/SET) during MIX makes the device leave fader mode by
     itself → the driver sends the exit writes (user: IN must return
     to gain control). The flashing red top LED on the input VU during
     MIX is the card's own indicator (manual: "the input level LEDs
     start to flash") — normal, not host-controllable.
   - **KERNEL DRIVER 0d (2026-08-26, `panel.c`)** — the same MIX
     state machine is now IN the kernel (previously it only MIRRORED
     the panel, which is why "MIX B did nothing"): host-ack on the
     0x44 flash, exit writes on the 2nd flash / device-driven exit,
     host-side SELECT L/R/both/none tracking, and the fader-mode
     wheel on the calibrated curve (standard map only, no low-map
     mirror, ±0.5 dB/click, seeded from the reference crosspoint on
     engage).  Wheel deltas are gated on the byte2 mode class so a
     mode switch is never read as a wheel jump.  `fader_selftest.c`
     checks the curve (round-trip + calibrated anchors); regress
     40/40.  **VALIDATED LIVE 2026-08-26 (mix_test.py)**: MIX → wheel
     moves AN1 (SELECT Left) / AN2 (Right) into the OUT-selected
     output at exact curve values; MIX again → gain; IN during MIX →
     device-driven exit.  Live bug fixed: the OUT mapping wrote into
     ADAT7/8 instead of Phones (enum 2→5 vs 2→1) — the wheel had been
     working all along, in the wrong block.**
   - **Wheel by mode**: MIX → the SELECT-chosen channel(s) of the
     selected input → selected output monitoring crosspoint (±0.5 dB,
     standard map only, no low-map mirror), OUT → the selected output
     master, IN → the SELECT-chosen preamp gain (±1 dB). The wheel
     now mirrors every change into the local model so the UI fader
     follows (was: raw USB write only, GUI frozen).
   - **SELECT = L/R/both/none — the PRESS is detectable (cap_select2,
     2026-08-24)**: the ep 0x82 status stream shows a byte3 flash
     0x50 on every SELECT press (the old "SELECT not in the readback"
     note only looked at the 0x17 control reads, whose DATA USBPcap
     doesn't capture). After IN, byte2 walks a 16-step counter
     0x44..0x4F,0x40..0x43 — the SELECT state is likely byte2
     low-nibble mod 4 (4 states per cycle); the counter also advances
     on the wheel. Linux check: press SELECT, watch the readback for
     the 0x50 flash + counter, and pin the state↔value mapping. The
     host-side counter can drift from the card's LED display
     (device-local, survives GUI restarts) until the mapping is
     verified.
   - The GUI now follows the panel's OUT selection (TotalMix:
     "highlight the currently selected submix") via
     `DeviceHandle::panel_selection()`.
   - **Panel gain family = 0x1A 0x000A+mic, VALIDATED 2026-08-24**: the
     wheel in gain mode writes the "ADC gain" registers (cap_select.
     pcap), NOT the GUI's 0x0000+mic — user confirmed the audible gain
     still follows. The two families drive the same preamp; the exact
     relation stays a curiosity, not a blocker.

   TODO capture (Windows): SELECT cycle in gain mode — press IN, then
   SELECT 4× slowly (no wheel), watch the 0x17 readback for a SELECT
   encoding; then SELECT + wheel after each press — the `0x1A` gain
   writes reveal which channel TotalMix targets per SELECT state
   (AN1 = 0x0000, AN2 = 0x0001, both = both regs). Same for MIX +
   SELECT + wheel (the 0x12 crosspoint targets).

   **FIXED 2026-08-24 (user reported the panel "getting lost between
   gain and volume")**:
   - `panel_wheel` had a duplicated empty `else if ps.out_mode()`
     (merge artifact) — the wheel was a NO-OP in OUT mode.
   - `out_mode()` accepted only byte2 0x8x, but the OUT wheel counter
     is a full byte carrying 0x8F → 0x90 (cap_set2: 0x90-0x9F): at
     0x9x the wheel fell through to the GAIN branch and moved the mic
     gain. `mode_class()` now treats 0x8x/0x9x as OUT.
   - The wheel-delta gate dropped the click at the 0x8F→0x90 carry.
   - `set_an12` re-composed the 0x0400 link base, re-linking a split
     pair — now `protocol::set_input_link(linked, an12)` composes
     with the tracked `BabyfaceProUsb::input_link` state, and
     `RmeDevice::set_input_link(linked)` exposes the flag for the
     future GUI stereo button.

1. **Sample rate switch IMPLEMENTED + VALIDATED on hardware
   2026-08-24** — `BabyfaceUsb::set_sample_rate(rate)` = SET_INTERFACE(5,
   alt) + stream restart at the rate's frame layout (56/40/32 B × 256
   frames = 14336/10240/8192 B URBs; the VU-meter accumulator is
   frame-aware now). User cycled 48→96→192→44.1→48 kHz from the GUI:
   no crash, the rate display follows, the VU meters keep running at
   every step. (The exact stream rate via bytes/s still measurable with
   ratetest.c if ever needed.)
2. **Clock source IMPLEMENTED (2026-08-24) — VALIDATE**: keepalive
   0x05CF word bit 2 — `set_clock_source("Optical In")` must flip the
   0x17 readback byte 2 0x40 (Internal) → 0x80 (no-lock), and back.
3. ~~**EQ formula fit — PRIORITY**~~ — **DONE (2026-08-24)**: the
   band biquad (RBJ, normalized-num/den split, verified to ~1 LSB)
   and the low cut (K=11508 c=1/11656 + slope factors) are FULLY
   DECODED and implemented (`protocol::set_eq_band` +
   `set_low_cut`, `BabyfaceUsb::write_eq_block` — bulk ep 0x0A).
   **What remains = hardware validation**: run
   `cargo run -p tuxmix-usb --features driver --example eqtest` —
   streams, routes AN1 (48V + 35 dB) to the phones, walks the
   ear-check steps (baseline, ±6 dB Low bell @ 200 Hz, +10 dB @ 4
   kHz, low cut 100 Hz @ 12 dB/oct, off). See §10 for the formula
   and the open refinements (2-band c4@0x34 sharing, high-freq
   warping, shelf exact variant).
2. **Validate the set_loopback fix**: OFF now clears all 30 channels
   (TotalMix behavior, cap_loopback_off.pcap — the capture CONFIRMS:
   TotalMix always writes the FULL 30-channel map on every toggle).
   Retest loopback3.c: OFF must always disengage.
2b. **set_fx_send curve — TOP CONFIRMED (cap_fxsend.pcap, 2026-08-26)**:
   the send pot max = 0x1000 (−3.02 dB fader curve), full range
   0x0003 (−62 dB) → 0x1000; `set_fx_send`'s `.min(0x1000)` clamp is
   right, no change needed.
2c. **set_width — FULL STRIP MAP RESOLVED (2026-08-26, cap_width3-7)**:
   the width writes mirror balance pairs (L = 0x1000·(1+w), R =
   0x1000·(1−w), L+R = 0x2000) to a FIXED target per strip: HW inputs
   + PB1 → out 0 (+ low map); PB2-6 → out n−2 (PB4 → out 2 = the old
   0x00AE mystery).  `set_width` currently writes only the low-map
   pairs — extend to the standard-map targets.  Width on a 0-routed
   strip writes nothing.
2d. **Ref level — FULLY DECODED (cap_reflevel3.pcap, 2026-08-26)**: the
   3 states (labeled): +4dBu = `0x17 0x000F`/`0x21 0x0000`, −10dBV =
   `0x17 0x0003`/`0x21 0x0000`, Boost = `0x17 0x0003`/`0x21 0x0003`;
   the 0x0C bits = the +4dBu ref level (NOT a constant base, NOT a
   PAD) — the preamp byte = 48V (bits 0/1) + ref (bits 2/3) + PAD
   (bits 4/5).  Implement: `set_ref_level` recomposes the 0x17 0x003F
   state (48V + ref + PAD) and sets the 0x21 wVal 0x0003 for Boost.
2e. **Mute + SOLO — implemented 2026-08-24, VALIDATED on hardware**:
   mute zeroes the strip's crosspoints (low map for inputs + all-output
   standard pairs) and restores them on unmute (cap_mute2.pcap). Solo
   = mute-the-others (cap_solo2.pcap): the soloed strip's low map goes
   0x2000 (playback) / 0x1000 (linked input), every OTHER strip's
   crosspoints go 0x0000, un-solo restores from the model; exclusive
   (the TotalMix default). Old "global solo registers"
   (0x0004/0x001F/0x000C/0x0027) removed — they were AS1/2's + PB1's
   LOW MAPS, not solo flags. User-verified 2026-08-24: mute AN1 cuts
   the mic, mute the PH3/4 master cuts everything, solo AN2 mutes
   AN1, solo AN1 keeps it audible, all restore cleanly (faders back to
   their positions).
3. **AN2 mic on Linux**: set BOTH preamps (48V 0x0F + gains 0x1A
   0x0000/0x0001) + both crosspoints; the mic works (confirmed on
   Windows). gaintest on AN2 should show the level now.
4. **Buffer size (host-side, design note)**: the Windows "Buffer Size"
   knob is the ASIO/WDM host buffer, not a device register. TuxMix's
   equivalent is our URB cadence (currently 8 URBs × 256 frames ≈
   42 ms at 48 kHz — very conservative). For the CC-beating latency
   goal, drop to fewer/smaller URBs (e.g. 32-64 frames) once real
   audio I/O + an ALSA/PipeWire bridge exist; measure round-trip
   against snd-usb-audio in CC mode.
- **Reverb/echo = HOST-side CONFIRMED** (no bulk OUT on ANY endpoint
   during FX changes — cap_fx/cap_fx2 re-verified on all of
   0x0A/0x07/0x09). Implement reverb/echo in software (or skip); the
   dry path stays in the device (zero latency), wet path = host
   compute + playback-return channels.
 - **Front-panel emulator (NEW 2026-08-23)**: MIX B now works —
   `poll_events` polls the 0x17 readback and turns wheel events into
   mixer writes (MIX monitoring crosspoint, OUT master, IN gain). See
   §12 item 0 for the validation steps.
5. If the user did more captures on Windows (cap_eq8c-f gain/shelf/
   Mid/High/two-band, cap_eq9 low cut freq, cap_reflevel, cap_fx3,
   cap_width2, cap_split2, cap_trim2): extract + document per
   WINDOWS-CAPTURE-PLAN.md.

## LONG-TERM: upstream to the Linux kernel

Once the driver is mature, submit it to the Linux kernel (like the
FireWire `snd-fireface` module) so the Babyface Pro FS is plug-and-play
in proprietary mode on any distro. Requirements: in-kernel control +
stream driver, ALSA PCM/mixer integration, code style + licensing
review, mainline review cycles. `tools/usbdump/PROTOCOL.md` is the
submission reference. Perf target: beat CC mode (snd-usb-audio) on
round-trip latency, and ideally Windows (the proprietary Windows driver
pads conservatively; a lean in-kernel driver can go lower).

## Tools available

- `probe` (48v/gain/master/vol/status), `vu` (terminal VU bars),
  `hold48v_iso` (stream + 48V hold) — `tuxmix-usb/examples/`.
- C tools in `tools/usbdump/`: `listentest.c` (routing recipe),
  `gainsweep_full.c`, `vudemo.c` — reference implementations.
- The streaming recipe: init burst → trigger
  (`0x10 0x0000 0x8000` + `0x1D`) → 9×OUT/9×IN 14336-B URBs (paired!)
  → `0x14 0xC000` arm. Never send `0x13 0xC000` while wanting audio.

## Report back

For each step: command, expected vs actual, and whether the sound/LED
actually changed. Update PROTOCOL.md / CALIBRATION.md / HANDOFF.md with
any corrections, then sync back to the SHARED partition.
