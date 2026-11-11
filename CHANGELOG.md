# Changelog

## Unreleased

### Fixed
- The stream URBs and their DMA buffers are freed at disconnect, not when
  the last application closes the card, after the host controller that
  allocated them may be gone.
- A stream that failed to start no longer leaves the other direction
  running with no URBs; it is stopped with an xrun.
- At 64 kHz and above, a period shorter than the device's packet (with
  `urb_frames_min` lowered) is no longer offered and then refused in
  hw_params.
- Disconnect no longer turns USB autosuspend on for the whole device,
  MIDI interface included.
- Each unit takes its own `index`/`id` module parameter entry.
- `Dim Switch` takes 20 dB off the Phones level instead of setting it to
  -20 dB, which made a quieter level louder. This is what TotalMix writes,
  checked against TotalMix FX on the unit.

### Added
- Probe checks that the streaming altsettings have the expected interrupt
  endpoints before using them.

## 0.2.3 (2026-11-05)

### Fixed
- Every probe and resume wrote to the AN1 and AN2 crosspoints, even at width
  0, which opened those inputs after the default mixer had left them muted.
  The cache still said 0, so a later write of 0 could not mute them again.
- Joining a split playback pair came back 6 dB low and lost the level set on
  its AN1/2 crosspoint. It now restores that level (#31).
- DIM, mute, the masters and the front-panel wheels no longer cancel each
  other on the Phones level, and the panel's master writes respect DIM.
- The front panel's MIX wheel lost clicks below the curve's first point; a
  muted input now comes up on the first click.
- Boost fell back to -10 dBV after AN 1>2, an input link or a state replay.
- An EQ band of type Off is now identity rather than a high shelf, and a
  failed strip upload is reported.
- A failed URB or DMA allocation during probe returned success after freeing
  the card. It now returns -ENOMEM.
- A substream prepared before the other direction changed the session can no
  longer start against stale rate and period settings.

### Added
- `tools/kernel/reload.sh` reloads the module and, when it cannot, names the
  processes holding the card. `-a` stops PipeWire and WirePlumber around it.

### Changed
- The driver uses `guard()` and `scoped_guard()` for all its locking, and
  `bf_seen_before()` keeps a fixed table of recent units. Neither changes
  behaviour.
- The series under `patches/` is v8, which answers the review of v7.

## 0.2.2 (2026-10-27)

### Fixed
- The front panel's OUT wheel moved ADAT7/8 when `Opt` was selected. It
  moves AS1/2, the first optical pair, which is the same mixer output in
  ADAT and S/PDIF mode, so the target no longer depends on the Optical
  Out Format setting (#12).

### Changed
- The series under `patches/` is the one submitted upstream on
  2026-10-27, `[PATCH v7 0/8]`, rebased on next-20261026.

## 0.2.1 (2026-10-27)

### Fixed
- The crosspoint map had AN1/2 and PH3/4 swapped and shifted every pair
  after them, so a mix into one output landed in the next pair's submix.
  It is now `0x34 * pair + 0x1A * side + source`, checked on an FS with a
  per-pair loopback matrix (#36, from the captures in #31).
- The EQ's alpha is `sin(w0 / (2Q))`, as TotalMix computes it, which
  removes the high-frequency warping the notes still listed as open (#35,
  #31).

### Changed
- From a 128-frame period the driver keeps one and a half periods of URBs
  in flight, splitting the period in two. Through PipeWire the round trip
  drops from 554 to 490 frames at 128 and from 1066 to 938 at 256 (#37).
- Documentation: the latency page explains the settings and the measured
  round trip, `tools/delay-sweep.sh` measures it, and the places where the
  captures in #31 disagree with ours are marked disputed (#32, #33, #34).

## 0.2.0 (2026-10-23)

### Added
- The PCM core now sees playback underruns, so a late write is reported
  instead of silently swallowed (thanks to davfre, #25). `tools/pcmlate`
  checks it.
- Capture is exposed in the device's own channel order; `tools/` reads the
  AS1/2 loopback on capture channels 4/5.
- The low-latency modprobe template follows the period-following URBs and
  the latest soak results.
- The card defaults to 256 frames through PipeWire and follows the PipeWire
  quantum through the updated WirePlumber rule.

### Changed
- `pcmxrun` primes playback before restarting after an xrun.
- Documentation: shorter README, with install, latency and features moved to
  `docs/`; real-time low-period soak results (32/24/16 frames clean, xruns
  are system events).
- Upstream: the v7 series is cut on next-20261002.

## 0.1.1 (2026-10-18)

Initial tagged release, see the git history.
