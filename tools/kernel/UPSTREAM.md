# Upstream submission — snd-usb-babyface-pro

This driver implements the **proprietary mode** of the RME Babyface Pro
FS (VID `0x2a39`, PID `0x3fc0`).  In that mode the PCM stream runs on
INTERRUPT endpoints (interface 5, ep 0x01 OUT / 0x82 IN) instead of the
class-compliant isochronous path, so `snd-usb-audio` cannot handle it
and the driver is standalone, modeled on `snd-usb-caiaq`.

## What is hardware-validated (2026-08, on a real unit)

- Interrupt-endpoint PCM: full-duplex 2 ch S24_LE, 9 rates
  32–192 kHz (3 bandwidth alts), zero xruns in sweeps and soaks
  (period ≥ 32 frames; 16 with nurbs=16, monitoring-grade).
- Vendor-request mixer (all decoded from Windows USB captures,
  `tools/usbdump/PROTOCOL.md`):
  - 6 output masters + mutes (the 8-bit register is the real volume,
    0.5 dB/step; the 16-bit is a kept-in-sync companion),
  - 4 mic preamp gains (0–65 dB, 3.25 dB/step) + 48V/PAD per mic
    (relay clicks and front-panel LEDs verified),
  - 84 crosspoints (6 outputs × 14 sources) on the two maps
    (standard + low),
  - pitch/varispeed (−5%…+5%, 16.8 fixed-point DDS quads),
  - loopback (30-channel 0x15 map), AN1>2, AN1/2 link, MS processor,
    width, FX send, DIM (−20 dB absolute on Phones),
  - front-panel poll (0x17 readback → read-only ALSA controls:
    buttons, wheel, IN/OUT selection, MIX, DIM).
- Re-probe resilience: full mixer cache restored across unbind/rebind
  and across S3 suspend/resume (the firmware has no mixer readback).
- Regression suite `tools/kernel/regress.sh`: 40/40 (rate sweep with
  signal tap, start/stop stress, mixer-restore, mid-stream disconnect).

## Files (as submitted, all checkpatch-clean)

- `babyfacepro.c` — core driver: vendor requests + cold init,
  interrupt-URB PCM streaming, mixer-state persistence across
  re-probes/resume, card lifecycle (probe/disconnect/PM/module entry)
- `babyfacepro-ctl.c` — ALSA control surface: mixer (masters, preamp,
  gains, crosspoints, flags, pitch, loopback…), front-panel readback
  poll + controls, hardware DSP EQ
- `babyfacepro.h` — shared state + register map

## Integration diff (kernel tree)

`sound/usb/Makefile`:

```make
snd-usb-babyface-pro-objs := babyfacepro.o babyfacepro-ctl.o
obj-$(CONFIG_SND_USB_BABYFACE_PRO) += snd-usb-babyface-pro.o
```

`sound/usb/Kconfig` — the entry in `tools/kernel/Kconfig`
(`config SND_USB_BABYFACE_PRO`, tristate, selects SND_PCM).

`MAINTAINERS` entry (add near the other USB sound drivers):

```text
RME BABYFACE PRO FS DRIVER (PROPRIETARY MODE)
M:	Ismaïl Bahloul <i.bahloul01@gmail.com>
L:	alsa-devel@alsa-project.org (moderated for non-subscribers)
S:	Maintained
F:	sound/usb/babyfacepro.c
F:	sound/usb/babyfacepro-ctl.c
F:	sound/usb/babyfacepro.h
```

## Before sending (reviewer will ask)

1. ~~**Squash to a small patch series** (probe/stream, controls, panel,~~
   ~~state persistence) with one driver per `sound/usb/babyfacepro.c` —~~
   ~~the split into 6 files is for development; upstream sound drivers~~
   ~~are usually single-file or two-file.~~ DONE 2026-08-28: squashed to
   two files — `babyfacepro.c` (core: protocol/pcm/state/lifecycle) +
   `babyfacepro-ctl.c` (ALSA controls: mixer/panel/eq) — build,
   `sparse`/`W=1`/`checkpatch` clean, live-tested on the physical unit.
   Still needs turning into an actual patch *series* (probe/stream,
   controls, panel, state persistence as separate commits) before
   `git send-email` — the two-file squash is the target layout, not
   yet the target commit structure.
2. **`request_firmware`?** No — the device needs no firmware upload;
   the cold init is a fixed vendor-request burst (documented).
3. **Suspend/resume + autosuspend**: S3 verified. USB autosuspend was
   untested and nothing paused the panel poll/keepalive for it, so
   DONE 2026-08-28: explicitly disabled with `usb_disable_autosuspend()`
   at probe (balanced with `usb_enable_autosuspend()` at disconnect) —
   live-tested, `power/control` reads back `on`, clean dmesg. This is
   the safe interim: full autosuspend support (pausing the panel poll/
   keepalive and pairing `usb_autopm_get/put_interface` around the
   stream) is a deliberate follow-up, not implemented/tested this
   round — say so explicitly in the cover letter rather than shipping
   an untested code path.
4. ~~**The panel poll** runs at 50 Hz continuously (vendor reads).  If~~
   ~~reviewers object to always-on polling, gate it on the card having a~~
   ~~control file open or make the interval a module param.~~
   DONE 2026-08-28: `panel_poll_ms` module param (10-1000 ms, 20 =
   default/unchanged), live-tested (loaded at 50 ms, panel controls
   still read correctly, no dmesg errors). Still always-on regardless
   of whether a control file is open — only the interval is tunable,
   not gated on usage — flag this if a reviewer wants the stronger
   fix.
5. **Open protocol items** (documented in PROTOCOL.md, not blockers):
   the preamp readback byte0 index semantics (0x003F vs 0x0000), the
   width strip-ownership tail (cap_width7 family), the ref-level
   3-state map, the EQ HF warp.
6. **Device naming**: the module/card name is `Babyface Pro FS`
   (the FS suffix matters — the non-FS unit has a different PID).

## Build / test commands

```sh
# out-of-tree build (CachyOS clang kernel)
make LLVM=1 -C /lib/modules/$(uname -r)/build M=$PWD modules

# non-regression suite (needs the card, no other client holding it)
sh tools/kernel/regress.sh --dur 1 --mixer-restore --disconnect-test

# style
/lib/modules/$(uname -r)/build/scripts/checkpatch.pl --no-tree --file <file>
```
