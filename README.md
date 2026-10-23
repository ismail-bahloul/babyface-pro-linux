<div align="center">

# snd-usb-babyface-pro

**Full channel count and the hardware DSP mixer of the RME Babyface Pro FS, native on Linux.**

[![AUR](https://img.shields.io/aur/version/snd-usb-babyface-pro-dkms?style=flat-square&logo=archlinux&logoColor=white&label=AUR&color=1793d1)](https://aur.archlinux.org/packages/snd-usb-babyface-pro-dkms) ![Kernel](https://img.shields.io/badge/Linux-ALSA%20kernel%20driver-333?style=flat-square&logo=linux&logoColor=white) [![License](https://img.shields.io/badge/license-GPL--2.0--only-555?style=flat-square)](LICENSE)

<br>

| 0.67 ms | 41 / 41 | 12 ch · 192 kHz |
|:---:|:---:|:---:|
| clean latency floor | hardware regression suite | full-duplex streaming |

</div>

<br>

A from-scratch kernel driver for the Babyface Pro FS in its **proprietary USB
mode**, which the stock `snd-usb-audio` cannot touch. The PCM stream runs on
interrupt endpoints and the TotalMix-class control surface shows up as a
normal ALSA mixer. Reverse-engineered from Windows captures, validated on real
hardware, and the only implementation of this mode on Linux.

- **Every channel**: 2 to 12 channels, 32 to 192 kHz, full-duplex
- **The whole DSP mixer** as ALSA controls: masters, 6x14 crosspoints, preamps, phantom, EQ
- **Front panel** decoded: buttons, wheel, DIM, with suspend/resume restore
- **Low latency** down to 0.33 ms, opt-in

## Install

```sh
paru -S snd-usb-babyface-pro-dkms     # Arch, CachyOS
```

Other distributions, and the manual build: [install guide](docs/install.md).

## A mixer to go with it

The driver exposes the controls; **[TuxMix](https://github.com/ismail-bahloul/TuxMix)**
is the GUI that drives them, TotalMix-style.

<div align="center">

<a href="https://github.com/ismail-bahloul/TuxMix"><img src="https://raw.githubusercontent.com/ismail-bahloul/TuxMix/main/docs/screenshots/mixer.png" width="560" alt="TuxMix, the companion mixer"></a>

</div>

## Learn more

<div align="center">

[Install](docs/install.md) · [Latency](docs/latency.md) · [Features](docs/features.md) · [All docs](docs/README.md) · [Upstream](tools/kernel/UPSTREAM.md)

</div>

## License

GPL-2.0-only, as a kernel driver should be.
