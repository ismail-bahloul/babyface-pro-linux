# Latency

## In short

- **Ask your sound server for a small period.** The driver follows it: the
  latency you get is set by the period (PipeWire's *quantum*, a DAW's
  *buffer size*), not by a module option.
- Below **32 frames**, the module options make the smaller periods
  available. From 64 frames up, `frames_per_urb=32 out_urbs=2` shortens the
  round trip, by 6.7 ms at 256 frames. It is not the default yet: see
  [Shorter URBs at long periods](#shorter-urbs-at-long-periods).
- A fresh install runs the TotalMix-parity **256 frames (about 5.3 ms at
  48 kHz)** when an application asks for that, and is stable everywhere.

## The words

At 48 kHz, 48 frames are 1 ms.

| word | what it is |
|---|---|
| **frame** | one sample for every channel of the card |
| **period** | how many frames the application handles at a time: PipeWire's quantum, a DAW's buffer size. This is the knob you turn |
| **URB** | one USB transfer. The driver moves audio in URBs, and an URB never holds more than one period |
| `urb_frames_min` | the smallest URB, in frames (default **32**). It is also the **smallest period the card accepts**: ask for less and the application is given this much, so lower it to go lower |
| `frames_per_urb` | the largest URB, in frames (default **256**). A longer period is split into several URBs |
| `nurbs` | the capture URBs queued in flight, and the most playback URBs (default **8**). The capture queue rides out scheduling hiccups and costs no latency, since its URBs fill one after the other. Raise it together with a very small period |
| `out_urbs` | the most playback URBs in flight (default **0**: two periods' worth, at most `nurbs`). Each one is latency, because the next frame written waits behind them; fewer leave less slack against a late interrupt |

So with the defaults, any period from 32 frames (0.67 ms) up gets URBs of
its own size, and the options below are only needed to go lower. While
another application already holds the card, the period it negotiated is the
floor for the next one.

## Getting a lower latency

**1. Ask for a smaller period.** For example, with PipeWire:

```sh
# 64 frames for every application, until you put it back:
pw-metadata -n settings 0 clock.force-quantum 64
pw-metadata -n settings 0 clock.force-quantum 0
# or for one application:
PIPEWIRE_LATENCY=64/48000 your-application
```

In a DAW, set its buffer size. In TuxMix, *Device settings* has a *Buffer
size* menu that does the same. Note that a WirePlumber rule pinning
`api.alsa.period-size` keeps the card at that size whatever you ask.

**2. Only to go below 32 frames,** use the module options. Put them in
`/etc/modprobe.d/` (the file under `/usr/lib/modprobe.d/` is a commented
template that a package update may replace):

```
options snd-usb-babyface-pro urb_frames_min=8 nurbs=16
```

**3. Reload the module, and check that it took.** A sound server holds the
card, so `modprobe -r` fails with "in use" until it lets go. Stop it first, or
simply reboot or replug the card:

```sh
systemctl --user stop pipewire.socket pipewire.service wireplumber.service
sudo modprobe -r snd-usb-babyface-pro && sudo modprobe snd-usb-babyface-pro
systemctl --user start pipewire.socket pipewire.service wireplumber.service

cat /sys/module/snd_usb_babyface_pro/parameters/urb_frames_min
cat /sys/module/snd_usb_babyface_pro/parameters/nurbs
```

(Adjust the unit names if your system also runs `pipewire-pulse`.) The
`/sys` files show what the module is really running with.

## What was measured

Stability at small periods, at 48 kHz, full duplex, mmap, 12 playback
channels, a buffer of 2 periods, real-time priority, 10 minutes per row
(2026-10-21, see `KERNEL-DRIVER.md`):

| period | per period | module options | result |
|---|---|---|---|
| 256 | 5.3 ms | defaults | 100 % stable |
| 32 | 0.67 ms | defaults | clean, with rare system xruns that recovered |
| 24 | 0.50 ms | `urb_frames_min=8 nurbs=16` | 0 xruns |
| 16 | 0.33 ms | `urb_frames_min=8 nurbs=16` | 1 capture xrun |
| 8 | 0.17 ms | `urb_frames_min=8 nurbs=16` | loses capture buffers: avoid |

The few xruns at 16 to 32 frames do not grow as the period shrinks: they are
system latency events (CPU frequency, interrupts), so at these sizes tuning
the system matters more than the driver.

These rows measure stability, not latency. The round-trip figures in
`KERNEL-DRIVER.md` come from the earlier fixed-size URBs (2026-08-24), where
the latency did not follow the period; they no longer describe the driver.

### Round trip through PipeWire

Measured on 2026-10-27 with a JACK-API client under `pw-jack` (an impulse
on playback 1, found on the AS1/2 loopback, capture 5), the quantum forced
with `clock.force-quantum`, one 8-second run per row, each steady to the
frame. A user measured 17 frames more with Cable on a cabled output, which
is the converters:

| quantum | before 2026-10-27 | now |
|---|---|---|
| 32 | 170 | 170 |
| 64 | 234 or 298 | 298 |
| 128 | 554 | **490** |
| 256 | 1066 | **938** |

Through PipeWire the round trip is **4 periods + 42 frames**, one period
more than the 3 periods + 42 of the `read`/`write` test below, because the
graph processes a period after it was captured. The driver used to keep two
periods of URBs in flight; from a 128-frame period up it splits the period
in two URBs and keeps three, one and a half periods, which is half a period
less (64 frames at 128, 128 at 256). That was soaked for 10 minutes at 128
and 256 frames (48 kHz, 12 channels, mmap, real-time priority) with no
xrun. At other rates it only applies while a URB lasts at least 1.33 ms.
`urb_split=1` goes back to two periods, `urb_split=2` forces the split.

### Round-trip latency

Measured on 2026-10-26 with `tools/kernel/delay-sweep.sh` (`looplat
--delay`): a Babyface Pro FS, 48 kHz, the digital AS1/2 loopback tap, a
buffer of 2 periods, one period read and one written in lockstep, 8 seconds
per period, one run each. The impulse is written, then seen coming back:

| period | measured round trip | the delay the driver reports |
|---|---|---|
| 32 (0.67 ms) | 138 frames, 2.88 ms | 152 frames |
| 48 | 186 frames, 3.88 ms | 200 frames |
| 64 (1.33 ms) | 234 frames, 4.88 ms | 248 frames |
| 96 | 330 frames, 6.88 ms | 344 frames |
| 128 (2.67 ms) | 426 frames, 8.88 ms | 440 frames |
| 256 (5.33 ms) | 810 frames, 16.88 ms | 806 frames |

The round trip is **3 periods plus 42 frames (0.9 ms)** at every period, with
2 or with 12 channels. The delay the driver reports to ALSA follows it to
within 14 frames (0.3 ms), and that gap does not grow with the period, so a
sound server compensating with the reported delay is right to within that.

This path is digital and carries no converter delay, and it is read and
written with `read`/`write`, not mmap. A run with PH3/4 cabled into IN3 (run
the script with `DETECT=2`) would add the converters; there is no such
figure here yet.

## Shorter URBs at long periods

At a long period the URBs are long as well, and two queues follow from them.
A frame the application writes waits behind the playback URBs already in
flight, about one period's worth. A recorded frame waits until its capture
URB is full, up to one more period. Short URBs and a short playback queue
remove most of both:

```
options snd-usb-babyface-pro frames_per_urb=32 out_urbs=2
```

(`out_urbs=3` adds 0.67 ms and some slack; see the soaks below.)

Measured on 2026-11-10 on a Babyface Pro FS at 48 kHz. The test program
reads one period and then writes one, as a DAW does, with a buffer of 2
periods, real-time priority, and the AS1/2 digital loopback. The time runs
from the write of an impulse to the read that brings it back: the round trip
an application sees. There were 8 seconds and 14 impulses per row, with no
xrun:

| period | defaults | `frames_per_urb=32 out_urbs=2` | class-compliant mode |
|---|---|---|---|
| 32 (0.67 ms) | 3.6 ms | 3.6 ms (the same URBs) | 1.6 ms |
| 64 (1.33 ms) | 6.2 ms | 4.9 ms | not measured: an xrun |
| 128 (2.67 ms) | 10.2 ms | 7.6 ms | 5.6 ms |
| 256 (5.33 ms) | 19.6 ms | 12.9 ms | 11.3 ms |

The defaults column matches the PipeWire figures above. The class-compliant
mode runs the generic USB audio driver. It hands playback to the card as soon
as the application writes it, and records in URBs of at most 1 ms. About 2 ms
are left between it and the short URBs, at every period: one 32-frame URB
each way, and the card's own delay.

### Soaks

Ten minutes each (2026-11-10), 48 kHz, 12 playback channels, mmap,
real-time priority (`chrt -f 70`), `frames_per_urb=32`:

| period | URB | `out_urbs` | xruns (play / capture) | playback queue ran dry | latest completion |
|---|---|---|---|---|---|
| 64 | 32 | 2 | 0 / 0 | 0 | 625 us |
| 128 | 32 | 2 | 0 / 0 | **4** | 1692 us |
| 256 | 32 | 2 | 0 / 0 | 0 | 581 us |
| 64 | 32 | 3 | 0 / 0 | 0 | 399 us |
| 128 | 32 | 3 | 0 / 0 | 0 | 384 us |
| 128 | 32 | 3 (2nd run) | 0 / 0 | 0 | 540 us |
| 256 | 32 | 3 | 0 / 0 | 0 | 197 us |
| 64 | 16 | 3 | 0 / 0 | 0 | 291 us |
| 128 | 16 | 3 | 0 / 0 | 0 | 209 us |
| 64 | 16 | 2 | 0 / 0 | 0 | 298 us |
| 128 | 16 | 2 | 0 / 0 | 0 | 289 us |

The 16-frame rows ran with `frames_per_urb=16 urb_frames_min=16 nurbs=16`.

ALSA saw no xrun in any of them, and the loopback tone was intact. At 128
frames with two playback URBs, though, the card was left without data four
times: one event of about 1.7 ms was later than the 0.67 ms that two
32-frame URBs last. The card holds the whole stream while that happens, and
I do not know whether it can be heard. With three playback URBs (1.33 ms of
slack, 0.67 ms more latency) the same run had none. That is one run each, so
it does not show that three is enough, only that two is not always.

So `out_urbs=2` is the lowest latency, with the least slack against a late
interrupt, and `out_urbs=3` costs one URB for more of it. Neither is the
default: the defaults went through longer soaks and on more rates, and these
have only had the runs above.

16-frame URBs take another 0.67 ms off the round trip with the same number
of URBs (2.2, 2.9, 4.2 and 6.9 ms of wall time at 32, 64, 128 and 256 frames
with two playback URBs, against 2.9, 3.6, 4.9 and 7.5 ms with 32-frame URBs;
add one period for the figures in the table above). Two 16-frame URBs leave
only 333 us of slack, and the latest completion in the soaks was 298 us:
those ran clean but with almost nothing in reserve. Three 16-frame URBs have
the latency of two 32-frame ones and the same 666 us of slack, so they gain
nothing. 16-frame URBs also mean about 3,000 interrupts a second in each
direction (1,500 at 32 frames). Neither size comes close to the
class-compliant mode at 32 frames (about 1.3 ms behind it). Look at the "ran
dry" count in `/proc/asound/cardN/stream` (below) after a long session on
your own machine before relying on any of these.

### Watching the queues

`/proc/asound/cardN/stream` shows the running session:

```
rate: 48000
urb frames: 32
playback: urbs 2, slack 666 us, latest 108 us, ran dry 0
capture: urbs 8, slack 4666 us, latest 110 us, ran dry 0
```

- **urbs**: the URBs in flight in each direction.
- **slack**: how late an URB's completion may be handled before its queue
  is empty.
- **latest**: the latest a completion has been handled this session,
  against the card's clock.
- **ran dry**: the times it was later than the slack. The card holds the
  whole stream while the playback queue is empty, so each one is a gap in
  both directions, and ALSA does not report it as an xrun.

The figures start again with every session, when the first application sets
the card up.

## Measuring your own

Be careful what your tool reports. The total round trip (output to input) is
one number. Many tools instead report the **offset left to compensate** once
the sound server has subtracted the delay it knows about. The driver reports
its own delay to ALSA (the URBs in flight plus a fixed delay inside the
card), so a sound server already compensates for it, and the residual is
smaller than the total. Compare like with like: total against total, residual
against residual, at the same period and rate.

`sh tools/kernel/delay-sweep.sh` prints the table above for your own card
(stop nothing, but keep the card free of other streams, and turn the monitors
down: it plays a click on playback channels 1/2).

For the class-compliant mode, see
[Shorter URBs at long periods](#shorter-urbs-at-long-periods). If you compare
the two modes and find a different gap, please open an issue with the period,
the rate, the tool, and whether it reports a total or a residual.

## Known issues

**A 32-frame period has one open issue.** Once, with the 32-frame URBs of
the earlier fixed setting, the unit stopped exchanging audio altogether (it
still answered control requests) and only came back after being unplugged: a
module reload, a USB re-enumeration and a host reboot did not help. Once, a
single 192 kHz session came up silent and the next one was fine. Neither has
been reproduced since, and neither has been seen at 256. If the card goes
silent, unplug and replug it.

**Below 32 frames, capture is the weak side**: 16 frames lost one capture
buffer in ten minutes and 8 frames loses them regularly, while playback holds.
Use these for monitoring, not for recording.
