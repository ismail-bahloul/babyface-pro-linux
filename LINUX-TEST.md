# Linux hardware test — Babyface Pro FS proprietary protocol probe

Quick recipe to validate the reverse-engineered protocol on the real
device. Run this from a **real Linux install** (not WSL2 — the device's
USB interrupt audio transfers need a real host stack; usbipd detaches
the device from the Windows driver, breaking TotalMix while attached).

## 1. Get the repo on Linux

The repo lives on the Windows `D:\` (NTFS). Building on an NTFS mount
is very slow, so copy it to the native filesystem first — **excluding
the 26 GB of Windows build artifacts** (`target/`) and `.git`:

```bash
mkdir -p ~/tuxmix
rsync -a --exclude target --exclude .git /run/media/$USER/TuxMix/ ~/tuxmix/
# (skip the copy if the repo is already on a Linux partition)
```

## 2. Install Rust (if missing)

```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
source ~/.cargo/env
```

## 3. Build the probe

```bash
cd ~/tuxmix
cargo build -p tuxmix-usb --features driver --example probe
```

This compiles libusb from source (vendored), so a C toolchain
(`gcc`/`build-essential`) must be present.

## 4. Verify the device is in proprietary mode

Plug the Babyface Pro FS, then:

```bash
lsusb | grep -i 2a39
```

Expected: `2a39:3fc0`. If the PID differs, the device enumerated in
class-compliant mode — stop and report the actual VID/PID.

## 5. Run the test

USB access may need root (unless your user is in the right group for
`/dev/bus/usb`):

```bash
sudo target/debug/examples/probe 48v on       # 48V phantom Mic 1 (AN1)
sudo target/debug/examples/probe status       # read status registers
sudo target/debug/examples/probe gain 35      # Mic 1 gain (raw 17, dB/2 placeholder — absolute scale pending calib)
sudo target/debug/examples/probe master 2000  # AN1/2 output master at 0 dB
sudo target/debug/examples/probe vol 0317     # AN1 -> AN1/2 fader (~ -40 dB)
sudo target/debug/examples/probe 48v off      # cleanup
```

### Streaming / 48V test (the important one)

48V works with or without a stream (verified 2026-08-22 — the old
"48V needs a valid stream" theory was wrong; the earlier blocker was a
swapped 48V constant). The stream itself runs on **interface 5,
alt-setting 1, ep `0x01` OUT / `0x82` IN, as INTERRUPT transfers**
(bmAttributes 0x03 = interrupt per the USB spec — 1 = isochronous, 3 =
interrupt; earlier RE notes had this backwards). 14336-B URBs submitted
as a pair (the device only services the endpoints while both have a
pending URB). `start_streaming` reproduces the driver's init burst
(`streaming_init`) + state-restore + trigger before the URBs.

```bash
cargo build -p tuxmix-usb --features driver --example hold48v_iso
sudo target/debug/examples/hold48v_iso | tee hold48v.log
```

Expected: `out=`/`in=` stats with `completed_ok` climbing and
`completed_err: 0` (the IN stream carries the 14-channel frame
format), AND the front-panel 48V LED physically lights up. **Save the
output to `hold48v.log`** — a past retest was reported as failing but
its output was lost, so the result could not be analyzed.

If the transfers time out (all `completed_err`), check, in order:

1. **Both endpoints must have a pending URB** — a lone OUT URB never
   completes; the paired submit is essential (the current code does
   this internally).
2. `SET_INTERFACE(5, alt 1)` actually succeeds.
3. `snd-usb-audio` is not holding the device (unload / udev rule).
4. Sample rate: the `0x1B` clock values in `streaming_init` were
   captured at 48 kHz; if the device runs at another rate the init is
   wrong (capture the `0x1B` values at the target rate on Windows).

Diagnostic caveat: the "streaming" flag read from 0x17 (bit 1 of byte
0) is itself unverified — captures of TotalMix actively streaming show
byte 0 = 0x0C (bit 1 clear). The **48V LED is the ground truth** for a
valid session.

### 0x17 byte 2 states (2026-08-22, see PROTOCOL.md "Fresh state")

- Byte 2 bits 1-3 are a power-cycle counter (111 fresh → 000) and bit
  7 (0x80) appears only with active audio on Windows. **They do NOT
  gate 48V** — 48V works with or without a stream (the whole
  "session must be valid" investigation was chasing a swapped 48V
  constant; see HANDOFF.md).
- A USB reset does NOT power-cycle the device (DSP state persists);
  the counter only resets on a real power cycle.
- Correct 48V write: `0x17 0x000D 0x003F` = ON, `0x17 0x000C 0x003F` =
  OFF (both + `0x21` commit) — **verified on hardware 2026-08-22**: the
  P48 LED follows the toggle exactly, with or without the stream.

### VU meters (2026-08-22 — the stack computes levels now)

```bash
cargo build -p tuxmix-usb --features driver --example vu
sudo target/debug/examples/vu
```

Port of `tools/usbdump/vudemo.c`: starts the stream, routes AN1 →
AN1/2, enables 48V + gain on Mic 1, then draws terminal VU bars for
AN1-4 (peak + decaying hold) for 12 s. Speak into the mic on AN1 — the
bar should track your voice. The backend API is
`BabyfaceUsb::input_peaks()` (ch0-3 = AN1-4, 0..1 FS per poll).

## 6. What to verify

- `probe 48v on` then `probe status`: the 0x17 register should report
  `48V=on` (bit 0 = 0). Confirms the vendor-write path works.
- Speak into the mic (routed AN1 → AN1/2, listen on the speakers):
  the level should jump with `gain 35` and change with `master`/`vol`.
- `probe status` also shows the streaming bit (bit 1 of 0x17).

## 7. Report back

Paste the output of:

```bash
lsusb | grep -i 2a39
sudo target/debug/examples/probe status
```

plus whether the gain/fader/48V effects were audible/visible. That
calibrates the gain scale (the current `dB / 2` placeholder) and
confirms the whole protocol stack end to end.

## Test matrix

| Command | Expect | Confirms |
|---|---|---|
| `probe 48v on` + `status` | P48 LED **lights** (`48V=on`) | vendor writes + 0x17 readback + the corrected mapping (0x000D = ON) |
| `probe gain 35` | mic louder; IN-stream level rises (5-bit field; absolute dB pending calib) | gain registers + dB scale |
| `probe master 2000` | louder output | master fader registers |
| `probe vol 0317` | quieter AN1 → AN1/2 | crosspoint registers |

## Notes

- The probe only needs control transfers — no audio streaming is
  involved in this test.
- If `snd-usb-audio` claims the device, the driver auto-detaches it
  (Linux only); if the control transfers fail with a busy error, unload
  the ALSA driver for this device (`sudo rmmod snd_usb_audio` or a udev
  rule) and retry.
