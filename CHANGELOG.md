# Changelog

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
