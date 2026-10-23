# Latency profiles

The driver ships the TotalMix-parity **256 samples (about 5.3 ms)**, so a
fresh install is stable everywhere. The low-latency profiles are off by
default. Measured at 48 kHz, full-duplex:

| `frames_per_urb` | `nurbs` | period | result |
|---|---|---|---|
| 256 *(default)* | 8 | 5.3 ms | 100 % stable |
| 32 | 8 | 0.67 ms | clean in the regression sweeps, see the open issue below |
| 16 | 16 | 0.33 ms | playback rock-solid, but capture drops about 1 sample every 7 s |

Latency is quantised to the URB boundary, so it does not depend on the
period size your DAW asks for.

## Switching profile

The DKMS package installs a commented template at
`/usr/lib/modprobe.d/snd-usb-babyface-pro.conf`. Uncomment a line there (or
override it in `/etc/modprobe.d/`) and reload the module:

```sh
sudo modprobe -r snd-usb-babyface-pro && sudo modprobe snd-usb-babyface-pro
```

## Known issues

**32/8 has one open issue.** Once, the unit stopped exchanging audio
altogether (it still answered control requests) and only came back after
being unplugged: a module reload, a USB re-enumeration and a host reboot did
not help. Once, a single 192 kHz session came up silent and the next one was
fine. Neither has been reproduced since, and neither has been seen at the
default 256. Use 32/8 if you want the latency and can live with that; if the
card goes silent, unplug and replug it.

**16/16 is asymmetric**: solid for playback, lossy for *recording*. Use it
for monitoring, not for capture.
