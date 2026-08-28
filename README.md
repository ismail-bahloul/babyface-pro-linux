# snd-usb-babyface-pro: Linux driver for the RME Babyface Pro FS (proprietary mode)

A from-scratch Linux kernel driver for the **RME Babyface Pro FS**
audio interface running in its **proprietary USB mode**
(`VID:PID 2a39:3fc0`), reverse-engineered from Windows captures and
validated on real hardware.  The goal is a production-quality driver
worth proposing to the Linux kernel (`linux-usb` / ALSA).

## Why proprietary mode?

The device presents two USB personalities:

- a **class-compliant** one handled by the stock `snd-usb-audio`, and
- the **proprietary** one (`2a39:3fc0`) whose PCM stream runs on
  *interrupt* endpoints (interface 5) and whose mixer/DSP is a
  vendor-control surface (the TotalMix-class control set).

The proprietary mode is the interesting one: it exposes the full
channel count and the hardware DSP mixer, and with this driver it
reaches a streaming latency floor of **0.33 ms** (16-frame URBs @ 48
kHz, 0 xruns) that Windows cannot match (its floor is 46 samples).

## Status

**Hardware-validated** on the Babyface Pro FS (2026-08):

- **Streaming**: 32–192 kHz, 2–12 channels, interrupt-URB, full-duplex;
  period floor 16 frames (0.33 ms), zero xruns across the sweep
- **Mixer** (ALSA controls): 6 output masters + mutes, the full
  6×14 crosspoint matrix, 4 preamp gains (two laws: mic 3.25 dB/step,
  instrument 0.5 dB/step), phantom power + PAD, pitch/varispeed,
  loopback (30-ch map), width, FX send, MS processing, input link,
  AN 1>2
- **Front panel fully emulated** (the host is "in the loop" like
  TotalMix): IN/OUT/MIX/SELECT/DIM buttons, the wheel in gain /
  volume / MIX-monitoring / balance modes, the MIX-mode VU display,
  SET = phantom toggle
- **PM**: suspend/resume with mixer-state restore
- **Automated checks**: `regress.sh` passes 40/40 on hardware (rate ×
  period sweep with 0 xruns, start/stop stress, mixer-restore across
  unbind/rebind, disconnect mid-stream); `selftests.sh` runs the law
  selftests, the build, and checkpatch without the card

## Build & load

```sh
cd tools/kernel
make LLVM=1 -C /lib/modules/$(uname -r)/build M=$PWD modules
sudo insmod snd-usb-babyface-pro.ko
# or: frames_per_urb=16 nurbs=16  → the low-latency profile
cat /proc/asound/cards          # card "BabyfaceProFS"
```

The card then works like any ALSA/PipeWire device; the mixer is the
normal ALSA control set (`amixer -c <n> controls`).

## Tests

```sh
sh tools/kernel/selftests.sh                 # laws + build + checkpatch (no card)
sh tools/kernel/regress.sh --dur 1 --mixer-restore --disconnect-test   # needs the card
```

## The reverse engineering

The protocol was reverse-engineered from USBPcap captures of the
Windows driver + TotalMix FX, then validated bit-by-bit on hardware.
The full reference is in this repo:

- **`tools/usbdump/PROTOCOL.md`**: every vendor request, the register
  maps, the front-panel protocol, the stream layout
- **`tools/usbdump/CALIBRATION.md`**: the calibrated laws (fader
  curve, master curve, gain laws, EQ biquad)
- **`tools/usbdump/`**: the capture-analysis tools (the captures
  themselves are not in the repo)
- **`KERNEL-DRIVER.md`**: the driver architecture + known gaps
- **`LINUX-VALIDATION.md`**: the hardware validation log

## Upstream plan

1. Core driver (stream + mixer + front panel + DSP EQ) → RFC on
   linux-usb / alsa-devel
2. Follow-ups: clock source, input trim, ref-level, phase, stereo
   split, the EQ HF-warping / shared-c4 details
3. The user-space mixer application lives in the sibling repo
   **[TuxMix](https://github.com/ismail-bahloul/TuxMix)** (control
   stack + GUI/TUI).

## License

GPL-2.0-only (kernel driver).
