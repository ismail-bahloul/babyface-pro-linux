# Install

Three ways to get `snd-usb-babyface-pro` running. The card then works like
any other ALSA / PipeWire device, and its mixer is the normal ALSA control
set: `amixer -c <n> controls`.

## Arch / CachyOS (AUR, DKMS)

```sh
paru -S snd-usb-babyface-pro-dkms
```

DKMS rebuilds the module on every kernel upgrade. To build the packaging
from this tree instead:

```sh
cd aur/snd-usb-babyface-pro-dkms && makepkg -si
```

## Any distribution with DKMS

`dkms.conf` ships in `tools/kernel/`:

```sh
sudo cp -r tools/kernel /usr/src/snd-usb-babyface-pro-0.2.0
sudo dkms add     -m snd-usb-babyface-pro -v 0.2.0
sudo dkms install -m snd-usb-babyface-pro -v 0.2.0
sudo modprobe snd-usb-babyface-pro
```

## Manual build

For a one-off test without DKMS, see the build, load and hardware-check
walkthrough in [`LINUX-TEST.md`](../LINUX-TEST.md).

## Next

- Tune the latency: [Latency profiles](latency.md).
- Control the mixer from a GUI:
  [TuxMix](https://github.com/ismail-bahloul/TuxMix).
