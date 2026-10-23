# Changelog

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
