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
| PCM playback + capture (2 ch S24_LE, 9 rates 32-192 kHz) | ✅ |
| Rate switch = SET_INTERFACE(5, alt) (3 bandwidth classes) | ✅ 32/44.1/48/64/88.2 = alt1, 96/128 = alt2, 176.4/192 = alt3 — measured exact (48/96/192 kHz frame rates) |
| Controls: 6 output masters (0-0x4000 raw, 0 dB = 0x2000) + mutes | ✅ per-output names ("AN1/2" / "PH3/4" / … Playback Volume+Switch) — the 8-bit register is the REAL volume (0.5 dB/step, 0xF3 = 0 dB) |
| System volume = PipeWire SOFTWARE volume | ✅ DONE 2026-08-26 (cap_sysvol2.pcap: the Windows volume is a host-side stream gain, zero USB writes): the masters are named per output so SPA finds no "Master" element → the sink falls back to software volume; `wpctl set-volume` no longer moves any hardware register (verified). See “System-volume model — CORRECTED AGAIN” |
| Controls: 4 preamp gains — TWO laws | ✅ AN1/2 mic: raw 0-20 = 0-65 dB (3.25 dB/step, cap_calib); AN3/4 instrument: 0-9 dB control, raw = dB×2 = 0-18 (0.5 dB/step, cap_gain34 — `bf_gain_max_db`) |
| Controls: 2× phantom + 2× PAD (0x17 0x003F + 0x21 commit) | ✅ (LEDs + relay clicks) |
| Controls: 84 crosspoints (6 out × 14 src) — output order corrected | ✅ (Phones = block 0) |
| Controls: pitch, loopback ×6, AN1>2, link, width, FX send, MS | ✅ |
| Loopback record staging | ✅ NO STAGING NEEDED (2026-08-26): the record is 1:1 with the master on Windows (aligned captures) AND Linux (live, S16 tone: −20 dBFS → −20.0).  The “fixed 2^-5 tap”/×32 were artifacts of mktone.py's broken 4-byte tone (chain ran −48 dB low → record quantizer → coarse square).  ×32 REVERTED; mktone.py → S16 |
| Front-panel readback (0x17 poll at 50 Hz → read-only button/wheel/IN/OUT/MIX/DIM ALSA controls, panel.c) | ✅ hardware-validated 2026-08-26 (all six flash codes + wheel; consume-on-get bug fixed — controls hold the last state) |
| Multi-channel PCM (2-12 ch, full 14-word frame) | ✅ (12-ch capture + playback verified; marker words skipped) |
| Default mixer state at probe (TotalMix-style: all sources → all outputs at unity, masters 0 dB) | ✅ |
| DSP EQ (eq.c): 4 strips × 3-band bell/shelf + low cut, 64-byte bulk coeff blocks on ep 0x0A | ✅ HARDWARE-VALIDATED 2026-08-27 on the mic (bell ±6 dB @ 200 Hz, +6 dB @ 3 kHz, low cut 100/300 Hz on/off; `eq_selftest` ~1 LSB vs the captures). Fixed-point Q27 (CORDIC + exp2, no FPU). NOTE: the loopback taps the record bus POST-EQ, so the input EQ is not measurable on the loopback chain (ear-validated instead) |
| Preamp state sync from 0x17 readback at probe | ✅ |
| Mixer-state persistence across interface re-probes (usbfs claim → detach → re-probe restores 48V/gains/crosspoints/pitch/flags) | ✅ 2026-08-24 |
| PM: suspend/resume with full cached-state restore (cold init + mixer re-apply) | ✅ |
| checkpatch | ✅ 0 errors / 0 warnings |

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
2. **Multi-channel PCM**: expose the full 14-channel frame (PB1-6
   playback, AN1-4 + ADAT/SPDIF capture) instead of 2 ch.
3. **Full control set**: crosspoints (the TotalMix matrix ~hundreds of
   controls), EQ bulk uploads (0x0A) — DONE, pitch/varispeed (0x1B DDS
   quads), loopback/MS/AN1>2/width/split flags, ref levels, clock
   source keepalive (0x10 0x05CF).
4. **Front panel** — DONE (2026-08-26, `panel.c`): the 0x17 readback is
   polled at 50 Hz in a delayed_work and mirrored into read-only ALSA
   controls (Front Panel Button/Wheel/In/Out/Mix/Dim).  What remains:
   TuxMix user-space translating those controls into mixer writes (the
   host is in the loop, like TotalMix).
5. **PM hardening**: full suspend/resume with mixer-state restore
   (device has no readback for faders — mirror TotalMix's re-apply).
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
   + `pcmxrun.c`)**: full-duplex sweep (9 rates × periods ≥
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

1. **Stream start = full cold-init + state restore.**  The firmware only
   validates a session preceded by the complete cold-init; without it the
   outputs stay silent.  The init wipes the mixer registers, so the
   cached state (preamp, gains, masters, crosspoints, pitch) is re-applied
   right after the arm — the session starts at the user's levels and the
   output is not muted at stream start.
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

## Protocol knowledge → kernel equivalents (from the RE)

| Protocol | Kernel equivalent |
|---|---|
| Vendor requests (0x12/0x17/0x1A/0x1B/…: crosspoints, masters, preamp, gains, DDS pitch) | `snd_kcontrol` set |
| 14×32-bit frame stream, 24-bit in bytes 1-3, interrupt ep 0x01/0x82 | PCM + interrupt URBs (caiaq-style) |
| Stream init/trigger/arm (`streaming_init`, `0x10 0x8000`+`0x1D`, `0x14 0xC000`; never `0x13` mid-run) | probe init + trigger-time stream start (workqueue) |
| 48V/PAD state (0x17 wIdx 0x003F + 0x21 commit) | boolean controls (works with no stream — verified) |
| Sample rate = SET_INTERFACE(5, alt) | hw_params → set_interface + stream restart |
| Front panel (0x17 readback, host-driven) | read-only ALSA controls — DONE 2026-08-26 (panel.c); the translation to mixer writes is TuxMix user-space |
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
aplay -D hw:3,0 -f S24_LE -c 2 -r 48000 /tmp/tone.raw
arecord -t raw -D hw:3,0 -f S24_LE -c 2 -r 48000 -d 3 /tmp/cap.raw
```

The user-space TuxMix stays the reference/validation suite forever
(everything was validated on real hardware — the kernel driver must
reproduce the same writes, verifiable against PROTOCOL.md).
