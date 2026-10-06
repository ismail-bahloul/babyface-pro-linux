# Kernel driver strategy — TuxMix → sound/linux (long term)

**Goal (user-stated, 2026-08-24):** turn the reverse-engineered
Babyface Pro FS proprietary-mode knowledge into a *kernel-grade* Linux
sound driver — meeting ALSA/kernel standards — to propose upstream
(sound/usb, linux-sound / linux-usb lists). The user-space TuxMix stack
stays as the reference implementation + validation harness (and the
mixer GUI).

## Architecture decision (FINAL — 2026-08-24, hardware-validated)

**Standalone driver** (`sound/usb/snd-usb-babyface-pro.c`, modeled on
`snd-usb-caiaq`), NOT an snd-usb-audio extension. Reasons, verified on
hardware:

1. The proprietary-mode PCM runs on **interrupt endpoints** (interface
   5, ep 0x01/0x82, bmAttributes 0x03) — `snd-usb-audio`'s PCM engine is
   100% isochronous and has no interrupt path; ISO URBs on those
   endpoints fail with EINVAL. Adding interrupt-PCM to snd-usb-audio
   would re-architect its core for one device (unacceptable upstream).
2. The only in-tree precedent for interrupt audio streaming is
   `snd-usb-caiaq` (and `snd-usb-line6`) — the review model.
3. The class-compliant mode (other PID) keeps working via snd-usb-audio
   untouched; our driver only owns interface 5 (probe returns -ENODEV
   for the others; the standard MIDI interface 2 stays with
   snd-usb-audio).

## Current stack (user-space reference, hardware-validated)

```
TuxMix GUI/TUI (mixer control, RmeDevice trait)
        │
tuxmix-core (the TotalMix-class mixer model)
        │
tuxmix-usb (proprietary USB: vendor requests + interrupt streams)
        │
tuxmix-sys (C ABI, cdylib)
        │
libasound_module_pcm_tuxmix.so  ← PipeWire via spa-alsa (sink/source)
```

## Kernel driver — status (2026-08-24, HARDWARE-VALIDATED)

`tools/kernel/snd-usb-babyface-pro.c` — first working skeleton:

| Feature | Status |
|---|---|
| Probe / card registration (card "Babyface Pro FS") | ✅ |
| Cold init sequence (cap_coldplug) + session arm at probe | ✅ |
| Interrupt stream (ep 0x01/0x82, 256 frames/URB, 8 URBs/direction) | ✅ |
| PCM playback + capture (S32_LE, 24 msbits, 9 rates 32-192 kHz) | ✅ |
| Rate switch = SET_INTERFACE(5, alt) (3 bandwidth classes) | ✅ 32/44.1/48/64/88.2 = alt1, 96/128 = alt2, 176.4/192 = alt3 — measured exact (48/96/192 kHz frame rates) |
| Controls: 6 output masters (0-0x4000 raw, 0 dB = 0x2000) + mutes | ✅ per-output names ("AN1/2" / "PH3/4" / … Playback Volume+Switch) — the 8-bit register is the REAL volume (0.5 dB/step, 0xF3 = 0 dB) |
| System volume = PipeWire SOFTWARE volume | ✅ DONE 2026-08-26 (cap_sysvol2.pcap: the Windows volume is a host-side stream gain, zero USB writes): the masters are named per output so SPA finds no "Master" element → the sink falls back to software volume; `wpctl set-volume` no longer moves any hardware register (verified). See “System-volume model — CORRECTED AGAIN” |
| Controls: 4 preamp gains — TWO laws | ✅ AN1/2 mic: 0-65 dB in 1 dB steps, packed `(fine << 5) | coarse` (corrected 2026-09-13, was wrongly read as 3.25 dB/step + a transaction counter — see the gain-encoding entry below); AN3/4 instrument: 0-9 dB control, raw = dB×2 = 0-18 (0.5 dB/step, cap_gain34 — `bf_gain_max_db`) |
| Controls: 2× phantom + 2× PAD (0x17 0x003F + 0x21 commit) | ✅ (LEDs + relay clicks) |
| Controls: 84 crosspoints (6 out × 14 src) — output order corrected | ✅ (Phones = block 0); AN1/2's low-map requirement found + fixed 2026-09-14, see below |
| Controls: pitch, loopback ×6, AN1>2, link, width, FX send, MS | ✅ |
| Loopback record staging | ✅ NO STAGING NEEDED (2026-08-26): the record is 1:1 with the master on Windows (aligned captures) AND Linux (live, S16 tone: −20 dBFS → −20.0).  The “fixed 2^-5 tap”/×32 were artifacts of mktone.py's broken 4-byte tone (chain ran −48 dB low → record quantizer → coarse square).  ×32 REVERTED; mktone.py → S16 |
| Front-panel readback (0x17 poll at 50 Hz → read-only button/wheel/IN/OUT/MIX/DIM ALSA controls, babyfacepro-ctl.c) | ✅ hardware-validated 2026-08-26 (all six flash codes + wheel; consume-on-get bug fixed — controls hold the last state) |
| Multi-channel PCM (2-12 ch, full 14-word frame) | ✅ (12-ch capture + playback verified; marker words skipped) |
| Default mixer state at probe (playback → all outputs at unity; hardware inputs not routed; analog masters -20 dB, digital 0 dB) | ✅ (inputs-off default 2026-09-28, see "Power-on defaults") |
| DSP EQ (eq.c): 4 strips × 3-band bell/shelf + low cut, 64-byte bulk coeff blocks on ep 0x0A | ✅ HARDWARE-VALIDATED 2026-08-27 on the mic (bell ±6 dB @ 200 Hz, +6 dB @ 3 kHz, low cut 100/300 Hz on/off; `eq_selftest` ~1 LSB vs the captures). Fixed-point Q27 (CORDIC + exp2, no FPU). NOTE: the loopback taps the record bus POST-EQ, so the input EQ is not measurable on the loopback chain (ear-validated instead) |
| Preamp state sync from 0x17 readback at probe | ✅ |
| Mixer-state persistence across interface re-probes (usbfs claim → detach → re-probe restores 48V/gains/crosspoints/pitch/flags) | Removed 2026-10: udev's `alsactl restore` overwrote it on every card add (see "Re-probe resilience"); a reset-resume no longer re-probes |
| PM: suspend/resume with full cached-state restore (cold init + mixer re-apply) | ✅ |
| checkpatch | ✅ 0 errors / 0 warnings |
| Packaging: DKMS (survives kernel upgrades, no manual rebuild) | ✅ 2026-09-07 — `tools/kernel/dkms.conf` + `aur/snd-usb-babyface-pro-dkms/PKGBUILD`; tries plain `make` first, falls back to `LLVM=1 CC=clang` for clang-built kernels (CachyOS). Hardware-validated: built, installed, MOK-signed, and reloaded live via `dkms build`/`install` + `modprobe` on the dev box — all recent controls (Clock Source, Ref Level, Phase, Split, Trim) confirmed present via `amixer` after the DKMS-managed reload |
| Latency profiles discoverable (not just in a README footnote) | ✅ 2026-09-12 — `tools/kernel/snd-usb-babyface-pro.conf`, a fully-commented `modprobe.d` template installed by the DKMS package to `/usr/lib/modprobe.d/`. Shipped commented on purpose: the driver's compiled-in defaults are unchanged (the tree is under upstream review), and 16/16 is NOT safe to default to — it drops ~1 capture sample / 7 s. README's blockquote used to recommend exactly that lossy profile; corrected to **32/8 (0.67 ms)**, which was re-verified live on 2026-09-12: 20 s full-duplex at period 32, 0 xruns either direction, capture file byte-exact (7,680,044 = 48000×2×4×20 + header) |

Hardware test results (2026-08-24, live card):
- 440 Hz playback heard on PH3/4; master mute/unmute verified by ear.
- Voice capture on AN1 verified (48V + gain 35 → RMS ≈ −25 dB,
  envelope tracks speech).
- Phantom P48 LED follows the control; PAD relay clicks on both edges.
- Gain control changes the level; rate classes exact to the frame.

### Known gaps / next steps (in order)

1. **Latency tuning** (the whole point of the kernel driver): defaults
   are 256 frames/URB × 8 URBs — the RME TotalMix 256-sample parity.
   Module params `frames_per_urb` (8..1024, mult. of 8) / `nurbs`
   (1..16) tune it down; the round-trip is measured phase-anchored
   with `tools/kernel/looplat.c`.

**MEASURED 2026-08-24 (32 frames/URB × 3 URBs, live loopback, phase-
anchored with `tools/kernel/looplat.c`):** the true loopback round-trip
latency is **138-170 frames = 2.9-3.5 ms** at 48 kHz — a 32-frame
(0.67 ms) step = exactly one URB, so the latency is quantized to the
URB boundary and does NOT depend on the app's buffer/period size.
(Earlier "2.21 ms" readings were a start-phase artifact of the naive
first-impulse method.)  Full sweep `tools/kernel/latency-sweep.sh`:

| channels | period (frames) | buffer (frames) | latency | xruns |
|---|---|---|---|---|
| 2 | 192-8192 | 384-16384 | 2.88-3.54 ms | **0** |
| 12 | 32-8192 | 64-16384 | 2.88-3.54 ms | **0** |

**REVISED 2026-08-25 (evening, real-audio full-duplex under PipeWire):**
- The min-PERIOD_BYTES clamp (`4 × 12 × frames_per_urb`) inflated the
  2-ch period floor to 1536 frames (32 ms).  Constraint is now
  `PERIOD_SIZE ≥ frames_per_urb` (frames), so the 2-ch floor = one URB
  at any channel count.
- The DSP engages with URBs down to **16 frames** (the old "256-frame
  URBs only" note was wrong).  Validated sweep (48 kHz, tone via
  PipeWire, full-duplex playback+capture):

| frames/URB | nurbs | period (2ch) | result |
|---|---|---|---|
| 256/128/64 | 8 | = fpu | 100 % stable |
| 32 | 8 | 0.67 ms | 100 % stable |
| 16 | 8 | 0.33 ms | light cutouts |
| 16 | 16 | 0.33 ms | pb rock-solid; cap ~1 drop/7 s (soak) |
| 8 | 16 | 0.17 ms | ~90 % (slight crackle) |

  Floor: **frames_per_urb=16, nurbs=16 → period 16 = 0.33 ms @ 48 kHz**
  (monitoring-grade; period 32 = 0.67 ms is the zero-glitch floor for
  recording — see the soak evidence below).
  The loopback of each output lands on the record words 2×out
  (AN1/2 → words 0/1 = capture ch0/1; the words 12/13 = ch10/11 are a
  separate fixed-gain playback tap used as the latency anchor — see
  the Loopback section of PROTOCOL.md).

  **REVISED 2026-09-06 (empirical, from TuxMix's VU-metering work):**
  the "ch10/11 = a separate, independent fixed-gain playback tap"
  framing above doesn't hold as stated. Test: routed a live mic signal
  (AN1) into the PH3/4 output bus via its own crosspoint (`amixer
  'AN1',14` → 100%), engaged `Loopback,1` (PH3/4), and captured all 12
  channels (`arecord -D hw:0 -f S32_LE -c 12 -r 48000`) while tapping
  the mic. Per-100ms-block peak analysis across an 8s capture: **ch2
  and ch3 track ch0 (AN1) exactly, sample-for-sample, at a fixed +3dB
  offset, every single block** — solid confirmation of the "words
  2×out" formula for PH3/4 (output pair 1, 2×1 = words 2/3). **ch11
  tracked the same signal just as tightly** — not independent of the
  looped-back output at all in this test. ch10 showed only a much
  weaker, inconsistent correlation (near noise floor, one early
  transient), unlike ch2/3/11's rock-solid match. Only one output
  pair/source combination was tested (PH3/4 loopback, AN1 as source) —
  not enough to fully re-derive what ch10/11 actually is, just enough
  to say "independent fixed-gain latency anchor" isn't it. Worth a
  proper re-investigation (try looping a *different* output pair, and/
  or actual software playback instead of an input→output loopback, to
  see whether ch11 tracks the specific looped bus or something more
  global like "whatever's currently on the primary monitor path").
2. **Multi-channel PCM** — DONE (see the status table above:
   `channels_max = 12`, 12-ch capture + playback hardware-verified,
   marker words skipped). This entry was stale — left listed as an
   open gap for a while after the table above had already marked it
   done, caught 2026-09-07 while cross-checking the two against each
   other. Originally described as "the full 14-channel frame"; what
   actually shipped is 12 (AN1-4 + the 8 ADAT words) — SPDIF isn't a
   separately addressable pair of PCM channels in the captured
   protocol, so 12 is the real ceiling here, not a shortfall against
   14.
3. **Full control set**: crosspoints (the TotalMix matrix ~hundreds of
   controls), EQ bulk uploads (0x0A), pitch/varispeed (0x1B DDS quads),
   loopback/MS/AN1>2/width flags — ALL DONE. **2026-09-06: the
   remaining 4 (stereo split, ref level, clock source keepalive, phase
   invert) are now done too**, hardware-validated on the real card
   (round-trip via `amixer` + survives an unbind/rebind cycle):
   - **Clock source** (`Sample Clock Source` enum, Internal/Optical In)
     — matches the name TuxMix's ALSA backend already looked for, zero
     Rust-side changes needed. Along the way, fixed 3 call sites that
     hardcoded the keepalive word to `0x0001` ("always Internal") —
     composed from tracked state now (`bf_settings_write`), or Optical
     would have silently reverted on the next pitch change or PM
     resume.
   - **Ref level** (`Instrument Ref Level` enum, +4dBu/-10dBV/Boost) —
     a single shared switch for the Instrument pair, not per-channel
     (no separate bits exist for IN3 vs IN4). Boost's distinguishing
     0x21 commit value (0x0003) is a one-shot, not a persisted register
     bit — `bf_preamp_state_write` now re-asserts it on every preamp
     write (any phantom/PAD toggle included), or Boost would silently
     degrade to plain -10dBV the next time anything else touched the
     shared preamp byte.
   - **Phase invert** (`<mic> Phase Switch`, AN1-4) — bitwise-NOT of
     the crosspoint value, all 6 outputs + the AN1/2 low-map shadow.
     **Known limitation, same class as TuxMix's own `usb.rs::set_phase`
     (not fixed there either)**: this negates the *current* crosspoint
     value once, at toggle time — a later fader move on the same
     [out][mic] slot writes the plain value, silently un-inverting
     phase. Making the crosspoint hot path itself phase-aware would
     close this properly; out of scope for this pass (touches all 84
     crosspoint controls' write path), flagged rather than silently
     shipped.
   - **Stereo split** (`<PBx> Stereo Split Switch`) — fixed constants
     (no fader dependency, unlike phase), AN1/2 destination only, same
     scope as CUE/mute/solo's own low-map reach.
   - New `BF_REG_LOWMAP_BASE_L`/`_R` constants generalize the low-map
     addressing MS-proc/width already used ad-hoc with hardcoded
     addresses (unchanged, just now named).
   - `sh selftests.sh`: laws + module build + `checkpatch` all still
     pass.
   - **Input Trim** (`<name> Trim Volume`, AN1-4) added same day, the
     last of the 5 upstream follow-ups — closes the list. Two curves
     combine (matching `tuxmix-usb`'s own already-shipped Rust
     reference, `cap_trim2/3/4.pcap`): the low map holds the trim ALONE
     on the MASTER curve, the standard map holds fader+trim SUMMED on
     the FADER curve (reused the existing static `bf_fader_raw_to_db2`/
     `bf_fader_db2_to_raw` helpers already written for the front-panel
     wheel, forward-declared rather than moved). Always writes all 8
     registers for the pair; the pair base is derived (`mic & ~1`) so
     the write lands correctly regardless of which channel's control
     triggered it. Two known limitations kept, not hidden: (1) exposed
     as 2 independent per-channel ALSA controls even though the
     hardware register is genuinely shared per pair — matches
     `tuxmix-usb`'s own per-channel `InputChannel::trim` model exactly,
     not a new problem; (2) same composition gap as Phase — a later
     fader move on the same crosspoint slot silently drops trim from
     the combined register (out of scope, same reasoning). Hardware-
     validated: round-trip via `amixer` (including a negative dB
     value), no dmesg errors, correct persistence across unbind/rebind.
     `sh selftests.sh`: laws + module build + `checkpatch` all pass.
     **Limitation (1) turned out to be a real bug, fixed 2026-09-13**:
     because the two per-channel controls tracked `chip->trim[]`
     independently, `bf_state_apply_flags` - which replays a pair from
     its EVEN index - replayed the wrong value whenever the user had
     set trim on the ODD channel (AN2 or AN4). Reproduced on hardware
     with a temporary `dev_info` in `bf_trim_apply` (same technique as
     the 2026-09-07 phantom root-cause): set `AN2 Trim Volume = -12`,
     unbind/rebind, and the trace showed `apply mic=0 trim_db2=0` with
     the cache holding `[0,-12,0,0]` - i.e. the -12 dB was silently
     dropped on the wire while the control still claimed it. Note the
     ALSA controls CANNOT reveal this: they read back from the cache,
     which was always restored correctly; only the wire write was
     wrong. Fixed by making `bf_trim_put` mirror the value into both
     entries of the pair and `snd_ctl_notify` the sibling control (new
     `chip->trim_kctl[4]`, same idiom as `panel_kctl`), so the cache
     can no longer claim a per-channel split the hardware cannot
     represent. Re-verified with the same trace on the fixed build:
     `apply mic=0 trim_db2=-24 cache=[-12,-12,0,0]` after rebind, both
     controls reading -12, dmesg clean. Trace removed afterwards.
   - **dB TLV metadata added 2026-09-13** on the three volume families
     that lacked it (only the 6 output masters had any): the 84
     crosspoints (`DECLARE_TLV_DB_LINEAR(TLV_DB_GAIN_MUTE, 600)` - the
     fader law is exactly linear in amplitude, `BF_FADER_TOP` = 0x2d41
     = 2 x `BF_FADER_0DB` = 0x16a0, so +6 dB), the 4 preamp gains and
     the 4 trims (`DECLARE_TLV_DB_SCALE`, both controls' values already
     being dB). Verified live: `amixer cget` reports
     `access=rw---R--` with `dBscale-min=0.00dB,step=1.00dB` (gain),
     `dBscale-min=-65.00dB` (trim) and `dBlinear-...,max=6.00dB`
     (crosspoints). FX Send deliberately has no TLV: its level law was
     never calibrated to dB (PROTOCOL.md only records "a level
     sweep").
4. **Front panel** — DONE (2026-08-26, `panel.c`): the 0x17 readback is
   polled at 50 Hz in a delayed_work and mirrored into read-only ALSA
   controls (Front Panel Button/Wheel/In/Out/Mix/Dim).  What remains:
   TuxMix user-space translating those controls into mixer writes (the
   host is in the loop, like TotalMix).
5. **PM hardening**: full suspend/resume with mixer-state restore
   (device has no readback for faders — mirror TotalMix's re-apply).
   **Root-caused 2026-09-07 (not a driver bug)**: the anomaly flagged
   2026-09-06 — `Phantom Power Mic 1` reading back "on" after an
   unbind/rebind where it should have been "off" — reproduced
   consistently (2/2) with temporary `dev_info` tracing added at
   `bf_state_save`, the probe-time `0x17` readback, and right after
   `bf_state_restore` returns. The trace showed `chip->preamp` staying
   correctly `0x0000` (off) through every stage of the driver's OWN
   save/restore path, and confirmed `bf_panel_set_phantom` (the
   front-panel SET-press handler, the other candidate) never fired.
   The actual cause is external to this driver entirely: udev's stock
   `90-alsa-restore.rules` runs `alsactl restore` on every card
   (re)appearance — including every unbind/rebind — from
   `/var/lib/alsa/asound.state`, which still had `Phantom Power Mic 1`
   (index 0) stored as `true` from earlier the same session (set via
   `amixer` during testing, never persisted with a matching `alsactl
   store`). Confirmed the fix: `sudo alsactl store` to sync the state
   file, then re-ran the same unbind/rebind cycle for both an off case
   and an on case — both restored correctly, proving the driver's own
   `bf_state_save`/`bf_state_restore` mechanism was never broken.
   **Practical implication**: `--mixer-restore` testing should
   `alsactl store` first, or a stale system-wide state file will look
   exactly like a driver bug — `regress.sh` *already* does exactly
   this (see its own `--mixer-restore` section, dated 2026-08-26, same
   root cause documented there already). The 2026-09-06 flag was raised
   from an ad-hoc manual `amixer` test that skipped that step, not from
   `regress.sh` itself; cross-checking existing knowledge before
   treating this as a fresh mystery would have caught it immediately.
   A real end user hitting this would only be affected by their OWN
   stale `asound.state`, same as any other ALSA device — not specific
   to this driver. Diagnostic `dev_info` calls were removed after
   confirming the root cause (not left in the shipped driver).
6. **PipeWire**: the kernel card should replace the tuxmix ALSA plugin
   as the system sink/source (PW resamples; the mixer stays in TuxMix).
   **Status 2026-08-24 (retested)**: the PW SINK works (tone heard via
   `alsa_output.usb-RME_...-05.stereo-fallback`); the PW SOURCE also
   carries the signal — an earlier "source broken" report was a FALSE
   POSITIVE: the mic chain was silent (no 48V ⇒ the Triton Fethead is
   dead, so zero signal) and the "direct capture signal" was a startup
   burst of non-audio words (incl. the 48000 rate word), not voice.
   With 48V + gain set, pw-cat -r from the kernel source and arecord on
   hw:3 record the SAME audio (back-to-back A/B within 1 dB, mic
   fluctuation excepted). The kernel card is usable as the PW
   sink/source as-is.
7. **Front-panel worker + upstream**: poll the 0x17 readback in a
   kernel worker (MIX/SELECT/wheel state) once the mixer is feature
   complete; then move into sound/usb/ (Kconfig entry — see
   tools/kernel/Kconfig), MAINTAINERS entry, docs, and submit to
   linux-sound/linux-usb. Optionally ask RME for protocol docs
   (clean-room RE is fine, vendor input de-risks the rest).
8. **Automated regression suite (2026-08-25, `tools/kernel/regress.sh`
   + `pcmxrun.c`)**.
   **IMPORTANT (found 2026-09-13): the suite was silently unrunnable
   from 2026-09-01 to 2026-09-13.** The v2-review fix that switched the
   PCM from S24_LE to S32_LE (`db15277`) never touched the test
   tooling, which kept asking the driver for S24_LE on a `hw:` device -
   a format it no longer accepts. The failure did not look like a
   format error: `regress.sh`'s device-busy probe is itself an `aplay
   -f S24_LE`, so the suite aborted with "FATAL: hw:0,0 still busy
   after destroying the RME nodes", which reads as a PipeWire problem.
   Fixed 2026-09-13 across `regress.sh` (4 aplay/arecord call sites),
   `pcmxrun.c` and `looplat.c`. The scaling constants had to move with
   the format: S24_LE is 24 bits RIGHT-justified in 4 bytes (full scale
   2^23) while S32_LE with 24 msbits is LEFT-justified (full scale
   2^31), so `pcmxrun`'s tone amplitude and its dBFS divisor both
   became 2^31, and `looplat`'s impulse/threshold constants went
   0x400000 -> 0x40000000. Confirmation the conversion is right: the
   signal tap reads -24.1 dB again, exactly the historical baseline.
   **Consequence for the RFC: the "40/40" figure quoted in
   UPSTREAM.md and in the v1/v2/v3 cover letters predates the S32_LE
   switch** - the suite had never been run against the code actually
   submitted as v3, nor against the 5 controls added 2026-09-06. It
   has now: 40 pass / 0 fail on 2026-09-13 (`--dur 1 --mixer-restore
   --disconnect-test`), covering the 9 rates x 4 periods sweep, 30
   start/stop cycles, mixer-cache restore across unbind/rebind and a
   mid-stream disconnect.
   What it does: full-duplex sweep (9 rates × periods ≥
   max(fpu,32)) with a mic-free signal-integrity check (the device
   playback-tap on capture ch10/11), start/stop stress, and
   --mixer-restore (48V+gain survive unbind/rebind).  It caught two
   real driver bugs, both fixed: (a) playback overrunning the app ring
   at small buffers (hw_ptr past appl_ptr → spurious XRUN) — the copy
   is now clamped to the written frames; (b) capture dying silently at
   176.4/192 kHz with frames_per_urb < 32 (alt-3 packets are 1024 B →
   -EOVERFLOW babble) — min_fpu per alt (8/16/32) is enforced in
   hw_params.  38/38 PASS at the 256×8 and 16×16 profiles.
9. **USB error-path hardening (2026-08-25, `15d06fc`)**: consecutive
   URB errors (bad status or failed resubmit) now stop the stream and
   wake the substreams with XRUN (apps get -EPIPE and re-arm) instead
   of resubmitting into a silently dead stream; disconnect stops the
   running substreams with DISCONNECTED so blocked apps wake promptly
   (verified: arecord exits rc=1 immediately on an unbind mid-stream).
   Suite is now 40/40 with `--disconnect-test`.
10. **Source split (2026-08-25)**: the 2600-line monolith became
    `snd-usb-babyface-pro.h` (shared state + externs) + `main.c`
    (probe/disconnect/PM/entry), `protocol.c` (vendor requests, cold
    init, rate table), `pcm.c` (interrupt-URB stream + PCM ops),
    `mixer.c` (controls), `state.c` (mixer-state persistence).
    Reproducible via `tools/kernel/split_driver.py`; checkpatch 0/0
    on every file; the 40/40 suite passes on the split build
    (byte-identical glue — only `static`→`extern` on cross-file
    symbols).  Makefile: `snd-usb-babyface-pro-objs := main.o
    protocol.o pcm.o mixer.o state.o`.
11. **Soak / robustness evidence (2026-08-25, `pcmxrun` long runs)**: 10 min
    full-duplex at the 256×8 default with a mixer storm (gains, masters,
    crosspoints changed mid-stream) — 0 xruns, tap stable at −24.1 dB,
    dmesg clean.  15 min of 48↔96 kHz rate switching (16 cycles) — 0
    xruns, dmesg clean.  5 min soaks at 16×16: period 32/64 = 0 xruns;
    period 16 = 0 playback + ~42 capture overruns (the 0.33 ms ring
    drops ~1 buffer per 7 s under any scheduler hiccup — fine for
    monitoring, not for clean recording).  Conclusion: the driver is
    glitch-free at period ≥ 32 across the matrix; period 16 is the
    aggressive monitoring floor.
12. **Suspend/resume (2026-08-25, tested on this laptop, deep S3)**: a
    full suspend→resume cycle with a distinctive mixer state set
    (48V ON, gain 35, master 8192) — the card survives in place (no
    USB drop on this box), the state is intact after resume, dmesg is
    clean, and full-duplex 48k/96k runs 0-xrun afterwards.  The
    re-probe path (USB drop → disconnect + probe with mixer-state
    cache) is covered separately by the suite's --mixer-restore.
13. **Physical hot-unplug/replug (2026-08-25, live card)**: unplugged
    the USB cable mid-stream (aplay on hw:3,0).  The disconnect path
    fired cleanly ("disconnect: stopping PCM substreams"), the app
    exited without hanging, and on replug the card auto-re-probed
    (new USB device number, same card 3) with 48V + gain restored
    from the cache — no kernel errors, full-duplex 0-xrun after.
    Only the master volume is re-owned by the system afterwards
    (alsactl/asound.state + WirePlumber — see the re-probe note).
14. **Upstream-prep pass (2026-08-28)**: a real kernel bug found and
    fixed — `bf_eq_get`'s Low Cut Slope case returned the raw stored
    dB value instead of the ALSA enum index, confirmed broken via raw
    `amixer cget`/`sset` independent of any userspace code, now fixed
    and re-verified live. `panel.c`'s boot-window SELECT re-assert had
    an inverted `time_is_before_jiffies`/`time_is_after_jiffies` check
    (re-asserted the power-on state *after* the 3 s window instead of
    during it). `sparse` (C=1/C=2) run for the first time: 8 real
    warnings (missing `static` on two file-local arrays, `int` vs
    `snd_pcm_state_t` on `babyface_pcm_stop_both`, two frame pointers
    that should have been `__le32*` not `u32*`) — all fixed, `sparse`
    and `W=1` now both clean. USB autosuspend (previously untested,
    nothing paused the panel poll/keepalive for it) explicitly
    disabled via `usb_disable_autosuspend()`/`usb_enable_autosuspend()`
    rather than left as a live untested path; the front-panel poll
    interval is now a module param (`panel_poll_ms`, 10–1000 ms,
    default unchanged) instead of hardcoded. **Six-file → two-file
    squash**: `main.c`/`protocol.c`/`pcm.c`/`state.c` merged into
    `babyfacepro.c` (core driver); `mixer.c`/`panel.c`/`eq.c` merged
    into `babyfacepro-ctl.c` (ALSA control surface);
    `snd-usb-babyface-pro.h` renamed `babyfacepro.h` — matching the
    upstream target layout in `UPSTREAM.md`. All of the above build +
    build + `sparse`/`W=1`/`checkpatch` clean and were live-tested (module
    reload, `amixer`, `dmesg`) on the physical unit.
15. **Runtime buffer reconfiguration** (roadmap — NOT yet implemented):
    switch the latency profile **on the fly** (e.g. 256-sample default
    ↔ 16-frame low-latency floor) without unloading/reloading the
    module, the way the Windows Fireface USB Settings panel changes the
    buffer size while the device is running.  Today `frames_per_urb` /
    `nurbs` are module params read **once in `probe()`** — the URB pool
    is sized from them at probe and is never reallocated, so changing a
    sysfs param at runtime does nothing to a live stream (only the next
    probe sees it); switching profiles currently means `rmmod`/`insmod`
    (or unbind/rebind), which cuts audio and re-runs the mixer-state
    restore.  To close this gap: expose a reconfiguration entry point
    (card sysfs attribute or ALSA control) that tears down and rebuilds
    the URB pool (`usb_kill_urb`/`usb_free_urb` → realloc at the new
    `frames_per_urb`/`nurbs`), re-applies the `PERIOD_SIZE ≥
    frames_per_urb` constraint on the open substream (watch out: a
    stream already running at period 16 would violate a *larger*
    `frames_per_urb` — the constraint must be re-negotiated), and stops/
    resumes any active stream cleanly (XRUN or suspend, not a full card
    reset).  Deliberately a **post-merge follow-up**, not an RFC
    blocker - it touches the streaming core right before submission.
16. **Front-panel state concurrency** (fixed 2026-09-28).
    The read-only panel controls are fed by the 0x17 poll work, which
    decodes into `chip->panel_*`; the ALSA `get` callbacks read those
    same fields from another context, and `panel_select` also has a
    second writer in the SELECT control's `put()`.  Every access to the
    shared fields (`panel_button/wheel/in/out/select/mix/dim`) now goes
    through `READ_ONCE()` / `WRITE_ONCE()`, so the lock-free sharing is
    defined and tear-free instead of a plain data race.  The locking is
    unchanged: a lock cannot be held across the panel handlers, which
    already take `chip->mutex` (an ABBA with `snd_ctl_notify`).
17. **Idle cost of the front-panel poll** (measured 2026-09-28).
    While bound, the 0x17 poll runs at ~47 Hz (`panel_poll_ms`, default
    20).  On the reference unit the RME's xHCI IRQ line (0000:07:00.4)
    goes from ~28 IRQ/s with the module unloaded to ~72 IRQ/s bound and
    idle - **~44 extra interrupts/s** from the driver.  That is
    negligible for CPU and small for idle power, and
    `usb_disable_autosuspend()` deliberately keeps the device active so
    the poll can run.  A lower `panel_poll_ms` (or an adaptive backoff)
    would trim it, but the OUT wheel decode is timing-sensitive (accel
    window 62 ms, fast-poll 5 ms for 200 ms after a click), so a backoff
    needs care - **not worth the risk for the measured gain**.

## ✅ 2026-09-14 — THE AN1/2 CROSSPOINT BUG: FIXED (the "low map" was not a shadow)

**The AN1/2 output's own crosspoint fader had no audible effect on the
signal, for every source, the whole time the matrix has existed.**
Found while trying to hardware-verify the crosspoint fader's TLV curve
(the 2026-09-13 dB-linear TLV added to the crosspoints had never been
checked against a real signal, only against the register-value
arithmetic) - which is exactly why that verification mattered.

- **Symptom**: sweeping a crosspoint fader from off (raw 0) through
  0 dB to +6 dB, with a real signal (a generated tone routed via PB1,
  and separately AN1's own preamp) into the AN1/2 output, produced NO
  change in the captured level - constant at every fader position,
  including "off". The exact same test against the PH3/4 (Phones)
  output, same code, same tone, same measurement, gave a clean
  monotonic response matching the fader law to within 0.6 dB (-62.6 dB
  near "off" to -12.0 dB at +6 dB, expected +6.02 dB span).
- **Root cause**: `bf_xpoint_put()` (the crosspoint ALSA control),
  `babyface_write_default_mixer()` (the probe-time unity default) and
  `babyface_restore_state()` (the re-probe/resume replay) all wrote
  only the "standard" crosspoint map
  (`BF_REG_CROSS_BASE_L/R + BF_REG_CROSS_STRIDE*blk + idx`). For the
  AN1/2 output specifically, PROTOCOL.md's own "Scene load" capture
  already showed the vendor software always writes a SECOND register,
  the "low map" (`BF_REG_LOWMAP_BASE_L/R + idx`, no block multiplier -
  it only exists for this one output), at the same value, every time.
  The driver's own comment called it "only a shadow" and skipped it in
  these three places - wrongly. The low map is what the AN1/2 submix
  actually sums from; the standard map alone reaches a register the
  hardware doesn't act on for this output. Every other output (Phones,
  AS1/2, ADAT×3) has no low map at all, which is presumably how the
  wrong assumption generalized from "5 of 6 outputs" to "all 6".
- **Not a fresh mistake in unfamiliar territory**: `bf_split_apply()`,
  `bf_phase_apply()` and `bf_trim_apply()` already wrote both maps
  correctly for their own AN1/2-only features (stereo split, phase,
  trim) - the pattern was right there to copy. `tuxmix-usb`'s own
  generic fader path (`usb.rs::set_volume`) already keeps the low map
  "in sync... TotalMix writes both" for `Output::An12`; TuxMix's
  `apply_scene` (its state-restore path) does too. The kernel's plain
  crosspoint fader control was the one place, across both codebases,
  this was missed.
- **The fix**: a new exported helper, `bf_xpoint_write()`, writes the
  low map first (when `out == 0`) and then the standard map, replacing
  the triplicated write sequence in all three call sites. Verified
  hardware-fixed with the same tone-sweep methodology that found the
  bug: off/0 dB/+3 dB/+6 dB now read -55.5/-18.5/-14.5/-12.0 dB on
  AN1/2, matching the already-working Phones result almost exactly.
  Confirmed the fix survives a real unbind/rebind with a non-default
  crosspoint value, both the cached control value and the actual audio
  level. `regress.sh --mixer-restore --disconnect-test`: 40/40.
- **Practical impact**: every source ever routed into the default
  output (AN1/2) via its crosspoint fader, rather than left at the
  probe-time unity default, was silently inaudible. The probe-time
  default itself (unity into every source) happened to look correct
  by accident, because `babyface_write_default_mixer()` never touched
  the low map either - meaning AN1/2 has been running on whatever the
  low map held from `bf_cold_init`'s register clear (0x0000, i.e.
  silence) this whole time, and the audible AN1/2 signal any tester
  actually heard came from sources whose low map got a real value via
  a DIFFERENT, AN1/2-specific control (phase/trim/split) rather than
  from the general-purpose fader.  Any earlier "AN1/2 works fine"
  observation almost certainly never moved this specific fader with a
  real signal in the loop - which was itself exactly the gap the
  2026-09-06 test (AN1 into PH3/4) left open, and this investigation's
  starting point (see the "v4 verification" thread below).
- PROTOCOL.md's "writing either (or both) is safe" claim about the two
  maps is corrected in place.

## Power-on defaults and card naming (decided 2026-09-13)

- **Masters come up at -20 dB**, not at TotalMix's 0 dB - **on the two
  analog outputs only** (AN1/2, PH3/4), narrowed 2026-09-15 after
  David Fredman pointed out the original blanket six-output default
  reached the four digital outputs too (AS1/2, ADAT3/4, ADAT5/6,
  ADAT7/8, all carried over the single optical port). Nothing
  downstream of a digital output can be damaged by a loud signal the
  way a speaker or a pair of headphones can, so there is no hazard to
  mitigate there, only a feed that would otherwise arrive 20 dB quiet
  for no reason a receiving device could infer - those four keep
  TotalMix's own 0 dB default. On the two analog outputs the six
  playback channels SUM, and the default is re-applied on every fresh
  module load, before udev's `alsactl restore` can put the user's own
  levels back. The -20 dB value is the exact 8-bit/16-bit pair
  (`0xcb` / `0x0333`) that the hardware's own DIM button writes,
  captured in `cap_dim2.pcap`, so it is measured rather than chosen.
  **Consequence**: DIM applies an *absolute* -20 dB on Phones, so with
  the new default it does nothing audible there until the Phones
  master is raised above -20 dB. That is how the hardware has always
  behaved; it is simply now visible from the first second.
  **Verified live** (2026-09-15): all 6 masters read back correctly
  split (819/819/8192/8192/8192/8192) both on a genuinely fresh
  `alsactl` state (no prior entry for this card) and after a
  `rmmod`/`insmod` cycle with the state file back in sync -
  `regress.sh --mixer-restore --disconnect-test`: 40/40. Chasing this
  down on hardware hit the same `alsactl restore` trap the 2026-09-07
  phantom-power incident already named: a stale system-wide
  `asound.state` entry (819 for every master, cached during earlier
  testing before this scope change existed) silently overwrote the
  driver's freshly-computed correct defaults about two seconds after
  every probe, for over half an hour of otherwise-inexplicable
  results, until `sudo alsactl store` resynced it. Same lesson as
  before: `--mixer-restore` testing (and any power-on-default check)
  needs a synced state file first, or a stale one looks exactly like a
  driver bug.
- **The routing default routes the playback only, not the inputs**
  (changed 2026-09-28). A fresh probe with no saved state used to route
  all 14 sources into every output at unity, summing a live mic or line
  input straight into the main out and the headphones the moment the
  module loaded - the hazard issue #4 raised, and the "mic audible in
  the phones" surprise. `babyface_write_default_mixer()` now routes the
  six playback channels (PB1-6, `BF_SRC_PB1`) to every output at unity
  and leaves the hardware inputs (AN1-4, AS1/2, ADAT) out of every
  output; raising an input's crosspoint in a mixer is what monitors it.
  The defaults still land before `alsactl restore`, but a fresh load can
  no longer blast a live input into the outputs.
- **DIM's scope is Phones-only, and that is confirmed faithful to the
  hardware, not a driver limitation - with one real open question.**
  PROTOCOL.md's own capture states plainly: "DIM only ever touched the
  Phones (out 1) in this capture." But the same section also documents
  that "Main Out" - what DIM actually targets - is a TotalMix-side
  software setting, reassignable, and DIM is itself described there as
  "also a configurable hotkey (Speaker B, Talkback...)". That means
  the *protocol* is very likely capable of writing the DIM burst to a
  different output's master registers when TotalMix's Main Out is
  reassigned - we simply have never captured that case, only the
  default (Main Out = Phones). David Fredman raised this
  (2026-09-14, issue #4): on a monitor setup where speakers live on a
  different output, DIM currently does nothing audible. **Not fixed**:
  guessing which output to target without a capture showing what a
  reassigned Main Out actually writes risks introducing behavior no
  evidence supports, which is worse than the current honest gap. The
  right next step is a fresh Windows capture (set Main Out = AN1/2 in
  TotalMix, press DIM, see what changes) - flagged as an open protocol
  question in PROTOCOL.md rather than guessed at in code.
- **The front-panel DIM button is reported, not acted on** (changed
  2026-09-17, issue #4). Each press increments the read-only
  `DIM Button Press Count` control and sends a change event; the driver
  no longer toggles `Dim Switch` itself. The fixed action dimmed Phones
  to an absolute -20 dB, which raised quieter levels and missed setups
  whose speakers are on AN1/2. The RME manual (p. 91) describes DIM as a
  20 dB reduction from the current level on the selected output, so the
  action is left to a mixer application such as TuxMix. `Dim Switch`
  keeps its old behaviour. The notes below describe the earlier wiring.
- **The front-panel DIM button acts** (wired 2026-09-13, confirmed on
  the physical unit the same day): two presses gave two `Dim Switch`
  and two `Front Panel Dim` events, clean toggle round-trip, no
  deadlock between the control path and the panel work. Note the press
  moved `Front Panel Dim`, NOT `Front Panel Button` - which does not
  match the description in issue #4 ("pressing it changes Front Panel
  Button and nothing else"). Not chased, since it does not affect the
  fix, but worth knowing before relying on `Front Panel Button` to
  observe a DIM press.
- **Card naming is model-neutral**: `card->driver` = `BabyfacePro`,
  shortname `Babyface Pro`, id `BabyfacePro`. The FS and the 2015
  non-FS share VID:PID, bcdDevice and iProduct shape - the FS's own
  `iProduct` is "Babyface Pro (73055480)", with no "FS" in it - so
  there is nothing to branch on at probe. `card->driver` is what
  alsa-lib and UCM match on and is effectively frozen by a kernel
  merge. The id is derived caiaq-style from the shortname rather than
  left to the core, which gave the last word only (`hw:FS`, then
  `hw:Pro`).

## Re-probe resilience: the usbfs claim (2026-08-24, diagnosed)

**LOCKED OUT 2026-08-26 (`80c5fff`)**: `BabyfaceUsb::open()` (tuxmix-usb)
refuses with `KernelDriverBound` when `snd-usb-babyface-pro` owns the
interface (a scan of the driver's sysfs dir) — libusb's
USBDEVFS_DISCONNECT_CLAIM would unbind the kernel driver and the card
vanishes mid-session.  The conflict is no longer just documented: it
is refused up front.

**Stream error paths hardened (`80c5fff`)**: a failed stream START now
wakes the apps with an XRUN and re-counts `stream_users` (the old
`err:` path left them RUNNING with no URBs — hang); the trigger's
`stream_users` ++/-- is under `chip->lock` (the two substreams have
separate ALSA locks — a lost increment stopped the stream mid-run);
broken `SNDRV_PCM_INFO_PAUSE` removed (PAUSE_RELEASE reset hw_ptr to 0).
`tools/kernel/selftests.sh` runs the law selftests + the build +
checkpatch without the card; the hardware regress stays `regress.sh`.

A userspace client can **claim the proprietary interface via usbfs**
(`USBDEVFS_DISCONNECT_CLAIM`), which silently detaches the kernel
bound driver — the ALSA card vanishes from /proc/asound/cards for the
duration (no USB disconnect/reset logged; confirmed by kprobe on
`usb_unbind_interface`: `device_release_driver_internal ← usbdev_ioctl
← ioctl`, from the **pipewire** process). Observed trigger:
`pw-cat -r --target <sink>` (record-from-sink) — PipeWire claims
iface 5 for the record duration, then releases → re-probe → card back
(~3 s later). Normal playback (`pw-cat -p`) and recording from the
SOURCE do NOT trigger it. Confirmed again 2026-08-24: merely opening
the KDE sound-settings panel (plasma-pa → wireplumber inspecting the
device) kills the card the same way. The TuxMix user-space daemon
(rusb/libusb) claims the same interface — run it OR the kernel driver,
not both.

**ROOT CAUSE (fully identified 2026-08-24):** the claim does NOT come
from PipeWire's core — it comes from the **TuxMix user-space ALSA
plugin** (`libasound_module_pcm_tuxmix.so` → `libtuxmix_sys.so` with
vendored libusb), which PipeWire loads in-process for the "tuxmix"
sink/source nodes (`~/.config/pipewire/pipewire.conf.d/50-tuxmix.conf`
+ `/etc/alsa/conf.d/50-tuxmix.conf`).  When the sound panel touches the
TuxMix device, libusb's auto-detach does `USBDEVFS_DISCONNECT_CLAIM`
on interface 5 (ftrace: `usbdev_ioctl cmd=0x8108551b` from the
pipewire data-loop thread) and then streams the device itself
(SETINTERFACE + SUBMITURB + REAPURBNDELAY burst).  The freeze the user
sees = the window where the kernel card is disconnected.

**FIX (applied):** with the kernel driver active, disable the tuxmix
PipeWire nodes — `mv 50-tuxmix.conf 50-tuxmix.conf.disabled` + restart
pipewire.  The kernel card is then the only Babyface device; the TuxMix
GUI/TUI still work (they use libtuxmix_sys directly, not the plugin).
Keep ONE stack wired into the sound system at a time.

**Fix (implemented):** the driver now saves the full mixer state at
disconnect (module-global cache keyed by device serial, `bf_saved`)
and restores it at the next probe (48V/PAD, 4 gains, 6 masters+
mutes, 84 crosspoints, pitch, loopback/AN1>2/link/width/FX/MS —
verified live: 48V + gain + a crosspoint set to 3000 all survived an
unbind/rebind cycle). Master VOLUME still gets overridden after the
restore by external agents, which is normal system behavior — pinned
down 2026-08-25: udev runs `alsactl --export restore` on every card
add, and `asound.state` DOES hold a `state.FS` block, so alsactl
writes back the last-saved playback volume (the stale 983 seen in the
replug test — the driver itself had restored 8192 at probe, rc=1);
WirePlumber then applies its own volume policy on node activation.
Phantom/gains/crosspoints have no
system equivalent and persist untouched.

**Removed (2026-10):** asound.state holds every control of the card,
so the `alsactl restore` udev runs on each card add overwrote the
restored state with the last stored one anyway, phantom, gains and
crosspoints included.  The TuxMix plugin that caused the usbfs claims
refuses to claim a bound interface since `80c5fff`.  A re-probe now
starts from the default mixer and alsactl, as for any card, and a
resume that resets the device goes through `reset_resume` instead of a
re-probe.

Test the full cycle:
```sh
amixer -c 3 cset numid=13 1; amixer -c 3 cset numid=17 65   # 48V AN1 + gain
SINK=$(pw-dump | python3 -c '...' )                        # kernel sink id
timeout 3 pw-cat -r --target $SINK ...                     # kills the card
sleep 4; amixer -c 3 sget ...                              # state restored
```

## PipeWire volume + stream-start fixes (2026-08-25, hardware-verified)

> **SUPERSEDED for the system volume (2026-08-26)**: the "sound panel
> volume drives the Phones master" model below was REVERTED — the
> Windows system volume is a host-side stream gain (cap_sysvol2.pcap),
> so the kernel sink now uses PipeWire SOFTWARE volume (the per-output
> master names leave no "Master" element for SPA → automatic software
> fallback, verified).  The TLV work below still stands (the masters
> are plain mixer controls with a correct dB law for the GUI).

Three fixes landed together to make the kernel sink behave like a normal
sound card in PipeWire:

1. **Stream start = full cold-init + state restore.**  (Superseded
   2026-09-26: a session start no longer runs the cold init, see below.)
   The firmware only validates a session preceded by the complete
   cold-init; without it the outputs stay silent.  The init wipes the
   mixer registers, so the cached state (preamp, gains, masters,
   crosspoints, pitch) is re-applied right after the arm — the session
   starts at the user's levels and the output is not muted at stream
   start.
2. **Rate change = stop URBs + restart instead of -EBUSY.**  A
   `hw_params` at a different rate while streaming used to fail with
   -EBUSY, which killed the PW sink whenever another stream (e.g. a 44.1k
   capture) ran.  Now the driver stops the URBs, re-points the bandwidth
   class and lets the stream work restart at the new rate (PW resamples
   through the brief rate step).  Zero EBUSY since.
3. **dB TLV on the output masters (`bf_master_tlv`).**  The master law is
   20·log10(v/0x2000) (0x2000 = 0 dB, 0x4000 = +6 dB — the raw value IS
   the linear amplitude), declared as a DB_RANGE with two DB_LINEAR
   items.  Before the TLV, WirePlumber could not map its volume to the
   control and applied a **software volume ~0.03** on top → the output was
   ~30 dB down / inaudible.  With the TLV (control access gains
   `TLV_READ`), WirePlumber drives the hardware master 1:1:
   `wpctl set-volume` moves the Phones register (1.0 → 0x4000 = +6 dB,
   0.5 → ~0x07F9 ≈ −12 dB) and `softVolumes` stays [1.0, 1.0] — no
   software volume at all.  The data path is untouched (loopback RMS
   identical at sink volume 0.5 and 1.0).

**Measured parity (loopback AN1/2 OUT → capture ch10/11, 440 Hz sine at
−8 dBFS source, 48 kHz):** aplay direct = −9.38 dBFS RMS; pw-cat via the
kernel sink at volume 1.0 = −9.55 dBFS RMS (≈0.2 dB conversion residue,
S32→S24).  Loopback round-trip latency still 2.88 ms / 0 xruns (looplat,
period 192 and 512).  The sound panel volume (Phones master, control
index 0) now controls the headphones end-to-end.

Known follow-up (Windows-side calibration, not a driver bug): WirePlumber
maps its slider to the hardware law with its own curve (0.5 → −12 dB,
0.25 → −30 dB on the Phones register); the exact dB-per-position
alignment with TotalMix's fader is a Windows calibration task.

## System-volume model — CORRECTED AGAIN 2026-08-26 (cap_sysvol2.pcap): Windows volume = HOST-SIDE stream gain, zero USB writes

**CONFIRMED 2026-08-26 by cap_sysvol2.pcap (clean re-extraction,
`tools/usbdump/sysvol_amp.py`): the Windows "Speakers" volume is a
HOST-SIDE software gain in the WDM driver.**  The capture (60 s,
continuous stereo tone at RMS 0.25 ≈ −12 dBFS, volume 100% → 0% →
100%) shows: **ZERO control-transfer records in the whole 60 s** while
the OUT ep 0x01 amplitude ramps down 0.250 → 0.0004 (t=0-24 s), holds
≈0 (t=25-30 s), ramps back up 0.0004 → 0.250 (t=31-53 s) — a smooth
host-side fader ramp, no register writes, no TotalMix fader movement.
(An earlier amplitude reading of "3.9G → 0" was an extraction bug: the
24-bit samples were not sign-extended.)

The first capture (cap_sysvol) agrees: t=0-40 s the amplitude dips
with no writes at all; the 0x03E0/0x0004 AN1/2-master writes at the
END were the USER manually moving the AN1/2 hardware fader (user-
confirmed), NOT the volume slider.  The earlier "volume = output
master register" and "volume = PB playback fader" readings both
conflated the user's own fader move with the slider.

So Windows does NOT move any mixer register for the system volume — it
attenuates the WDM stream host-side (per WDM device: Speakers, Analog
3+4, SPDIF/ADAT each have their own host-side volume).  Only the app's
stream is attenuated; the mixer stays untouched (the mic monitored
through the same output keeps its level).

**Implication for the kernel driver**: the current behavior (system
volume = the PH3/4 hardware master, a mixer register) is NOT the
Windows model.  To match Windows, the system volume should be PipeWire's
SOFTWARE volume (host-side, on the sink stream) — configure WirePlumber
to not bind the sink volume to the Phones master control (software
volume), so the mixer registers stay untouched and the mic monitoring
isn't yanked by the OS volume.  The Phones master stays a plain mixer
control (TuxMix GUI).  The earlier "playback faders" plan is moot —
Windows doesn't write those either.

The Windows WDM mapping (Speakers=PB1→AN1/2, Analog 3+4=PB2→PH3/4,
SPDIF/ADAT=PB3→AS1/2) is still the reference for the per-WDM-device
host-side volumes.

## 2026-08-25 later — the PHONES output is MONO (L+R) at the device level

**Finding (hardware + ear-verified)**: the PH3/4 (Phones) output bus
mixes L+R into both channels — a stereo (panning) playback arrives
centred on both ears; the AN1/2 output bus is stereo.  Proven with a
pure usbfs session (lr_test.c, the RE's exact TotalMix init): with
L=440 Hz / R=880 Hz on PB1 routed canonically (PB1 L → L-reg 12, PB1 R
→ R-reg 13 of each block), the AN1/2 record words 0/1 carry L/R
separated (~20 dB) but the Phones record words 2/3 carry L+R mixed on
BOTH words.  The user's ears confirm (a L→R pan tone stays centred on
the headphones).  The kernel driver's routing is correct — this is a
device behavior of the Phones (the manual's "output channels 3/4").

**Default-mixer fix included with this note**: the probe-time default
now writes the crosspoints with the source idx_l/idx_r on the
canonical block (like the stream-start restore) instead of the raw
index on both bases — the old default left the "cross" registers
(L-reg idx_r / R-reg idx_l) at 0 dB, which would put PB1 R on the L
side and PB1 L on the R side.  It does NOT change the Phones mono
(device behavior) but is the correct TotalMix-style default.

**Open**: how TotalMix on Windows delivers stereo Phones (if it does)
— quick Windows check: play a hard-panned tone with TotalMix; if
stereo, a targeted capture of the session-start/assign writes is
needed (the RE init does not produce stereo Phones).  See
WINDOWS-CAPTURE-PLAN Capture 17.

## ✅ 2026-08-25 later — THE MONO PHONES BUG: FIXED (the "cross" crosspoint registers)

**The user reported mono on the Phones (Reaper).  Root cause found +
fixed: the "cross" crosspoint registers of every output block (L-reg
at the odd stereo indices 5,7,…23 and R-reg at the even 4,6,…22) were
never zeroed by the driver.**

- The probe-time default originally wrote ALL 24 raw indices on BOTH
  the L and R bases (PB1 R → the L side, PB1 L → the R side → L+R on
  both channels = mono).
- The first fix wrote the source idx_l/idx_r on the canonical block
  but LEFT the cross registers alone — and the 0x16 cold-init clear
  covers only 0x00-0x3D, so the stale cross values (0x16A0 from the
  old default, or whatever the previous session left) PERSISTED →
  still mono.
- **The fix (`bf_crosspoint_clear_cross`) explicitly zeroes the 20
  cross registers per block, in both the probe default and the
  stream-start restore.**  Ear-verified: an alternating hard-L /
  hard-R tone now plays on ONE ear at a time (was centred on both).
  The loopback record of the Phones bus went from L+R-on-both-words
  to clean L/R separation (lr_test.c, 20 dB).

**Also resolved along the way**: the RE's usbfs init (loopback3.c /
`lr_test.c`) has the same gap — its 0x16 0x00-0x3D clear does not
cover the cross registers either, so stale values pollute the RE
measurements (the earlier "Phones is mono at the device level"
conclusion was this artifact).  Windows TotalMix is stereo on the
Phones by default (user-confirmed with a stereo test video; and the
stereo/mono toggle per Bus is host-side — zero USB writes,
cap_stereo.pcap).  Capture 17 is now RESOLVED (no capture needed).

## 2026-08-25 later — the "no sound" root cause + the PipeWire crackle

**Root cause of "no sound on Phones" = INVERTED MUTE CONTROL**
(`ddf1bd2`): `Master Playback Switch` returned the internal `muted` flag
directly, so the ALSA convention was inverted — writing 1 (the
panel/WirePlumber "unmute") actually MUTED the output, and the default
showed as "off".  The Phones output stayed muted; it only came alive
while a volume write landed (`bf_master_put` clears muted), then the
next stream start's restore re-muted it.  Fixed: the control now
follows ALSA semantics (1 = enabled).

**PipeWire crackle = an xrun stop/start loop** — timer-based scheduling
(tsched) mis-estimates the device position between the interrupt-URB
completions (5.25/5.37 ms jitter) and the sink fell into a ~0.55 s
STOP/START recovery cycle (each restart = cold-init + restore = a
crackle).  Fixed with a WirePlumber rule
(`tools/alsa/wireplumber-rme.conf` →
`~/.config/wireplumber/wireplumber.conf.d/51-rme.conf`):
`api.alsa.disable-tsched = true` + `api.alsa.period-size = 2048`.
Verified: 0 trigger STOPs during a 20 s pw-cat tone (was ~18), the tone
is clean, aplay direct was always clean (the driver was fine).

**2026-08-25 evening**: the shipped config defaults to `period-size =
256` (TotalMix parity, 5.33 ms @ 48 kHz, light on CPU) and sets
`node.description`/`node.nick` = "Babyface Pro FS" (the USB product
string "Babyface Pro (73055480)" would otherwise leak into the
PipeWire UI name).  The validated low-latency DAW profile is
`period-size = 16` with the module loaded `frames_per_urb=16
nurbs=16` (0.33 ms @ 48 kHz) — the ALSA period is floored at
frames_per_urb, so the module param must match.

## 2026-09-19 — the session follows prepare/hw_free, not the trigger

Bitwig on PipeWire went silent whenever a heavy plugin that produced
xruns was added or removed, and stayed silent until something
else touched the device (the front-panel wheel was enough).  Bitwig on
raw ALSA never did.  With dynamic debug on, each add/remove logged five
`stream stopped` / `stream started` pairs within a second: every xrun
recovery (prepare + START) tore the USB session down and ran the full
cold-init again.  Upstream issue #5 is the same symptom from a buffer
size change; the reporter found raw ALSA working in the broken state.

The session now lives from the first `prepare` to the last `hw_free`,
the pattern of the FireWire audio drivers.  `hw_params` counts a
substream (`stream_setup[]`, `stream_users`), `prepare` starts the
session if it is down, START/STOP only gate the URB handlers' copy, and
the OUT handler sends silence whenever no running substream fed it
(before, a set-up but stopped playback substream replayed the URB's old
contents).  `stream_work` is left with the persistent-URB-error path.
Because the URBs now outlive a substream closed while the other
direction runs, the handlers read `chip->subs[]` under RCU and `close`
calls `synchronize_rcu()`.

## 2026-09-26 — a session start sends what the Windows driver sends

Every session start ran `bf_cold_init()` (the 0x16 clear of registers 0x00-0x3d, the varispeed quad, the rate/settings write, keepalives), then `babyface_restore_state()` and `bf_state_apply_flags()` to put back what the clear had wiped.  Timed per phase on the hardware at 48 kHz, that is 1.06 s per start: clear 135 ms, the rest of the cold init 31 ms, trigger pair 4 ms, URBs and arm 2 ms, state restore 805 ms, flags 85 ms.  The WirePlumber probe after hotplug opens the device several times and pays this each time.  These times were measured through a chain of three USB hubs, where each control write took about 2.25 ms; on a direct port a write takes about 0.25 ms (the probe's cold init, 74 writes, in 18 ms), the same spacing as in the Windows capture.

The RME Windows driver sends no init at a stream start (`cap_audio.pcap`, `tools/usbdump/PROTOCOL.md`): the trigger pair `0x10 0x8000` + `0x1D`, the ISO endpoints, the arm `0x14 0xC000`.  The 0x16 clears appear only in the cold-plug capture.  A session start now does the same, preceded by the rate write (`bf_clock_write()`: family register and settings word, which is also what Windows sends on a rate change).  The cold init and the full restore stay at probe and resume, where the device state is unknown; both now also re-apply the flags, which the next session start used to do.

Checked with a patch cable from the PH3/4 jack into IN3/IN4, only PH3/4 live and no input routed to any output, one session per measurement: the tone arrives at the same level with the old and the new start (-29.4 dBFS both channels) at 48, 44.1 and 32 kHz and after a rate change between sessions; a master change made while no session runs shows up in the next session (+10.0 dB, and back).  All nine rates measure correct through ALSA (`ratecheck`, within +0.003 %).  A session start takes about 1 ms on a direct port (9-13 ms through the hub chain).

One thing the cold init did cover: a session triggered less than about 15 ms after the previous one stopped comes up with the outputs silent (and nothing on the FX send), for the whole session.  On a direct port, with back-to-back `pcmxrun` runs and a minimum time from the stop to the trigger: 12 ms left 18-20 of 20 sessions silent, 14 ms 4-9 of 20, and 16 ms or more none, at 32, 44.1 and 48 kHz alike.  Through the hub chain the slower control writes before the trigger hid part of that window.  Sending the Windows session stop `0x13 0xC000` at the stop does not help.  The 1 s cold init always covered this window.  `babyface_stream_start()` now waits until `BF_SESSION_GAP_MS` (50 ms) have passed since the last stop; only a session restarted at once pays it.

## 2026-09-27 — the unit goes standalone when the PC suspends (firmware, not the driver)

Putting the PC into S3 leaves the Babyface Pro showing its standalone state.  Like the rest of RME's units it runs standalone — its own stored routing and clock — whenever no computer is actively driving it, and a suspended host is exactly that.  Nothing the driver sends changes this: the standalone/interface switch is firmware, and there is no "stay in interface mode" request to send to a host that is asleep for the duration.

Whether the unit stays powered (and so shows standalone) or goes dark depends on the USB port: one that keeps its 5 V through S3 leaves the unit on with no host.  On this box the USB link survives the suspend too — dmesg shows `PM: suspend entry (deep)` / `PM: suspend exit` with no `usb 3-1: ... disconnect` and no re-enumeration — so the driver's `suspend()`/`resume()` callbacks run, not the disconnect + re-probe pair, and the host-side state is re-applied on resume (validated 2026-08-25, item 12; the flags/EQ re-apply later folded into `resume()` is not separately re-measured).

The unit's standalone routing and levels come from its own stored configuration, which the driver does not manage, so they need not match the Linux session's — set them from TotalMix (Windows/macOS) if the standalone behaviour matters.

## Protocol knowledge → kernel equivalents (from the RE)

| Protocol | Kernel equivalent |
|---|---|
| Vendor requests (0x12/0x17/0x1A/0x1B/…: crosspoints, masters, preamp, gains, DDS pitch) | `snd_kcontrol` set |
| 14×32-bit frame stream, 24-bit in bytes 1-3, interrupt ep 0x01/0x82 | PCM + interrupt URBs (caiaq-style) |
| Stream init/trigger/arm (`streaming_init`, `0x10 0x8000`+`0x1D`, `0x14 0xC000`; never `0x13` mid-run) | probe init + trigger-time stream start (workqueue) |
| 48V/PAD state (0x17 wIdx 0x003F + 0x21 commit) | boolean controls (works with no stream — verified) |
| Sample rate = SET_INTERFACE(5, alt) | hw_params → set_interface + stream restart |
| Front panel (0x17 readback, host-driven) | read-only ALSA controls — DONE 2026-08-26 (babyfacepro-ctl.c); the translation to mixer writes is TuxMix user-space |
| Loopback/MS-proc/AN1>2/width/split flags | boolean/route controls — TBD |
| EQ = bulk OUT ep 0x0A coefficient uploads | BYTES controls + bulk URBs — TBD |
| Reverb/echo = host-side | out of scope for the kernel (TuxMix user-space) |

## Build / test (hobby box)

```sh
cd tools/kernel
make LLVM=1 -C /lib/modules/$(uname -r)/build M=$(pwd) modules   # CachyOS = clang
sudo insmod snd-usb-babyface-pro.ko
# reload after a change (interface 5 is bound):
echo "3-1:1.5" | sudo tee /sys/bus/usb/drivers/snd-usb-babyface-pro/unbind
sudo rmmod snd_usb_babyface_pro && sudo insmod snd-usb-babyface-pro.ko
aplay -D hw:3,0 -f S32_LE -c 2 -r 48000 /tmp/tone.raw
arecord -t raw -D hw:3,0 -f S32_LE -c 2 -r 48000 -d 3 /tmp/cap.raw
```

The user-space TuxMix stays the reference/validation suite forever
(everything was validated on real hardware — the kernel driver must
reproduce the same writes, verifiable against PROTOCOL.md).
