# Hardware test — snd-usb-babyface-pro on the Babyface Pro FS

How to build, load and validate the driver on a real Babyface Pro FS
running in its **proprietary mode** (`VID:PID 2a39:3fc0`).  Run this
from a real Linux install with the device plugged in (not WSL2 — the
interrupt-endpoint audio transfers need a real host stack).

## 1. Check the device is in proprietary mode

```bash
lsusb | grep -i 2a39
```

Expected: `2a39:3fc0`.  A different PID means the device enumerated in
class-compliant mode — switch it back to proprietary (the RME
Fireface USB Settings panel, or the front-panel mode toggle) before
going further.

## 2. Build the module

```bash
cd tools/kernel
make LLVM=1 -C /lib/modules/$(uname -r)/build M=$PWD modules
```

## 3. Load it

```bash
sudo insmod snd-usb-babyface-pro.ko              # default profile
# low-latency DAW profile (0.33 ms @ 48 kHz, monitoring-grade):
sudo insmod snd-usb-babyface-pro.ko frames_per_urb=16 nurbs=16
cat /proc/asound/cards        # expect a card named "BabyfaceProFS"
```

The card is a normal ALSA device: it appears in PipeWire, and its
mixer is the regular ALSA control set.

## 4. Stream check (play + record)

Generate a test tone, then play/record at the card number shown by
`/proc/asound/cards` (3 in the examples below):

```bash
python3 tools/kernel/mktone.py /tmp/tone.wav 440 -20 5
aplay -D hw:3,0 -f S24_LE -c 2 -r 48000 /tmp/tone.wav
arecord -D hw:3,0 -f S24_LE -c 2 -r 48000 /tmp/rec.wav
```

Note: the device only advances audio while **both** directions have a
pending URB (IN+OUT are submitted as a pair), so a lone `aplay`
without a matching `arecord` may not produce sound — keep both running
for a full-duplex test.

## 5. Mixer check

```bash
amixer -c 3 controls          # the 124-control set
amixer -c 3 sget 'AN1/2 Playback Volume'
```

Known hardware quirk: the firmware has **no mixer readback** — the
control values live in the driver's cache and are restored on
unbind/rebind and on S3 suspend/resume.

## 6. Front-panel probes (optional)

`usbwrite.c` is a raw usbfs vendor-request tool (build it with
`gcc usbwrite.c -o usbwrite`).  `selhold_probe.sh` /
`selhold_probe2.sh` / `mix_display_probe.sh` drive the front-panel
readback (they build `usbwrite` automatically if missing);
`panel_monitor.py` / `mix_test.py` watch the panel ALSA controls live.

## 7. Automated validation

```bash
sh tools/kernel/selftests.sh   # law selftests + build + checkpatch — no card needed
sh tools/kernel/regress.sh --dur 1 --mixer-restore --disconnect-test   # needs the card
```

`regress.sh` runs the rate × period sweep (expect 0 xruns), the
start/stop stress, the mixer-restore across unbind/rebind, and a
mid-stream disconnect — 40/40 on the reference unit.

## 8. Reporting problems

Include: `lsusb | grep -i 2a39`, `cat /proc/asound/cards`, the
`dmesg` tail around the failing step, and which `regress.sh` step
failed.
