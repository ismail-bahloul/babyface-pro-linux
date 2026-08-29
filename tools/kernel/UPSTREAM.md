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

Live under `sound/usb/babyfacepro/` (a subdirectory, NOT flat files
directly in `sound/usb/` — corrected 2026-08-28 after actually
building the integration: this matches the snd-usb-caiaq convention,
and every other vendor-specific USB sound driver in current
linux-next is a subdirectory too, e.g. `6fire/`, `bcd2000/`, `caiaq/`,
`hiface/`, `line6/`. `babyfacepro.c` calls 8 functions defined in
`babyfacepro-ctl.c` directly from `probe()`, so the two files can
only ever be built/linked together — this also killed any hope of a
clean file-boundary patch split, see item 1 below).

- `babyfacepro.c` — core driver: vendor requests + cold init,
  interrupt-URB PCM streaming, mixer-state persistence across
  re-probes/resume, card lifecycle (probe/disconnect/PM/module entry)
- `babyfacepro-ctl.c` — ALSA control surface: mixer (masters, preamp,
  gains, crosspoints, flags, pitch, loopback…), front-panel readback
  poll + controls, hardware DSP EQ
- `babyfacepro.h` — shared state + register map
- `Makefile` — `snd-usb-babyface-pro-y := babyfacepro.o
  babyfacepro-ctl.o` + `obj-$(CONFIG_SND_USB_BABYFACE_PRO) +=
  snd-usb-babyface-pro.o` (copy of `sound/usb/caiaq/Makefile`'s
  pattern)

## Integration diff (kernel tree)

`sound/usb/Makefile` — add `babyfacepro/` to the subdirectory list:

```make
obj-$(CONFIG_SND) += misc/ usx2y/ caiaq/ 6fire/ hiface/ bcd2000/ qcom/ babyfacepro/
```

`sound/usb/Kconfig` — add before `source "sound/usb/line6/Kconfig"`
(`config SND_USB_BABYFACE_PRO`, tristate, selects SND_PCM).

`MAINTAINERS` entry (added alphabetically, before `RNBD BLOCK
DRIVERS`):

```text
RME BABYFACE PRO FS DRIVER (PROPRIETARY MODE)
M:	Ismaïl Bahloul <i.bahloul01@gmail.com>
L:	alsa-devel@alsa-project.org (moderated for non-subscribers)
S:	Maintained
F:	sound/usb/babyfacepro/
```

The RFC-ready patch (`git format-patch` output of exactly this diff,
built and verified against a real linux-next checkout — see item 7
below) lives at `patches/0001-ALSA-usb-add-RME-Babyface-Pro-FS-driver-proprietary-.patch`.

## Before sending (reviewer will ask)

1. ~~**Squash to a small patch series** (probe/stream, controls, panel,~~
   ~~state persistence) with one driver per `sound/usb/babyfacepro.c` —~~
   ~~the split into 6 files is for development; upstream sound drivers~~
   ~~are usually single-file or two-file.~~ DONE 2026-08-28, but as ONE
   patch, not a series: squashed to two files — `babyfacepro.c`
   (core: protocol/pcm/state/lifecycle) + `babyfacepro-ctl.c` (ALSA
   controls: mixer/panel/eq) — build, `sparse`/`W=1`/`checkpatch`
   clean, live-tested on the physical unit. A file-boundary patch
   series (patch 1 = babyfacepro.c, patch 2 = babyfacepro-ctl.c) was
   considered and rejected: `babyfacepro.c` calls 8 functions defined
   in `babyfacepro-ctl.c` straight from `probe()`, so patch 1 alone
   wouldn't link — the only way to make that bisectable would be
   throwaway stub functions in patch 1, which is worse than one clean
   patch. This also matches common practice for a wholesale new-driver
   addition (nothing to bisect in code that doesn't exist yet). See
   the RFC patch at `patches/0001-...patch`.
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
   width strip-ownership tail (cap_width7 family), and the EQ HF warp.
   (The ref-level 3-state map was FULLY DECODED 2026-08-26 — see
   LINUX-VALIDATION.md — and the driver forces the +4dBu default; it
   just isn't exposed as a control.)
6. **Device naming**: the module/card name is `Babyface Pro FS`
   (the FS suffix matters — the non-FS unit has a different PID).
7. **linux-next compile test + get_maintainer.pl** — DONE 2026-08-28:
   shallow-cloned linux-next (20260828 snapshot), `make modules_prepare`
   (not a full build — no Module.symvers, so MODPOST is symbol-unresolved
   by design; `KBUILD_MODPOST_WARN=1` gets past that to confirm the .ko
   still links). Compiled clean against today's headers, zero source
   changes needed. Also ran linux-next's own (newer) `checkpatch.pl
   --strict`, which surfaced 34 CHECK-level style nits `selftests.sh`
   silently misses (it only grep-filters for ERROR|WARNING, not CHECK) —
   fixed 30 of them (alignment-to-open-paren, stray blank lines, a
   chained assignment, a line ending in `(`); left 4 as deliberate
   false positives: `bInterfaceNumber` (CamelCase — it's usb.h's own
   struct field, not renameable), `(1 << 27)` → `BIT()` (declined —
   `BIT()` returns `unsigned long`, risky in this file's signed
   `s32`/`s64` Q27 fixed-point math), and `ang` "misspelled" ×2 (a
   real CORDIC angle variable, not a typo — this is literally the
   same false "fix" that `checkpatch --fix-inplace` silently applied
   when first tried, which is also why `--fix-inplace` output was
   discarded wholesale rather than trusted: it also changed
   `(1 << 27)` to `BIT()` unprompted).
   `get_maintainer.pl -f` (run from the linux-next tree with the new
   files copied to their target `sound/usb/` paths) →
   Jaroslav Kysela <perex@perex.cz>, Takashi Iwai <tiwai@suse.com>,
   linux-sound@vger.kernel.org, linux-kernel@vger.kernel.org. Re-run
   before actually mailing — MAINTAINERS entries can change.

## Cover letter

The mailing cover letter (0/N email, separate from the 1/N patch) is
`patches/COVER-LETTER.md` — includes the known-limitations block
(autosuspend, open protocol items, load-time latency profile) that the
`Before sending` items below ask to state explicitly.

## Follow-ups (post-merge)

- **Dynamic buffer reconfiguration**: switch the latency profile (e.g.
  256-sample default ↔ 16-frame low-latency floor) **at runtime** like
  the Windows Fireface USB Settings panel, instead of the current
  load-time-only module params (`frames_per_urb`/`nurbs`, read once in
  `probe()`; today switching means a reload/rebind).  See
  `KERNEL-DRIVER.md` Known-gaps item 15 for the design sketch. Kept
  out of the RFC on purpose: it touches the streaming core and is not a
  submission blocker.

## Build / test commands

```sh
# out-of-tree build (CachyOS clang kernel)
make LLVM=1 -C /lib/modules/$(uname -r)/build M=$PWD modules

# non-regression suite (needs the card, no other client holding it)
sh tools/kernel/regress.sh --dur 1 --mixer-restore --disconnect-test

# style
/lib/modules/$(uname -r)/build/scripts/checkpatch.pl --no-tree --file <file>
```
