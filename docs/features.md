# Features and status

**Hardware-validated** on a real Babyface Pro FS, and reported to run
unmodified on an original (non-FS) Babyface Pro.

## Streaming

32 to 192 kHz, 2 to 12 channels, interrupt-URB, full-duplex. 32-frame URBs
(0.67 ms at 48 kHz) run clean across the sweep, with one open issue, see
[Latency](latency.md). 16-frame URBs (0.33 ms, monitoring-grade) are
playback-solid but drop the occasional capture buffer.

## Mixer (ALSA controls)

- 6 output masters and mutes
- the full 6x14 crosspoint matrix
- 4 preamp gains, phantom power and PAD
- pitch/varispeed, loopback, width per stereo source, FX send, MS processing,
  input link, AN 1>2
- clock source, optical out format (ADAT/S/PDIF), reference level, phase and
  trim

## Front panel

The host is "in the loop", like TotalMix. Every button, the wheel and the
IN/OUT/MIX selection are decoded and exposed as read-only controls. The
driver acts on SET (phantom), the wheel and MIX-mode, including the VU
display.

DIM presses are reported through `DIM Button Press Count` for a mixer
application to act on ([TuxMix](https://github.com/ismail-bahloul/TuxMix)
does); the `Dim Switch` control takes 20 dB off the Phones level, as TotalMix
does with its default Main Out. `Dim Level` sets how much (0 to 65 dB, 20 by
default), and `Main Mono Switch` sums the stereo sources into the Main pair,
like the mono button on TotalMix's control room.

Every source has its own `FX Send` and every output pair an `FX Return`,
TotalMix's "send FX" and "return FX" knobs. The unit's reverb and echo
parameters are not sent over USB by TotalMix, so only the levels are here.

## Power management

Suspend/resume with full mixer-state restore.

## Automated checks

`regress.sh` passes 41/41 on hardware; `selftests.sh` runs the laws, the
build and checkpatch without the card.

## Why a dedicated driver?

The device presents two USB personalities:

- a **class-compliant** one, handled by the stock `snd-usb-audio`;
- the **proprietary** one (`2a39:3fc0`), whose PCM stream runs on interrupt
  endpoints and whose mixer is a vendor-control surface.

The proprietary mode gives the full channel count and the hardware DSP mixer
at the lowest latency, and this is the only reverse-engineered implementation
of it on Linux. The rest of the ecosystem stays on the more limited
class-compliant surface ([oscmix](https://github.com/huddx01/oscmix),
[rme-control-cli](https://github.com/stistrup/rme-control-cli)).

The protocol was decoded from Windows USB captures and verified bit by bit on
hardware; the reference is in [`tools/usbdump/PROTOCOL.md`](../tools/usbdump/PROTOCOL.md).
