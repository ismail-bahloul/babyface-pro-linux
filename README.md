<div align="center">

# snd-usb-babyface-pro

**A Linux ALSA driver for the RME Babyface Pro FS, running in its proprietary USB mode.**

<code>VID:PID 2a39:3fc0</code> · reverse-engineered from Windows captures · validated on real hardware

**Hardware-validated** **&nbsp;·&nbsp;** **checkpatch / sparse / W=1 clean** **&nbsp;·&nbsp;** **40/40 regression suite** **&nbsp;·&nbsp;** **0.33 ms latency floor**

</div>

---

## What it is

A from-scratch kernel driver that brings the **full channel count** and the **hardware DSP mixer** of the Babyface Pro FS to Linux — in its proprietary USB mode, which a stock `snd-usb-audio` cannot touch. Modeled on `snd-usb-caiaq`, the in-tree precedent for interrupt-based USB audio.

The proprietary mode runs the PCM stream on interrupt endpoints (interface 5) and exposes the TotalMix-class control surface as a normal ALSA mixer, reaching a **0.33 ms** streaming floor (16-frame URBs @ 48 kHz) that Windows cannot match.

> **Related project:** the companion user-space mixer app lives in the sibling repo **[TuxMix](https://github.com/ismail-bahloul/TuxMix)** (control stack + GUI/TUI).

## Get started

The card works like any other ALSA / PipeWire device once loaded.

```sh
# DKMS (recommended — survives kernel upgrades). Arch/CachyOS:
cd aur/snd-usb-babyface-pro-dkms && makepkg -si
# ...or any distro with dkms installed (dkms.conf ships in tools/kernel/):
sudo cp -r tools/kernel /usr/src/snd-usb-babyface-pro-0.1.0
sudo dkms add    -m snd-usb-babyface-pro -v 0.1.0
sudo dkms install -m snd-usb-babyface-pro -v 0.1.0
sudo modprobe snd-usb-babyface-pro
```

Then the mixer is the normal ALSA control set: `amixer -c <n> controls`.

> **Low-latency profile:** load with `frames_per_urb=16 nurbs=16` for the 0.33 ms monitoring floor (the default is the TotalMix-parity 256 samples). To switch profiles you currently reload the module.
> **Full build / load / test walkthrough** (manual build, hardware checks, front-panel probes) → **[`LINUX-TEST.md`](LINUX-TEST.md)**

## Status & features

**Hardware-validated** on a real Babyface Pro FS:

- **Streaming** — 32–192 kHz, 2–12 channels, interrupt-URB, full-duplex; period floor 16 frames (0.33 ms), zero xruns across the sweep.
- **Mixer (ALSA controls)** — 6 output masters + mutes, the full 6×14 crosspoint matrix, 4 preamp gains, phantom power + PAD, pitch/varispeed, loopback, width, FX send, MS processing, input link, AN 1>2, plus clock source, ref level, phase and trim.
- **Front panel fully emulated** (the host is "in the loop", like TotalMix) — buttons, wheel, MIX-mode VU display.
- **PM** — suspend/resume with full mixer-state restore.
- **Automated checks** — `regress.sh` passes 40/40 on hardware; `selftests.sh` runs laws, build and checkpatch without the card.

> The protocol was decoded from Windows USB captures and verified bit-by-bit on hardware. The full reference, the calibrated laws and the engineering history live in the docs below.

## Documentation

Everything lives in dedicated files; this README only links to them.

| If you want to… | Go to |
|---|---|
| **Build, load & test** the driver on real hardware | [`LINUX-TEST.md`](LINUX-TEST.md) |
| Understand the **driver architecture & current gaps** | [`KERNEL-DRIVER.md`](KERNEL-DRIVER.md) |
| See the **hardware validation log** (what was verified, and when) | [`LINUX-VALIDATION.md`](LINUX-VALIDATION.md) |
| Read the **protocol reference** (register maps, requests, stream layout) | [`tools/usbdump/PROTOCOL.md`](tools/usbdump/PROTOCOL.md) |
| Read the **calibrated laws** (fader/master curves, gains, EQ) | [`tools/usbdump/CALIBRATION.md`](tools/usbdump/CALIBRATION.md) |
| Follow the **upstream submission** (RFC series, review prep) | [`tools/kernel/UPSTREAM.md`](tools/kernel/UPSTREAM.md) · [`patches/COVER-LETTER.md`](patches/COVER-LETTER.md) |
| Dive into the **RE tooling** (USBPcap capture analysis) | [`tools/usbdump/README.md`](tools/usbdump/README.md) |

<details>
<summary><b>Optional reading</b> — review & reverse-engineering notes</summary>

- External AI review of the driver & how each finding was resolved: [`docs/REVIEW-FINDINGS.md`](docs/REVIEW-FINDINGS.md)
- Draft reply to the maintainer's v2 review: [`docs/REPLY-TO-TAKASHI-V2.md`](docs/REPLY-TO-TAKASHI-V2.md)
- Original Windows capture plan (RE background): [`tools/usbdump/WINDOWS-CAPTURE-PLAN.md`](tools/usbdump/WINDOWS-CAPTURE-PLAN.md)

</details>

## Why a dedicated driver?

The device presents two USB personalities:

- a **class-compliant** one, handled by the stock `snd-usb-audio`, and
- the **proprietary** one (`2a39:3fc0`) whose PCM stream runs on interrupt endpoints and whose mixer is a vendor-control surface.

The proprietary mode is the interesting one — full channel count + hardware DSP mixer at the lowest latency — and the only reverse-engineered implementation of it on Linux. The rest of the ecosystem stays on the more limited class-compliant surface ([oscmix](https://github.com/huddx01/oscmix), [rme-control-cli](https://github.com/stistrup/rme-control-cli)).

## License

GPL-2.0-only (kernel driver).
