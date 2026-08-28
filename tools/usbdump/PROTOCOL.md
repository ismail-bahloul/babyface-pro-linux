# RME Babyface Pro FS protocol — proprietary mode (RE in progress)

Discovered by USB capture (USBPcap) while watching TotalMix FX drive the
Babyface Pro FS. All commands go through **vendor requests on endpoint 0**
(control transfers). The interrupt (0x01/0x02) and bulk endpoints do NOT
carry the mixer commands — control is 100% vendor control.

## General format

One vendor transfer = 2 USBPcap records (USBPcap ≥ 1.5):
1. **SETUP** — the 8-byte USB setup packet.
2. **COMPLETE** — the returned data (read) or nothing (write).

The setup packet follows the RME convention:

```
bmRequestType = 0xC0  (read: IN, vendor, device)
                0x40  (write: OUT, vendor, device)
bRequest      = <register address / command code>
wValue        = <value to write>  (write)
wIndex        = <sub-address / channel>  (write)
wLength       = 4 (read) / 0 (write)
```

## Reads (driver polling, ~50-250 Hz)

The driver polls 5 status registers in a loop (4-byte replies):

| bReq | Observed reply (idle) | Meaning |
|------|-----------------------|---------|
| 0x11 | `40 0E 0F 02`         | status |
| 0x17 | `0D 01 80 40` / `0C 01 80 40` | **global status — byte 0 bit 0 = 48V Mic 1 flag** (0x0D = on, 0x0C = off — mapping verified on hardware 2026-08-22; the earlier note had it inverted) |
| 0x1C | `03 01 42 01`         | status |
| 0x1E | `40 35 30 56`         | status |
| 0x1F | `97 16 3A A5`         | status |

Registers 0x11/0x1C/0x1E/0x1F did not change during the tests (sample
rate unchanged). To correlate with more tests (sample rate changes, sync,
etc.).

## Writes (TotalMix actions)

All in `dir=OUT bReq=<code> wValue=<value> wIndex=<register> wLen=0`
(the value lives in wValue; there is no data phase).

### Fader volume

Every fader movement produces writes in (left/right) pairs:

```
bReq=0x1A  wValue=<8-bit val>    wIndex=0x0006 / 0x0007   (with a certain fader)
bReq=0x12  wValue=<16-bit val>   wIndex=0x03E2 / 0x03E3   (same fader)
bReq=0x12  wValue=<16-bit val>   wIndex=0x0034 / 0x004E   (another fader, no 0x1A)
```

- The registers are crosspoint addresses; the high wIndex flags
  (0x4000/0x8000/0xC000) vary on every write (role to determine —
  likely a counter/transaction).
- 16-bit scale: values rise with the fader (0x0317 → 0x139E
  ≈ -40 dB → -20 dB on 0-65535). To be calibrated precisely.

### Crosspoint address map (decoded by sweep — cap_sweep.pcap)

The volume of a source channel into an output pair is a crosspoint
register computed as:

```
L register = 0x0034 + 0x0034 * out + src_idx
R register = 0x004E + 0x0034 * out + src_idx
```

Source channel indexes (`src_idx`) — **resolved by cap_srcmap.pcap**:

| Source | idx |
|---|---|
| AN1 / AN2 / AN3 / AN4 | 0 / 1 / 2 / 3 |
| AS1/2 (stereo pair) | 4 / 5 |
| ADAT3/4 | 6 / 7 |
| ADAT5/6 | 8 / 9 |
| ADAT7/8 | 10 / 11 |
| Playback 1 … 6 (stereo pairs) | 12/13 … 22/23 |

Mono sources use the same index on L and R; stereo pairs use the even
index on L and the odd index on R (L register = base + even, R register =
base + odd).

**Output ordering — CORRECTED 2026-08-24 (hardware-verified, kernel
driver): the crosspoint map lists the Phones FIRST, unlike the master
map.** The first crosspoint block (0x0034, `out = 0`) feeds the output
whose MASTER is 0x03E2/0x0006 (the Phones/PH3/4 — the monitor
output); the second block (0x0068, `out = 1`) feeds the 0x03E0/0x0004
master (AN1/2). Verified live: zeroing the 0x0034 block mutes the
headphones (and the capture MIX mode on Phones writes 0x0034/0x004E —
cap_mix.pcap), zeroing the 0x0068 block does not. The old cap_sweep
labels ("AN1 into AN1/2 wrote 0x0034") were mis-attributed; the
remaining blocks 2-5 (AS1/2, ADAT3/4, ADAT5/6, ADAT7/8) align with
the master order. Crosspoint order: out 0 = PH3/4, out 1 = AN1/2,
out 2-5 = AS1/2, ADAT3/4, ADAT5/6, ADAT7/8. The kernel driver maps
canonical output N → register block `{1, 0, 2, 3, 4, 5}[N]`;
`tuxmix-usb/src/map.rs` has the SAME mis-pairing and must be fixed.

**THE "CROSS" REGISTERS MUST BE ZEROED (2026-08-25, hardware-verified —
the mono-Phones root cause):** the L side of an output sums its
L-block, the R side its R-block.  A stereo source (even idx_l / odd
idx_r) therefore needs the "cross" slots silent — L-reg at the odd
indices (5,7,…23) and R-reg at the even (4,6,…22) — otherwise PB1 R
feeds the L side and PB1 L the R side (L+R on both = MONO).  The
0x16 cold-init clear covers only 0x00-0x3D (block 0 L-reg 0-9), so
the cross registers SURVIVE from the previous session; TotalMix
writes them at 0x0000 (a fresh scene upload covers them), and the
kernel driver now zeroes them explicitly
(`bf_crosspoint_clear_cross`, probe default + stream-start restore).
Ear-verified: hard-L/hard-R tones now play one ear at a time on the
Phones.  The RE's usbfs init (`loopback3.c`/`lr_test.c`) has the same
gap — its 0x16 0x00-0x3D clear does not cover them either (stale
values polluted earlier loopback measurements).

### CUE on outputs — cap_cue.pcap (DECODED + HARDWARE-VERIFIED 2026-08-23, cuetest.c)

CUE on an output strip toggles the **playback crosspoints of the AN1/2
output** (the CUE bus = the AN1/2 monitor path): 10 `0x12` writes, PB1-6
L/R pairs, **bare wIdx (no 0xC000 transaction flag)**: `0x0000` = CUE ON
(playbacks muted), `0x2000` = CUE OFF (restore). One pair is excluded
per output — the output's dedicated CUE source, kept at 0x2000:
**source PB = out_idx+1** (verified AN1/2→PB1, PH3/4→PB2, AS1/2→PB3,
ADAT3/4→PB4, ADAT7/8→PB6; ADAT5/6→PB5 predicted). No CUE on hardware
inputs. The 0x10 0x05CF keepalive is suspended while TotalMix toggles.

### Solo / Mute of the selected channel — cap_lowmap.pcap (SUPERSEDED)

An early capture wrote 0x2000 (ON) / 0x0000 (OFF) to 0x0004/0x001F/
0x000C/0x0027 together. Those are **AS1/2's and PB1's LOW MAPS** (see
`set_solo` below / cap_solo2.pcap), NOT solo flags — that write muted
the two strips. The real solo mechanism = mute-the-others, see the
solo section below.

### Per-channel MUTE — DECODED (cap_mute2.pcap, 2026-08-24)

Toggling MUTE on every strip + the global M/S/F. The mute has NO
dedicated flag register — it ZEROES the strip's crosspoints (0x0000)
and RESTORES them on unmute:

| Strip | Written registers | Restore value |
|---|---|---|
| AN1/2 INPUT | low map 0x0000/0x001A + 0x0001/0x001B (4 regs) | 0x1000 (the linked −6 dB pair default) |
| PB1-5 (playback) | standard out0 pair 0x0040+2k / 0x005B+2k (PB1→out0 = 0x0040/0x005B, PB2 = 0x0042/0x005D…) | 0x2000 (the "active" marker — same as CUE) |
| Output master | the known pattern (0x12 0x0000 on 0x03E0+2·out + 0x1A 0x003B on the 8-bit companion) | the saved fader |
| Solo | the solo registers 0x000C/0x0027 (+ 0x0040/0x005B, 0x0144/0x015F = PB1 crosspoints into out0/out5 — the selected channel's routes) | 0x2000 |

So a strip mute in TotalMix is GLOBAL (the strip's crosspoints into
multiple outputs are zeroed — the visible writes follow the current
submix + the low map for inputs). `set_mute` implemented: zero the
low map (inputs) + all-output standard crosspoints, restore from the
model on unmute (the exact 0x2000/0x1000 marker values vs the fader
value = Linux check).

### Per-channel SOLO — DECODED (cap_solo2.pcap, 2026-08-24)

Solo has NO dedicated flag register either — it is **mute-the-others**:
while a strip is soloed, every OTHER strip's crosspoints go 0x0000, and
un-soloing restores them. The capture (submix = AN1/2, so the writes
are the low-map mirror of the AN1/2 submix strips) shows the toggle:

```
SOLO ON  : 0x12 0x0000 on AN1 low map (0x0000/0x001A) + AN2 (0x0001/0x001B)
           + PB1 low map (0x000C/0x0027)  → the OTHER strips muted
SOLO OFF : 0x12 0x1000 on AN1/AN2 low maps + 0x12 0x2000 on PB1 low map
           → restored to the "active" markers (0x1000 linked input pair,
           0x2000 playback — same values as the mute restore)
```

The soloed strip's own low map goes to the same "active" marker
(0x2000 playback / 0x1000 linked input) — the capture shows PB1 at
0x2000 while AN1/2 are 0x0000. `set_solo` (tuxmix-core) implements
mute-the-others across ALL outputs' crosspoints + low maps (not just
the current submix) so the mute is global, with exclusive solo (the
TotalMix default): soloing a strip clears the other strips' solo flags,
and un-solo restores every strip from the model (a strip muted by its
own M button stays muted). The exact 0x2000/0x1000 "marker" restore vs
the real fader values = Linux check, same as mute.

### FX RETURN = source idx 24/25 — DECODED (cap_fxroute.pcap, 2026-08-24)

Dragging the FX-return fader in TotalMix revealed the return is a FULL
mixer source at **idx 24/25** (right after the 12 playbacks at 12-23):

- low map: `0x0018/0x0033` (L = 0x0000+24, R = 0x001A+25)
- standard crosspoint into out0: `0x004C/0x0067` (= 0x0034+24 /
  0x004E+25); the other outputs follow the usual stride (PH3/4 =
  0x0080/0x009B…)

So the wet reverb return (OUT stream ch12/13) is mixed into any output
like a regular source — TotalMix routes it by writing its crosspoints.
**Re-interpretation of the old "solo registers"**: 0x000C/0x0027 is
NOT a solo flag register — it's **PB1's low map** (0x0000+12 /
0x001A+13), and 0x0004/0x001F = AS1/2's low map (idx 4/5). The
earlier "solo = 0x2000/0x0000 on 0x0004/0x001F/0x000C/0x0027" was
actually zeroing those strips' low maps = muting the non-soloed
channels (classic solo = mute the others). The true per-channel solo
mechanism = mute the OTHER strips' crosspoints.

### Second crosspoint map ("low map") — cap_srcmap.pcap / cap_lowmap.pcap

Dragging a source fader SOMETIMES writes a SECOND register map instead
of the standard one:

```
L register = 0x0000 + src_idx
R register = 0x001A + src_idx
```

Observed pairs: AN1 → (0x0000, 0x001A), AS1/2 → (0x0004, 0x001F),
Playback1 → (0x000C, 0x0027). Same value on both sides (centered).

Trigger analysis (cap_lowmap.pcap, three identical AN1 fader drags):

| Context | Map written |
|---|---|
| drag AN1 (no click) | LOW map (0x0000/0x001A) |
| click PH3/4, drag AN1 | STANDARD map (0x0034/0x004E = AN1→AN1/2!) |
| click AN1/2, drag AN1 | LOW map (0x0000/0x001A) |

The standard map write went to AN1→AN1/2 even though PH3/4 was clicked
— the click did NOT change the destination, but DID change the register
map from low to standard. Not fully explained; possibly TotalMix 2.0's
selected-channel panel vs strip-fader path. For TuxMix: write BOTH maps
or stick to the standard one + verify against the hardware.

**Output strip order (TotalMix 2.0, Babyface Pro FS) — reported by the
user; the bases below follow the strip order left → right:**

| Strip | base | verified by |
|---|---|---|
| AN1/2   | 0x034 | all sources ✓ |
| PH3/4   | 0x068 | AN1 ✓ |
| AS1/2   | 0x09C | AN1 ✓ |
| ADAT3/4 | 0x0D0 | AN1 ✓ |
| ADAT5/6 | 0x104 | AN1 ✓ |
| ADAT7/8 | 0x138 | confirmed (cross-checked 2026-08-28 against the kernel driver's independently hardware-verified `BF_REG_CROSS_BASE_L + BF_REG_CROSS_STRIDE·bf_xpoint_block[5]` = 0x0034 + 0x0034·5 = 0x138) |

(R bases = L + 0x1A.)

⚠️ The sweep instructions used speculative names (AN3/4, ADAT1/2...) that
match the hardware spec but NOT the TotalMix strip labels (the Babyface
Pro FS shows: inputs AN1/2, Instr 3/4, AS1/2, ADAT 3/4-7/8; outputs
AN1/2, PH3/4, AS1/2, ADAT 3/4-7/8; playbacks AN1/2, PH3/4, AS1/2,
ADAT 3/4-7/8 — 6 pairs each). The strip ORDER above is therefore
certain (0x34 stride, base 0x34), but the NAME per base needs one
confirm capture (AN1 fader → the strip labeled PH3/4, then AS1/2).

TotalMix strips can be split stereo → mono with the "stereo" button
(separate AN1 / AN2 buses); how that changes the register writes is not
mapped yet.

Observations from the sweep:

- A fader move writes both the L and R registers when the source is
  centered (e.g. AN1 into AN1/2 wrote 0x34/0x4E), but only one side
  when the channel is panned off-center (AN3 → 0x36 only, AN4 → 0x51
  only, AN1 into AN3/4-PH3/4-ADAT1/2-ADAT3/4 → 0x68/0x9C/0xD0/0x104
  only). The writes follow the current pan state.
- Register 0x0000 receives a fader gesture too (likely the selected
  output's master volume) — to confirm.
- The high wIndex flags cycle 0xC000 → 0x4000 → 0x8000 → 0x0000 on
  every write — a 2-bit transaction counter.
- The 0x1A companion writes (8-bit, wIdx 0x0006/0x0007) did NOT appear
  in the sweep — they seem tied to specific crosspoints (the PH3/4
  monitor path seen in earlier captures), not to every fader move.

### Mute (identified — channel mapped to registers 03E2/03E3)

```
bReq=0x1A  wValue=0x003B  wIndex=0x0006 / 0x0007   (MUTE ON)
bReq=0x12  wValue=0x0000  wIndex=0x03E2 / 0x03E3   (volume to -inf)

bReq=0x1A  wValue=0x00F3  wIndex=0x0006 / 0x0007   (MUTE OFF)
bReq=0x12  wValue=<vol>   wIndex=0x03E2 / 0x03E3   (volume restored)
```

Note: the mute of the other tested channel (registers 0034/004E) only
emitted `bReq=0x12 wVal=0` (no 0x1A). Both forms exist.

### Pan / Balance (identified — stereo crosspoint)

The pan of a stereo channel is a **balance**: it only changes the volume
of ONE of the two crosspoint channels (the other stays fixed). On the
tested channel (PCM PH3/4 → PH3/4), pan only varied channel
`0007 / 03E3`:

```
bReq=0x12  wValue=<fixed L vol>     wIndex=0x03E2   (+ bReq=0x1A on 0x0006)
bReq=0x12  wValue=<variable R vol>  wIndex=0x03E3   (+ bReq=0x1A on 0x0007)
```

The two channels of a crosspoint: `0x0006/0x0007` (8-bit) and
`0x03E2/0x03E3` (16-bit). A mono pan (input AN1) emits NOTHING — pan
only exists on stereo crosspoints.

**Pan law = linear in raw (cap_pan_stereo.pcap, 2026-08-22):** the
varied side attenuates linearly from the current raw to `0x0000` at
100%: `varied = fixed_raw · (1 − |pan|)` (observed on the AN1/2 output
balance, 0x03E0/0x03E1 one-sided sweeps with a constant 0x9C raw
step). `tuxmix-usb::set_crosspoint_balance` implements exactly this.

### 48V (phantom) — per-mic, verified on hardware (2026-08-22)

```
bReq=0x17  wValue=<state>  wIndex=0x003F   (48V per-mic state, FULL write)
bReq=0x21  wValue=0x0000  wIndex=0x0000   (commit, follows every 0x17)
```

State byte layout (the two front-panel P48 LEDs follow the bits
exactly):

| wValue | Effect |
|--------|--------|
| `0x0C` | all 48V off, PAD off (base) |
| `0x0D` | Mic 1 (AN1) 48V on — left P48 LED |
| `0x0E` | Mic 2 (AN2) 48V on — right P48 LED |
| `0x0F` | both 48V on |
| `0x1C` | Mic 1 48V off + **PAD** (relay clicks) |
| `0x1D` | Mic 1 48V on + PAD |
| `0x2E` | Mic 2 48V on + **PAD Mic 2** (relay clicks, -4.3 dB
  measured on the noise floor) |

Layout: bits 0-1 = 48V Mic 1/2, **bit 4 = PAD Mic 1, bit 5 = PAD Mic
2** (`0x10 << mic`, verified by relay click + level drop), bits 2-3 =
constant base (role unknown, keep set), bits 6-7 = presumed PAD Mic
3/4. The write is a FULL state (0x0E turns AN1 off). **PAD toggles a
physical relay — the audible click** (user observation; 48V itself is
silent). AN2's phantom works (verified: mic heard on AN2 with bit 1
set).

The state is read back in the polled register 0x17 (byte 0).

### Preamp block (48V + gain + PAD) — cap_gain_solo.pcap + hardware

The register `0x17 wIndex=0x003F` is a **preamp state bitmask** written
before every preamp change, followed by the `0x21` commit. Bits observed:

| wValue | Meaning |
|--------|---------|
- `0x000D` | 48V Mic 1 ON, PAD off |
- `0x000C` | 48V Mic 1 OFF, PAD off |
- `0x001D` | 48V Mic 1 ON, **PAD on** |
- `0x001C` | 48V Mic 1 OFF, PAD on (captured: 0x0C -> 0x1C = PAD only) |

So: base `0x0C`, bit 0 = 48V ON flag (0x0D = on), **bit 0x10 = PAD**.
The other bits likely cover the remaining mics/inputs — to map with a
targeted capture.

### Preamp gain — cap_gain_solo.pcap

Every gain-knob movement produces, after the `0x17`+`0x21` preamble, a burst
of four 8-bit writes (one per mic preamp):

```
bReq=0x1A  wValue=<gain mic1>   wIndex=0x0000
bReq=0x1A  wValue=0x0000        wIndex=0x0001
bReq=0x1A  wValue=0x0000        wIndex=0x0002
bReq=0x1A  wValue=0x0000        wIndex=0x0003
```

- **wIndex 0x0000-0x0003 = gain of Mic 1-4.**
- The **value field is 5 bits (bits 0-4, 0-31 ≈ 0-62 dB in 2-dB
  steps)**; bits 0x20/0x40/0x00 cycle as a transaction counter (bits
  5-6). The raw wValue looks like 0x2A, 0x0A, 0x49, 0x29, 0x09, ...
  (counter 2/0/4 + value). **Mask with `& 0x1F` to recover the value**
  (`& 0x0F` drops the 5th bit and destroyed values ≥ 16 — fixed
  2026-08-22).
- **Calibration (partial, 2026-08-22)**: `tools/usbdump/gainsweep_full.c`
  swept raw 0-31 measuring the mic-noise RMS: ~2 dB/step in the mid
  range, **saturation at raw ≥ 23** (values 24-31 change nothing — the
  device clamps). The absolute raw→dB anchor is NOT yet measured: the
  "raw 17 = 35 dB" note below is an UNVERIFIED assumption (dB/2).
  Definitive calibration = a Windows capture with known dB values
  (see WINDOWS-CAPTURE-PLAN.md, capture 3).

### Solo — cap_gain_solo.pcap

```
bReq=0x12  wValue=0x2000  wIndex=0x000C   (SOLO ON)
bReq=0x12  wValue=0x2000  wIndex=0x0027
bReq=0x12  wValue=0x0000  wIndex=0x000C   (SOLO OFF)
bReq=0x12  wValue=0x0000  wIndex=0x0027
```

The two registers 0x000C and 0x0027 are always written together with the
same value (0x2000 = on, 0x0000 = off). Per-channel solo mapping not yet
resolved (identical writes regardless of which strip was soloed in the
capture).

### Output master fader (selected output) — cap_gain_solo.pcap

Dragging the **master fader of an output strip** writes a shadow pair of
registers — 8-bit + 16-bit volume, like the mute companion pattern:

```
bReq=0x1A  wValue=<8-bit vol>   wIndex=0x0004 + 2*out (L) / 0x0005 + 2*out (R)
bReq=0x12  wValue=<16-bit vol>  wIndex=0x03E0 + 2*out (L) / 0x03E1 + 2*out (R)
```

| Output | 16-bit master | 8-bit master | verified |
|---|---|---|---|
| AN1/2   | 0x03E0/0x03E1 | 0x0004/0x0005 | ✓ cap_gain_solo |
| PH3/4   | 0x03E2/0x03E3 | 0x0006/0x0007 | ✓ cap_topo_cal |
| AS1/2   | 0x03E4/0x03E5 | 0x0008/0x0009 | ✓ cap_as_test |
| ADAT3/4 | 0x03E6/0x03E7 | 0x000A/0x000B | confirmed (cross-checked 2026-08-28, see below) |
| ADAT5/6 | 0x03E8/0x03E9 | 0x000C/0x000D | confirmed (cross-checked 2026-08-28, see below) |
| ADAT7/8 | 0x03EA/0x03EB | 0x000E/0x000F | confirmed (cross-checked 2026-08-28, see below) |

The three ADAT rows were originally "predicted (stride)" — extrapolated
from the confirmed AN1/2 - AS1/2 rows, never independently captured.
Cross-checked 2026-08-28 against the kernel driver's own addressing
(`BF_REG_MASTER_16` = 0x03E0 + 2·out, `BF_REG_MASTER_8` = 0x0004 +
2·out — hardware-verified 2026-08-24 per `mixer.c`, and re-confirmed
live via `tuxmix-core`'s ALSA backend read/write round-trip on
ADAT5/6 and ADAT7/8 the same day): out=3/4/5 land exactly on the
predicted values above. Upgraded from predicted to confirmed on that
basis — no new USB capture was taken, the confirmation is via the
independently-implemented and hardware-verified kernel driver, not a
second protocol-level capture.

- The 16-bit values run through the same scale as the crosspoint faders
  (the drag passed 0x0317 = -40 dB mid-ramp).
- The 8-bit value tracks the same drag (0x73→0xF4 observed).

**THE 8-BIT REGISTER IS THE REAL OUTPUT VOLUME — HARDWARE-VERIFIED
2026-08-24 (kernel driver, live):** writing the 8-bit changes the
level, the 16-bit does NOT (0x2000 vs 0x0103 on the 16-bit = no
change; 0xF3 vs 0x73 on the 8-bit = full vs silent). The 16-bit is a
companion kept in sync by TotalMix (it also carries the mute at
0x0000).

**8-bit master scale (calibrated 2026-08-24 from cap_calib master
sweeps + live sweeps): 0.5 dB per step, 0xF3 = 0 dB** (the scene-load
default), bottom 0x73 = -64 dB (silence), top 0xFF = +6 dB. Formula:
`8bit = 0xF3 + 2·dB`, i.e. `0xF3 + 12·log2(v16/0x2000)`. Mute code =
0x3B (the mute pattern writes 8-bit 0x3B + 16-bit 0x0000; the unmute
restores the 8-bit volume code + the 16-bit value).
- Same pattern family as the mute/pan path: `0x0006/0x0007` +
  `0x03E2/0x03E3` were the pair tested earlier (PH3/4 path). So the
  "monitor/selected" block is:
  - `0x03E0/0x03E1` + `0x0004/0x0005` (this capture: AN1/2 master)
  - `0x03E2/0x03E3` + `0x0006/0x0007` (earlier capture: PH3/4 path)
- Whether the address follows the selected output (stride 2 per output:
  0x03E0, 0x03E2, ...) is to be confirmed with a capture on another output.

### Periodic writes while solo testing — cap_gain_solo.pcap

While the solo buttons were being exercised, `0x12 wValue=0x0000/0x2000`
on 0x000C/0x0027 repeated at very regular ~2.09 s intervals (8 writes).
Either a solo keep-alive or the user toggling on a rhythm — the state
alternates 0000→2000 each time.

### Periodic commands (cycles ~1.5-2 s)

```
bReq=0x10  wValue=0x0000  wIndex=0x8000
bReq=0x1D  wValue=0x0000  wIndex=0x0000
bReq=0x14  wValue=0x0000  wIndex=0xC000
bReq=0x13  wValue=0x0000  wIndex=0xC000
```

Role resolved (analysis of cap_audio.pcap): this is the **streaming
keepalive**. During the 8.6 s before streaming started, NO writes were
emitted at all; the trio `[0x10, 0x1D, 0x14]` starts at the very moment
the ISO streaming starts, then the cycle `[0x10, 0x1D, 0x14] → ~1.1-1.5 s
→ 0x13 → ~0.2 s → …` repeats (~1.5-3 s period, irregular).

**There is NO special init sequence** to start streaming: it is just
those few writes + the driver opening the ISO endpoints. No firmware
dump, no register table to upload. Bit 1 of register 0x17 = streaming
active (0x0D → 0x0F).

## Sample rate / clock (cap_rates.pcap + cap_rates2.pcap, 2026-08-22)

**A sample-rate change is PURELY `SET_INTERFACE(5, alt N)` — no vendor
writes at all** (no `0x1B`, no `0x10` rate field). Final table from the
clean 9-rate sweep (cap_rates2.pcap, user-confirmed order):

| Rate | SET_IFACE alt | note |
|---|---|---|
| 32 kHz | 1 | cap_rates2 sweep (48→32→44.1→64→88.2→96→128→176.4→192→48) |
| 44.1 kHz | 1 | same |
| 48 kHz | 1 | CONFIRMED 3× (streaming, cap_fus 96→48, sweep) |
| 64 kHz | 1 | sweep |
| 88.2 kHz | 1 | sweep |
| 96 kHz | 2 | CONFIRMED (cap_fus 48→96, sweep) |
| 128 kHz | 2 | sweep |
| 176.4 kHz | 3 | sweep |
| 192 kHz | 3 | sweep |

**The alt-setting is a BANDWIDTH CLASS, not a 1:1 rate code.** The
sweep (user confirmed the exact order 48→32→44.1→64→88.2→96→128→
176.4→192→48) produced only 3 SET_IFACE — the 3 alt transitions
(88.2→96: 1→2, 128→176.4: 2→3, 192→48: 3→1), ~15 s apart = 2 rate
changes each. Rates within the same class emit NO USB traffic.
Alt classes: **alt 1 (448 B) = 32/44.1/48/64/88.2 kHz, alt 2 (640 B) =
96/128 kHz, alt 3 (1024 B) = 176.4/192 kHz** (alt 0 = 64 B unused).
Rule of thumb: alt = smallest packet size where 8×size ≥ rate×ch×4
B/ms (14ch ≤64k, 10ch at 88.2-128k, 8ch at 176.4/192k).

Alt packet sizes: 0 = 64, 1 = 448, 2 = 640, 3 = 1024 B (`alt 0` unused).

Traffic around each change: `0x10 wVal=0x0010/0x0020/0x0000 wIdx=0x0030`
on a ~7-15 s cycle (phase counter, NOT rate-specific — the wVal cycles
0x0010 → 0x0020 → 0x0000 regardless) + `0x10 0x0001 wIdx=0x05CF`
keepalive (~3 s). The 480-B telemetry (ep 0x85) carries only meter/
level fields — no rate info.

Code impact: `streaming_init`'s `0x1B` block (from the coldplug capture)
stays for COLD START; mid-session rate change = SET_INTERFACE only.
Whether the 0x1B values are rate-dependent is unverified (the sweep
showed NO 0x1B writes — they likely are not).

**Alt verification via the `0x11` readback (cap_rates2.pcap)**: the last
byte of the 4-byte `0x11` reply encodes the current alt-setting as
`2^alt` (alt 1 → 0x02, alt 2 → 0x04, alt 3 → 0x08) plus a low counter
incrementing every ~5 s poll cycle (0x04→0x05→0x06 between changes,
reset to the 2^alt base on each SET_IFACE). On Linux, after
`SET_INTERFACE(5, alt N)`, read `0x11` and check byte 3 & 0x0F for
2/4/8 to confirm the alt took effect. (The `0x17` readback stays
`[0D,01,40,40]` — it does NOT report the rate.)

**HARDWARE-VERIFIED on Linux (2026-08-22, ratetest.c)**: the whole
table above works with **mid-session SET_INTERFACE only**, and the IN
stream runs at the expected rate per alt — but the URB size must match
the alt's frame layout (frame bytes × 256 frames per URB):

| Alt | frame | URB size | measured rate |
|---|---|---|---|
| 1 | 14 ch × 4 = 56 B | 14336 B | 2.69 MB/s = **48 kHz** ✓ |
| 2 | 10 ch × 4 = 40 B | **10240 B** | 3.84 MB/s = **96 kHz** ✓ |
| 3 | 8 ch × 4 = 32 B | **8192 B** | 6.15 MB/s = **192 kHz** ✓ |

14336 B is NOT a multiple of alt-2's 640 B packet (640×22.4) — the
stream stalls there unless the URBs are resized (10240 = 640×16 =
40 B × 256). alt3 worked even at 14336 B (1024×14) but the frame
layout is 32 B. `0x11` byte 3 read 0x02/0x06/0x0A for alt 1/2/3
(2^alt + poll counter) as documented.

### Clock source / no-lock state (cap_clk.pcap, 2026-08-22) — the 0x80 mystery SOLVED

**Changing the clock source (Internal ↔ Optical) produces NO register
writes at all** — only the keepalive `0x10 wVal wIdx=0x05CF` changes:

| Keepalive wVal | 0x17 byte 2 | Meaning |
|---|---|---|
| 0x0001 | 0x40 | clock Internal, locked (normal) |
| 0x0004 | 0x80 | clock source = Optical, NO LOCK |

So `0x17` byte 2 = **0x80 is the clock no-lock state, NOT "streaming
active"** (the old hypothesis — the long-sought "0x41→0x80 transition"
is now explained). The host selects the clock source via the keepalive
wVal (bit 0x04 = Optical); the device reports lock state in the 0x17
readback. On Linux: keepalive `0x10 0x0004 0x05CF` = Optical,
`0x10 0x0001 0x05CF` = Internal. **HARDWARE-VERIFIED on Linux
(2026-08-22, clktest.c)**: baseline 0x0001 → byte 2 = 0x40; 0x0004 →
0x80 immediately and sustained; back to 0x0001 → 0x40. Exact.

Other keepalive flag bits seen (cap_fus2, during the settings phase):
`0x0401` and `0x0041` — believed to be the Optical-Out format
(ADAT/SPDIF) select, unconfirmed (the Optical-Out capture is pending).

### The keepalive `0x10 0x05CF` wVal = host settings-state register (FULLY DECODED 2026-08-22)

The `0x10 wVal wIdx=0x05CF` keepalive (~3 s) doubles as the HOST
SETTINGS COMMAND WORD. Confirmed by dedicated captures:

| wVal | Setting | Capture |
|---|---|---|
| 0x0001 | default (ADAT optical, EQ-record off, clock Internal) | all |
| 0x0004 | clock source = Optical (device: 0x17 byte 2 = 0x80, no lock) | cap_clk |
| 0x0401 | Optical Out = SPDIF (0x0001 = ADAT) | cap_opt |
| 0x0041 | EQ for Record = ON (0x0001 = off) | cap_eqr |

Bit map: bit 2 = clock Optical, bit 6 = EQ for Record, bit 10 = SPDIF
out. The device reacts to these bits (clock: 0x80 no-lock readback).
The SPDIF-In TMS bit is unknown (probably bit 5 = 0x0020, never
captured). Settings changes produce NO register writes — only this
flag word (EXCEPT pitch, which writes the 0x1B DDS quads, and the full
state uploads the app does on open/OK, which are host-side re-syncs
and not needed to change a setting).

### Pitch / varispeed — the 0x1B quad is 24-bit fixed point (cap_pitch.pcap, 2026-08-23, FULL SWEEP DECODED)

A full pitch-slider drag (0 → +5 → -5 → 0, 454 quads) decoded the
quad completely:

- **bank0 = the DDS as 16.8 fixed point**: `wVal` = integer part,
  `wIdx` high byte = 8-bit fraction (`wIdx` low byte = bank 0). At
  0%: `0xC350 0x0000` = 50000.000. The 24-bit sequence is perfectly
  smooth (per-step deltas ~-5664 on the drag).
  `DDS_24 = round(50000 × 256 / (1 + pitch/100))`.
- **bank1 (16-bit wVal) = round(DDS_16 × 0.72562)** — exact on all
  454 quads (err ≤ 1).
- **bank2 (16-bit wVal) = round(DDS_16 × 2/3)** — exact (err ≤ 1).
- **bank3 = QUANTIZED in steps** (sticks at values like 0x7C01 over
  DDS ranges) — the earlier "0x7CFF 0xF803 constant terminator" was
  just the 0% value; bank3 needs a lookup table from the sweep.
- The wIdx high bytes of banks 1/2/3 (their own fraction bits) are
  NOT yet derived — a Linux test should try sending frac=0 for b1/b2/
  b3 with the exact 16-bit parts and verify the pitch applies.

**Linux test DONE (2026-08-23, pitchformula.c) — the fractions do NOT
matter**: a DERIVED quad (bank0 = round(12800000/(1+p/100)) split
16.8, bank1 = round(DDS16×0.72562), bank2 = round(DDS16×2/3), bank3 =
0x7CFF frac 0) produced the IDENTICAL IN rate as the captured verbatim
+4% quad (2795520 B/s both, +4.09% over baseline). So `set_pitch(p)`
can synthesize any pitch: DDS_24 = round(50000×256/(1+p/100)), bank1 =
round((DDS_24>>8)×0.72562), bank2 = round((DDS_24>>8)×2/3), bank3 =
0x7CFF (frac 0), + keepalive 0x10 0x0001 0x05CF. No bank3 lookup
needed (the 0% value works across the tested range).

**PITCH SHIFTS THE SAMPLE RATE DYNAMICALLY (user-observed 2026-08-23)**:
the DDS drives the device clock, so pitch ±5% moves the ACTUAL rate to
nominal × (1 ± 0.05) (e.g. 48 kHz → 50400 Hz at +5%). The stream rate
changes WITHOUT any SET_INTERFACE; re-syncing the rate after a pitch
change must use the effective rate.

Every quad must be followed by the clock keepalive `0x10 0x0001
0x05CF` (cap_fus2 pattern; without it the pitch did not apply).

### The `0x1B` banked register format (cap_fus.pcap, 2026-08-22, updated 2026-08-23)

Fireface USB Settings re-uploads the whole DSP register file on
settings changes: bursts of `0x1B` writes. The wIdx low byte = the
bank (0-3); the wIdx high byte = the 8-bit FRACTION of a 16.8
fixed-point value for the DDS bank (see "Pitch / varispeed" above —
it is NOT a register address). Writes come in quads (one reg per
bank). The 48 kHz clock quad — byte-identical to the coldplug init
block — is:

```
0x1B 0xC350 0x0000   bank 0 = DDS 50000.000 (16.8 fixed, frac 0x00)
0x1B 0x8DB8 0xD201   bank 1 = round(50000 × 0.72562)  (frac 0xD2)
0x1B 0x8234 0xD302   bank 2 = round(50000 × 2/3)      (frac 0xD3)
0x1B 0x7CFF 0xF803   bank 3 = 0.64 × 50000 quantized  (frac 0xF8)
```

**Hardware-verified 2026-08-23** (pitchest.c / tonepitch.c): the bank-0
value is a **clock divisor** — the IN-stream rate × bank0 = const, and
the audio pitch follows (a 440 Hz host tone through PB1 → AN1/2 detuned
with the quads). The quad must be followed by the clock keepalive
`0x10 0x0001 0x05CF`. The two 4% quads in the earlier notes were
sign-flipped: 0xBBCC = +4.00% (rate +4.1%), 0xCB72 = −4.05% (rate
−3.9%). Formula: **DDS ≈ 50000 / (1 + pitch/100)**. FULL sweep decode
(454 quads, cap_pitch.pcap): DDS = 16.8 fixed point, bank1 = 0.72562×
DDS16, bank2 = 2/3×DDS16 (exact), bank3 = quantized (lookup needed).

Full dumps cover 100-337 regs (grows across bursts — the app uploads
more of the register space each time). **Per-setting registers:
QUESTION CLOSED (2026-08-24, cap_fus3/4/5)**: a one-setting-at-a-time
campaign proved the settings (clock source / optical out / EQ-for-
Record / TMS) write the KEEPALIVE FLAG WORD ONLY (0x10 0x05CF —
0x0004 / 0x0401 / 0x0041, TMS = no USB write in v1.276) and the DDS
quads (pitch) — NO per-setting 0x1B register writes exist. A fresh
app launch does NOT re-upload the dump in v1.276 (killed + relaunched
mid-capture: zero writes — the driver caches the state); the big dumps
are a cold-start/full state re-sync snapshot (host-side, redundant
with the decoded writes). Remaining 0x1B unknown: the individual
semantics of the snapshot registers — pure completeness, no function
is unmapped.

## Status stream (bulk IN)

The device continuously pushes **480-byte** blocks on the bulk IN
endpoints (ep 0x85 and 0x8B), ~44 blocks/s:

```
offset 0x00 : 32-bit magic/counter (changes every block)
offset 0x04 : incremental counter (4, 5, 6, ...)
offset 0x08 : variable field (03 31 01 00 ...)
offset 0x10 : 2 × variable 16-bit values
offset 0x60 / 0xF0 : 0x1F 00 00 00 ×2
offset 0x150 : 30 A6 20 00 | 40 04 00 00 | F0 02 00 00 | 20 03 00 00
offset 0x180 : B0 05 00 00 | A0 05 00 00
offset 0x1C0 : 90 00 00 00 ×2
the rest: zeros (nothing active on the input)
```

## Audio stream (decoded — cap_audio.pcap capture)

While streaming, all audio runs on two **interrupt** endpoints
(interface 5, alt-setting 1, 448-B packets; the Windows driver sends
14336-byte URBs = 32×448 B every ~5.33 ms → ~48 kHz):

| Endpoint | Direction | Content |
|----------|-----------|---------|
| ep 0x01  | OUT (host→dev) | playback audio (14 channels) |
| ep 0x82  | IN  (dev→host) | input audio (14 channels) |

> **Endpoint identity — CORRECTED (this session, verified 3 ways).** An
> earlier note here claimed `0x01`/`0x82` are ISOCHRONOUS — that was a
> misread of `bmAttributes` (0x03 = `USB_ENDPOINT_XFER_INT` = interrupt;
> 0x01 = isochronous; 0x02 = bulk). Verified against: (1) the live
> device descriptor on Linux, (2) the kernel's own parse
> (`/sys/kernel/debug/usb/devices`: interface 5 shows `Atr=03(Int.)`),
> (3) a manual byte-by-byte decode of the device's configuration
> descriptor inside cap_coldplug.pcap:
>
> | Interface | Alt | Endpoints | bmAttributes | wMaxPacketSize |
> |---|---|---|---|---|
> | 0 | 0 | `0x03` OUT / `0x84` IN | `0x01` **isochronous** | 0 / 0 |
> | 0 | 1 | `0x03` OUT / `0x84` IN | `0x01` isochronous | 420 / 396 |
> | 0 | 2 | `0x03` OUT / `0x84` IN | `0x01` isochronous | 572 / 524 |
> | 0 | 3 | `0x03` OUT / `0x84` IN | `0x01` isochronous | 900 / 804 |
> | 5 | 0 | `0x01` OUT / `0x82` IN | `0x03` **interrupt** | 64 / 64 |
> | 5 | 1 | `0x01` OUT / `0x82` IN | `0x03` interrupt | 448 / 448 |
> | 5 | 2 | `0x01` OUT / `0x82` IN | `0x03` interrupt | 640 / 640 |
> | 5 | 3 | `0x01` OUT / `0x82` IN | `0x03` interrupt | 1024 / 1024 |
>
> cap_audio.pcap is 25718× `URB_INTERRUPT`, **zero** `URB_ISOCHRONOUS`:
> the Windows driver streams interrupt transfers on `0x01`/`0x82`
> (cap_audio.pcap), and the Linux kernel rejects ISO URBs on those
> endpoints with `EINVAL`. Interface 0's ISO pair saw zero packets in
> the captures (it is a different, unused function).

### Frame format

Each URB = **256 frames × 56 bytes**. Each frame = 14 channels × 32 bits,
verified on hardware 2026-08-22 (`tools/usbdump/framedump.c`):

```
frame n : [ch0][ch1][ch2][ch3][ch4][ch5][ch6..ch13]
          bytes: 00 | 24-bit audio LE (bytes 1-3)   | 20 00 00 00 x2 | zeros
```

- **Byte 0 of every audio word is 0x00; the 24-bit sample lives in
  bytes 1-3 little-endian.** Recover it with an arithmetic shift:
  `sample = (int32)word >> 8` (the u32 is already sign-extended — byte
  3 = 0xFF for negatives). The earlier ">> 16" reading was wrong.
- **USBPcap payload offset (tooling — CRITICAL, 2026-08-26)**: the
  USBPcap pseudoheader's `header_len` field (u16@0) VARIES: **28 B for
  control records, 27 B for audio URBs** (`tools/usbdump/hlen_check.py`
  on cap_lbph: ctrl hlen=28, in/out-audio hlen=27). Audio payloads
  always start at `pkt[header_len:]` — the old hard-coded `pkt[28:]`
  misaligned every audio frame by 1 byte in the analysis tools (now
  fixed in lbcap_levels/lbcap_ph/lbcap_timeline/lbcap_bitexact/
  frame_dump/frame_full/calib_analog; `lbcap_ab.py`/`in_words.py` use
  the aligned grid). Control setups follow the 28-byte ctrl header
  (`pkt[28:36]` — parse_usbcap.py/vendor_writes.py are correct).
  Linux raw-URB tools (framedump.c, the kernel driver) have no such
  header and were always aligned.
- **ch4/ch5 = `20 00 00 00`** (0x20 in byte 0) — a fixed frame marker
  (the earlier "0x20000000 / constant 32" note was a byte-order
  misread).
- **IN**: record stream channel map (aligned grid, cap_lbcal/cap_lbph/
  cap_audio 2026-08-26): **w0/1 = the AN1/2 record bus, w2/3 = the
  PH3/4 record bus** (the Instr 3/4 inputs at idle; the PH3/4 output
  bus when its loopback is on — the loopback taps the output bus into
  the record words of the same channel pair, wIdx = 2·out), w4/5 = the
  fixed marker (NOT meters), w6-13 = ADAT/SPDIF record (0 when
  disconnected). The mic AN1/AN2 live test (ch0 = AN1, ch1 = AN2,
  clipped peaks) is consistent; the earlier "ch2/ch3 = AN3/AN4" note
  was read at the misaligned offset — superseded by cap_lbph (w2/3 =
  the Phones bus, identical L/R).
- **OUT**: ch0/1 = the playback audio (identical on both = duplicated
  mono), ch2-13 = 0 (no signal routed to PB3-12).
- The 24-bit values follow the audio sample by sample (fast variation) —
  this is NOT a meter stream.
- **WDM device → playback pair map (user-confirmed 2026-08-24, WDM
  Devices = 3)**: Windows exposes one stereo device per playback pair,
  and the pair index = the OUT frame channels: **"Speakers" (a.k.a.
  "Analog (1+2)") = PB1 = ch0/1**, "Analog (3+4)" = PB2 = ch2/3,
  "SPDIF/ADAT (1+2)" = PB3 = ch4/5. The user's setup plays the
  "Speakers" device through the PH3/4 headphones because TotalMix
  routes PB1 into PH 3/4 — the routing is mixer-side, the WDM→channel
  map is fixed. For the Linux ALSA plugin: ch0/1 is the default
  playback pair (the "Speakers" device).

## Scene load (cap_scene.pcap) — full mixer state dump

Loading a TotalMix scene writes the COMPLETE mixer state as a burst:

1. Crosspoints — **both maps written together**: the standard map
   (`0x0034+0x0034·out+idx`) AND the low map (`0x0000+idx`) for the same
   crosspoints, same values. So the low map is a second write of the
   AN1/2 submix crosspoints, always kept in sync.
2. 8-bit output masters (`0x1A` on `0x0004+2·out`) with the unmute value
   `0x00F3`, and 16-bit masters (`0x12` on `0x03E0+2·out`).
3. Preamp state (`0x17` wIdx=0x003F) + the four gain writes
   (`0x1A` on `0x0000-0x0003`).
4. Crosspoints for the remaining outputs (e.g. `0x0138/0x0152` = out 5
   ADAT7/8 — **base 0x0138 confirmed**), playbacks 3-6 at idx 16-23
   (`0x0044/0x005F`, `0x0046/0x0061`, `0x0048/0x0063`, `0x004A/0x0065`
   into AN1/2 — **playback indices confirmed**).
5. The keepalive cycle `[0x10, 0x1D, 0x14] → 0x13`.

Interesting: a single fader gesture during the load produced BOTH the
low-map and standard-map writes at the same value (0x0000 for muted,
0x0243 for the AN1→AN1/2 fader at -20 dB…). The two maps are always in
sync; writing either (or both) is safe.

**EQ/FX = bulk OUT ep 0x0A (resolved by cap_eq.pcap timestamps).** A
live EQ/reverb toggle on the mic input (sound audibly changed) produced
NO vendor-control writes, but dense bursts of the 64-byte blocks below
(17 blocks at +11.95 s, 150+ blocks at +14.96-18.87 s) — while a later
fader drag (vendor writes at +29-34 s) produced NO bulk blocks. So:

- Mixer commands (faders, mutes, gains…) = vendor control writes.
- **EQ/FX parameters = 64-byte coefficient uploads on bulk ep 0x0A**
  (5 × 32-bit values at 0x20 + footer 0x38 = 00 00 00 04 00 00 00 00).
- To fully decode: set EXACT EQ values (band gain +6 dB, pause for the
  ramp to settle, then -6 dB…) and read the settled coefficients.

### Bulk OUT ep 0x0A (64-byte blocks) — DSP coefficient stream

64-byte blocks on ep 0x0A with the layout:

```
offset 0x00 : 00 00 00 80
offset 0x14 : 4 × 32-bit values (smoothly progressing during changes)
offset 0x34 : 1 × 32-bit value (same)
offset 0x38 : 00 00 00 04 00 00 00 00
```

The 5 values at 0x14-0x23 + 0x34 progress monotonically during parameter
changes — DSP coefficient/state interpolation. After each change the
stream ends with an all-zero block (`00000000 ×4 + 00000008`) and stops
until the next change. The values look like Q31 fixed point (±0.24 max
observed). **Exact meaning not decoded yet**: they do not directly match
standard biquad coefficients for the tested EQ settings, and every ramp
settles to zero — possibly the DSP's internal slew/ramp state rather
than the final coefficients. For TuxMix: the transport is mapped, but
full EQ control needs either more analysis or a byte-replay approach.

## VU meters: conclusion

**There is no separate VU-meter register/endpoint.** TotalMix computes
its meters host-side:

- **Input meters**: level computed from the received audio (ISO IN ep 0x82).
- **Playback meters**: level computed from the sent audio (ISO OUT ep 0x01).
- **Output meters**: sum of routed sources × faders (state known to the
  host).

For TuxMix: reading the ISO streams (IN + OUT) and computing per-channel
levels is enough to display VU meters. The bulk-480 fields (q60/qE0,
p170, counter c) are DSP telemetry (buffer positions, queue depth) and
do not change in a "meter" fashion.

**CONFIRMED with live audio (cap_vu.pcap, 2026-08-24)**: music playing
at a constant -4.8 dBFS on OUT ch0/1 (PB1) while the candidate words
w92/w108 grow monotonically 0.4M → 43M (accumulators, not meters) —
the audio RMS stays flat while the telemetry counts up. The telemetry
has no output-level fields; host-side meter computation is the only
way (and what TotalMix itself does).

## Open gap: firmware never marks the stream "active" (blocks 48V + real audio)

**RESOLVED 2026-08-22 (late): the blocker was a SWAPPED 48V mapping, not
the session state.** The 48V LED physically engages with `0x17 0x000D
0x003F` + `0x21` (ON) / `0x000C` (OFF), with or without a stream. The
whole "session must be valid / byte 2 must reach 0x80" investigation
below was chasing the wrong value (`0x000C` = OFF was being written as
"ON"). The byte-2 states (0x40/0x41/0x43/0x4F/0x80) and the never-0x80
observation remain real and documented in the "Fresh state" section,
but they do NOT gate 48V. Keep the historical analysis below for
context; see the "48V (phantom)" section for the corrected mapping.

**Status as of 2026-08-22 (second Linux retest):** the software stack
works end-to-end — vendor-request writes, real isochronous transfers
via a `libusb1-sys` FFI layer (`tuxmix-usb`'s `device/iso.rs`, needed
because neither `rusb` nor `nusb` 0.2.7 submit ISO transfers) — but the
device's own status readback never reports streaming as active, and 48V
phantom power never physically engages on AN1, even though the preamp
register write is acknowledged and echoed back correctly.

The endpoints are NOT the problem anymore (see the ISO section above —
interface 5 / ep `0x01`/`0x82` / alt 1 is confirmed by the device's own
captured descriptor). What remains is the **session-start handshake**:
something TotalMix's driver does that we don't reproduce yet.

### What the Windows driver does at session start (cap_coldplug.pcap)

Two things were added to the Linux port to mirror the Windows driver:

1. **Correct endpoints**: the audio ISO endpoints are **`0x01` OUT /
   `0x82` IN on interface 5** (bmAttributes 0x03, isochronous — see the
   ISO section above; the captured descriptor is authoritative).
   Interface 0's `0x03`/`0x84` are **interrupt** endpoints; ISO
   transfers on them complete without USB errors but the device never
   treats the session as valid.
2. **Init sequence + alt-setting**: the driver selects **alt-setting
   1** on interface 5 (`SET_INTERFACE wVal=1 wIdx=5`, 448-byte packets,
   capture records #43/44) and sends a config burst right after:

```
bReq=0x16 x57  wValue=0x0000  wIndex=0x0000..0x001D, 0x0020..0x003D  (reg clear)
bReq=0x1B      wValue=0xC350  wIndex=0x0000   (clock setup — values
bReq=0x1B      wValue=0x8DB8  wIndex=0xD201    captured at 48 kHz,
 bReq=0x1B      wValue=0x8234  wIndex=0xD302    likely change with the
 bReq=0x1B      wValue=0x7CFF  wIndex=0xF803    sample rate)
bReq=0x10      wValue=0x0021  wIndex=0x05FF
bReq=0x17      wValue=0x000C/0x000D  wIndex=0x0000   (preamp, wIdx=0 here)
bReq=0x21      wValue=0x0000  wIndex=0x0000
bReq=0x10      wValue=0x0000  wIndex=0x3000   x2
bReq=0x10      wValue=0x0800  wIndex=0x0800   x3
```

The 48V interlock is **latched**: once a valid stream has run, the LED
stays controllable even with the ISO URBs paused. `device.rs` now
reproduces the whole sequence (`start_streaming`).

### Other cold-start observations

- State restore begins with a burst of **`bReq=0x15` writes** (30×,
  `wValue=0`, `wIndex=0x0000-0x001D`) — a register clear.
- Two new `0x17` uses: `0x17 wVal=0 wIdx=0x8080` (right before the
  preamp block) and `0x17 wVal=0 wIdx=0xF040` (right after). Role
  unknown; TotalMix sends them around preamp/state writes.
- The 48V toggle itself uses the known `0x17 0x0C/0x0D 0x003F` +
  `0x21` pattern (no extra writes at the LED-on moment).

### To verify

Retest on Linux with the corrected endpoints (`hold48v_iso` example):
the streaming bit should go active and the 48V LED should physically
engage. **The retest as of 2026-08-22 was reported as still failing**
(streaming never active, 48V LED never on) — but the exact `hold48v_iso`
output was not saved to the repo, and it is not certain the retest ran
with the interface-5 code (the repo copy on Linux may have predated the
fix). Redo the retest and **save the full output** (see LINUX-TEST.md).

**Diagnostic caveat — the "streaming bit" mapping is unverified.** The
current code reads 0x17 **bit 1 of byte 0** as the streaming flag, but
captures of TotalMix actively streaming show byte 0 = `0x0C` (bit 1
clear) while other bytes change (`0C 01 41 40` vs idle `0C 01 80 40`).
The bit may live in byte 1/2 or change with the 48V state. Before
trusting "streaming=active" from 0x17, do a **controlled capture**: one
session, TotalMix streaming, and compare the 0x17 readback with the
stream running vs stopped. The **front-panel 48V LED is the ground
truth** for whether a session is valid.

**"TotalMix presence" theory (user) — not supported.** The device is a
USB peripheral; it cannot detect whether TotalMix is running, only the
host requests it receives. Re-plugging on Windows with TotalMix open
lights the LEDs because `firefaceu` (the kernel driver) re-initializes
the device — expected behavior, not evidence that TotalMix itself is
required. If the Linux stream is invalid, the difference is in the host
requests, not in TotalMix's presence.

**Confirmed on real hardware (this session — interrupt transport):**

- The `0x17`+`0x21` preamp write/commit sequence is correct and in
  order (matches captured TotalMix traffic byte-for-byte).
- **Interrupt transfers on interface 5 / alt 1 (`0x01`/`0x82`, 448-B
  URBs) complete with zero errors, sustained**, and the IN stream
  carries the 14×32-bit frame format decoded above (ch0/ch1 = AN1/AN2
  with live noise, ch4/5 = constant 32). This is the real audio stream.
- The device only services the endpoints while **both** endpoints have
  a pending URB (a lone OUT URB never completes; adding the IN URB
  makes both complete within ~1 ms) — so the streams must be submitted
  and resubmitted as a pair (raw async libusb; rusb's synchronous
  per-URB API cannot express this and times out).
- 14336-byte single interrupt URBs never complete; 448-B (one
  `wMaxPacketSize`) URBs do.
- Submitting ISO URBs on interface 5's endpoints fails with `EINVAL`
  in the kernel (they are interrupt endpoints) — the earlier "ISO on
  the right pair" retests could never have worked.
- Interface 0's isochronous pair (`0x03`/`0x84`) submits fine but the
  device ignores it (no audio, no 48V engagement) — it is not the
  audio stream (zero packets there in the captures).
- Order of 48V vs stream doesn't matter; the keepalive trigger
  (`0x10 0x0000 0x8000` + `0x1D 0x0000 0x0000` — the first two
  requests of `keepalive()`) precedes every stream restart in
  cap_audio.pcap (frames 5807/5809, 8405/8407) and is sent before the
  URBs by `start_streaming`.

**Still open:**

1. ~~Sample-rate/clock state~~ — RESOLVED (see "Sample rate / clock"):
   a mid-session rate change is SET_INTERFACE(5, alt) only; the alt
   table for all 9 rates is complete. The `0x1B` init block stays as
   captured (48 kHz, cold start).
2. ~~Whether the 48V LED physically engages~~ — RESOLVED (hardware):
   the P48 LED follows the toggle; 48V works with or without a stream.
3. **The `0x16`/`0x15` register-clear ranges and the `0x1C` role**
   (written once in the init burst, then polled in the status cycle).
4. ~~Streaming at higher sample rates~~ — RESOLVED: alt 1 = 32/44.1/48/
   64/88.2, alt 2 = 96/128, alt 3 = 176.4/192 kHz (cap_rates2.pcap).
   Note: byte 2 = 0x80 (long interpreted as "streaming/session valid")
   is actually the CLOCK NO-LOCK state — see "Clock source / no-lock
   state" below.

Guessing further on live hardware has diminishing returns; needs a
**targeted fresh Windows capture** (start streaming in TotalMix while
capturing, with 48V toggling and the sample rate changed, and compare
with the current implementation):

## Fresh state / byte-2 counter / 48V gate (2026-08-22, hardware-verified)

**A true power cycle (unplug ~10 s) on the Linux test unit gave the
device's FRESH state: `0x17 = 0C 01 4F 00`.** Byte 2 = 0x4F, byte 3 =
0x00. The init write `0x10 0x0021 0x05FF` flips byte 3 to 0x40 and it
stays (that is the always-0x40 byte 3 of every TotalMix read). Byte 2
stays 0x4F through the ENTIRE session (init + trigger + URBs + arm +
48V) — it never becomes 0x80.

### Byte 2 bits 1-3 = a 3-bit right-shifting counter

Observed values across all captures + this unit:

| value | bits 1-3 | seen in |
|---|---|---|
| 0x80 | bit 7 set | TotalMix steady (cap_audio etc.) |
| 0x41 | 000 | Windows idle after init (coldplug) AND coldstart-with-stream |
| 0x43 | 001 | our device after old sessions (DIM LED lit) |
| 0x47 | 011 | our device after an all-interfaces claim test |
| 0x4F | 111 | our device fresh power-on |

111 → 011 → 001 → 000: each step clears the highest remaining bit (a
shift right). Windows' device is always at 000 (0x41) or 0x80.
**What decrements the counter is unknown.** SET_INTERFACE, alt-settings,
per-interface claims, init writes and rate codes do NOT move it (one
un-reproduced 0x4F→0x47 shift happened around a claim-all-interfaces
test). Only a power cycle resets it to 111.

### cap_coldstart contradicts the "0x80 required for 48V" theory

cap_coldstart.pcap (TotalMix cold start, stream already running, 48V
toggled ×204) reads `0c/0d 00 41 40` the WHOLE time — byte 2 = 0x41
with a running stream and 48V toggles. So 0x80 is NOT the 48V gate
(48V works with or without it) — and cap_clk.pcap (2026-08-22) shows
0x80 is the CLOCK NO-LOCK state (Optical clock source, no lock).
Byte 1 = 0x00 there vs 0x01 everywhere else (early driver state).
The old "0x80 = session/stream valid" reading was wrong — the byte-2
bit-7 semantics are: 0x80 = clock source not locked.

### Other verified facts (2026-08-22)

- **0x17 readback byte 0 = the PREAMP state register (NEW decode
  2026-08-23)**: byte 0 mirrors the 0x17 write state (base 0x0C,
  bit0/1 = 48V AN1/2, bit4/5 = PAD AN1/2). cap_padpan.pcap shows
  `0C 01 40 40` (all off), `0D 01 40 40` (48V AN1), `1D 01 40 40`
  (48V+PAD AN1) — the old "byte 0 = streaming-ish state" note was
  wrong. The preamp state **persists across power cycles** (48V/gain
  survived a reboot) and the init burst's `0x17 0x000C 0x0000` write
  does NOT clear it (verified on hardware 2026-08-23: 0x17 reads
  0x0D before and after the full init+stream). This gives TuxMix a
  real "get": `BabyfaceProUsb::open()` reads 0x17 and syncs the
  phantom/PAD UI state. Gains have NO readback (no 0x1A/0x12 reads
  exist).

- The device streams at exactly 48.0 kHz with our URBs (187.5
  completions/s of 14336 B) — rate config is NOT the blocker.
- IN frames: ch0/ch1 = live 24-bit audio (noise floor from the
  unconnected/AN1 inputs), ch4/ch5 = constant 0x20 (the "32" marker,
  low byte; the earlier "0x20000000" note was a byte-order misread).
- The 9-byte status packets on ep 0x82 (always flowing on Windows'
  device) NEVER flow on our device (0 completions with 9-B URBs after
  a full init). Interleaved with audio in cap_audio (~every 7-8 ms).
- 0x1B rate codes (0xC350 48k, 0xB372 44.1k-derived, 0x86A0 96k,
  0x66E3 88.2k) change NEITHER byte 2 NOR the 0x10/0x19 reads.
  0x10 = `21 48 00 00` / 0x19 = `44 DC 00 00` after init are static.
- `0x1C 0x0000 0x0000` WRITE in our init is a misread: cap_coldplug
  frame #173 is a 0x1C READ (`bm=0xC0 wLen=4`). Init order corrected:
  0x16 clears → 0x1B ×4 → [0x1C read] → 0x10 0x0021 0x05FF → 0x17
  0x000C 0x0000 → 0x21 → 0x10 0x3000 ×2 → 0x10 0x0800 0x0800 ×3.
- Session stop = `0x13 0xC000` only; 0x17 reads 0x80 right up to the
  stop — the bit is sticky once set.
- The 0x10 family in every session capture is ONLY `0x10 0x0000 0x8000`
  (trigger) + 0x1D + URBs + 0x14 0xC000. No other 0x10/0x1C/0x1A writes
  in steady state.
- Fresh state: reg 10 = `10 40 00 00`, reg 19 = `44 00 00 00`, regs
  11/1B/1D byte 3 = 0x01 (vs 0x02 after our init). 0x1C/1E/1F match
  TotalMix in every state.

### Windows capture checklist (the 0x41 → 0x80 transition has NEVER been captured)

1. **Cold plug full**: start USBPcap → unplug + replug the Babyface →
   wait for TotalMix to start a session → toggle 48V → stop. Must
   contain: fresh state, init, the FIRST session start (0x41→0x80),
   and the 48V writes inside a valid session.
2. **Rate change**: in TotalMix → Settings, switch sample rate
   (44.1/48/88.2/96/176.4/192k) while capturing → real 0x1B values per
   rate + the rate-change write pattern.
3. **No-capture bonus**: the user can read the sample rate + clock
   source from TotalMix's toolbar — record it for the init comparison.
4. Also record whether the fresh power-on state on the Windows unit is
   0x4F too (confirms the counter is normal device behavior) and which
   events decrement it.

### Session persistence — how the card / host restores settings (2026-08-23)

**What the CARD stores (non-volatile):** the analog preamp state —
48V/PAD bits in the 0x17 readback byte 0 survive a power cycle and the
init burst's `0x17 0x000C 0x0000` write does NOT clear them (hardware-
verified 2026-08-23). Gains may also be stored in the device EEPROM
("48V/gain survived a reboot"), but there is NO gain readback so the
card's gain state is invisible to the host — treat as unknown/0.

**What the card does NOT store:** the DSP mixer (faders 0x12,
crosspoints, routing, pitch) — no readback paths exist (only
0x11/0x17/0x1C/0x1E/0x1F reads). The host must re-upload it.

**How TotalMix/firefaceu restores (host-side):** registry Device
Parameters (Frequency, mic gains, 48V, PAD...) + TotalMix workspace
files. On connect/start the driver re-uploads: coldplug init burst
(0x16 clears + 0x1B clock quads) then the full 4-bank register file
(the big 0x1B uploads in cap_fus when the app re-syncs) and scene
loads (cap_scene = full mixer dump).

**How TuxMix does it:** auto-save/restore of the mixer to
`~/.local/share/tuxmix/scenes/auto.json` (TotalMix-style), and
`BabyfaceProUsb::open()` reads 0x17 at open to sync the 48V/PAD state
that the card does remember. Verified: gain 36 dB + 48V set in the GUI
were restored by the TUI at open.

**Open:** (1) does the card ALSO store the gains in EEPROM? Test on
Linux: set gain raw 17, power-cycle the CARD (unplug ~10 s), replug,
start the stream, measure the IN level WITHOUT re-writing the gain —
same level = card stored it. (2) The exact TotalMix session-restore
write ORDER (init → clock → mixer dump) — capture: set a mixer state,
quit TotalMix, relaunch, capture the re-apply.

### ~~Session watchdog~~ — RESOLVED 2026-08-25: measurement artifact

The suspected “7.5 s session death” was the END OF THE TEST TONE FILES:
aplay stops when the file ends, the stream closes, the loopback goes
silent. 8-s tones “died” at 7.5 s, 30-s tones at 29.5 s — the same
“level gate” was just the file lengths. A 60-s tone survives the full
60 s with a stable loopback, no keepalives, no status reads, no extra
traffic (hardware-verified 2026-08-25). **There is NO session watchdog;
the kernel driver streams continuously.** The Windows captures
(cap_watchdog / cap_watchdog2, 6 min + 2.5 min, USBPcap, all endpoints)
were analysed and their traffic (5-register status poll ~50 cycles/s,
bulk ep 0x85 480 B ~45/s, 14336-B interrupt URBs, readback values
0x17=0x8A/0x11=400e0f02) matches Linux exactly — session segments
survived 182 s and 137 s, same as Linux.

**TotalMix session-restore capture (cap_restore2.pcap, 2026-08-23) —
RESOLVED**: a TotalMix relaunch (device NOT replugged, firefaceu still
loaded) re-uploads the MIXER only:

1. `0x15` register clears (0x0000-0x001D)
2. `0x17 0x0000 0x8080`
3. per-output blocks: master `0x12 0x2000 0x03EX` (0 dB) + unmute
   `0x1A 0x00F3 0x0004-0x0009` + all crosspoints (the snapshot state;
   default = PB1-6 → AN1/2 at 0x2000, hardware inputs at 0x0000)
4. `0x17 0x000D 0x003F` + `0x21` — the 48V state (from the card's
   EEPROM persistence, NOT a host re-apply)
5. `0x17 0x0000 0xF040`

Key observations: the restored mixer = TotalMix's last SAVED snapshot
(slot Mix 1) — unsaved changes are lost on kill; the gains are NOT
re-written (no 0x1A gain writes; the device keeps its last-written
registers); NO 0x1B clock quad (clock is programmed only at coldplug);
NO keepalives in the burst. The gain DISPLAY comes from the snapshot
(host) and may differ slightly from the device's last-written value.

### Fireface USB Settings UI reference (user-provided 2026-08-23)

Tab 1 "Babyface Pro":
- **Buffer Size** dropdown: 48/64/96/128/256/512/1024/2048 samples;
  Errors label (e.g. "Errors: 0/0")
- **Options**: DSP "EQ for Record" checkbox; **Optical Out** dropdown
  (SPDIF/ADAT); **SPDIF In** "TMS" checkbox; **WDM Devices** dropdown
  (0-6)
- **Clock Mode**: **Sample Rate** dropdown (32/44.1/48/64/88.2/96/128/
  176.4/192 kHz); **Clock Source** dropdown (Optical In / Internal);
  Current label (e.g. "Current Internal"); **Pitch** dropdown
  (+4%/+0.1%/0%/-0.1%/-4%) + slider -5%..+5% with 0.000% precision
  (the dropdown's 5 positions = the discrete quads captured; the
  slider = continuous 16.8 fixed-point DDS)
- **Input Status**: Optical label (e.g. "No Lock --")

Tab 2 "About": driver date 09.07.25, version 1.276, HW rev 322;
**Lock Registry** checkbox (settings persistence-related — unknown
mechanism); "Enable MMCSS for ASIO" checkbox; "Sort ASIO devices"
dropdown; Preview button. (MMCSS/ASIO-sort are Windows-only.)

### Front-panel IN/OUT buttons — state is HOST-VISIBLE (cap_buttons.pcap, 2026-08-23)

The front-panel channel-selection buttons are NOT device-local: their
state appears in the `0x17` readback (and the host polls it).

**IN button** (cycles input pair Ch1/2 → Ch3/4 → Opt → …):

| 0x17 byte 2 | Panel input selection |
|---|---|
| 0x40 | Ch 1/2 (top) |
| 0x50 | Ch 3/4 (middle) |
| 0x60 | Opt (bottom) |

Byte 1 stays 0x01; each press = one step (6 presses = 2 full cycles).

**OUT button** (cycles output pair Ch1/2 → Phones → Opt → …): byte 2
goes to **0x80 (sticky)** and **byte 1 cycles 01 → 02 → 00** (Ch1/2 →
Phones → Opt), while the HOST sends `0x17 0x0000 0x2000` polls (~0.75 s)
— the previously-unmapped **`0x17 wIdx=0x2000` = the panel-state poll**.

So the front panel is no longer a blind spot: TuxMix can read which
channel pair the panel controls via 0x17 (and can replicate the poll
write). Wheel / A SET / MIX B / SELECT / DIM still uncaptured.

### Front-panel gain / fader / A SET — the host is in the loop (cap_panel.pcap, 2026-08-23)

The panel is NOT device-local for gain/fader: the HOST writes the
registers in response to panel events (the device reflects the panel
state in the 0x17 readback and the driver translates it):

- **Wheel (gain mode, input selected)**: `0x1A` wIdx `0x000A` — raw
  10/11/12 observed (the selected channel's gain). NOTE: TotalMix
  writes the AN1 gain to `0x1A wIdx 0x0000` — 0x000A is a SECOND
  gain address (unidentified — maybe the analog/ADC gain, registry
  "ADC Gain").
- **MIX + wheel (fader mode)**: `0x12` on the LOW MAP `0x0000+idx` /
  `0x001A+idx` (AN1→AN1/2), values 0x0970→0x0E47 = ~0.5-1 dB steps
  (the user: ±0.5 dB in +6..-6, then ~0.8 dB below -6 — to verify).
- **0x17 panel-state writes**: `0x17 0x0480 0x8000` and
  `0x17 0x8480 0x8C80` (the previously-unmapped 0x8000/0x8C80
  family) — the panel/SELECT state.
- **0x17 readback during panel use**: byte0/byte1 gain a 0x80 bit
  (0x0C→0x8C, 0x04→0x84) while the panel is engaged; byte 2 = the
  wheel step counter (0x40+n in gain mode, 0x00+n in fader mode);
  SELECT presses flash byte 3 (0x42/0x50 transients).
- **A SET** = phantom power **STANDALONE-ONLY** per the manual (§5.1):
  "press SET to activate phantom power (only in standalone mode)".
  In online mode SET only stores the Recall volume (hold 2 s with OUT
  selected). This explains why the P48 LED never toggled in captures —
  no standalone capture exists yet, so the 48V-via-panel write is still
  unknown.

### Front-panel behavior per the official manual (manual.txt §5, 2026-08-23)

`tools/usbdump/manual.txt` = clean text extraction of the RME manual
(copied from the user's `~/Downloads/bface_pro_fs_e.txt`, 197 KB —
much better than any in-repo extractor). Front-panel facts to verify
by capture where marked:

- **IN** cycles input pairs 1/2 → 3/4 → Opt; **SELECT** steps left →
  right → both (flashing LEDs) ✓ already captured (0x17 byte2).
- **Wheel + IN selected** = analog gain (per-channel via SELECT).
- **SET** = phantom power **only in standalone mode** (§5.1). In
  online mode: SET (hold 2 s, OUT selected) = **store Recall volume**
  (host-side only — NO USB write, confirmed cap_dim.pcap).
- **OUT** cycles outputs 1/2 → Phones → Opt ✓ already captured
  (byte1 01→02→00). **Wheel** = output level (moves TotalMix fader in
  sync). **Hold SELECT + wheel** = output balance (= PAN of the stereo
  hardware output in TotalMix — matches the captured pan writes).
- **DIM** = Dim on the Main Out as defined in TotalMix; LEDs only lit
  when the Main Out is the selected OUT. DIM is also a configurable
  hotkey (Speaker B, Talkback…). Hold **DIM 2 s = Recall** (loads the
  saved Main volume).

### DIM semantics — the USB writes (cap_dim2.pcap, 2026-08-26)

Every DIM press is a host-side burst on the **Phones** output (out 1 =
BF_REG_MASTER_8 0x0006/7, BF_REG_MASTER_16 0x03e2/3):

| action | 8-bit (0x1a, 0x0006/7) | 16-bit (0x12, 0x03e2/3) | flag 0x17 wIdx=0x2000 |
|---|---|---|---|
| DIM ON | 0xCB | 0x0333 | wVal=0x2000 |
| DIM OFF | restore pre-DIM | restore pre-DIM | wVal=0x0000 |

- **DIM = an absolute −20 dB** on the Phones master (0xF3 − 0xCB =
  40 × 0.5 dB; 16-bit 0x0333 ≈ −20 dB), **independent of the current
  level** — verified with pre-DIM at 0 dB (0xF3) and −40 dB (0xA3):
  DIM always writes 0xCB/0x0333 (and would make a −40 dB monitor
  LOUDER, so it is a fixed level, not a floor).
- The pre-DIM level is kept host-side and restored on DIM OFF — the
  driver needs the same two-value cache.
- The 0x17 wVal=0x2000 wIdx=0x2000 write is the DIM flag (likely the
  unit's DIM LED); the flag is cleared with wVal=0x0000.
- DIM only ever touched the Phones (out 1) in this capture — the
  monitor output, matching "Dim on the Main Out".  The Linux driver
  currently has no DIM control ("DIM doesn't dim" report).

### System volume — HOST-SIDE stream gain, zero USB writes (cap_sysvol2.pcap, 2026-08-26)

**The Windows "Speakers" volume writes NOTHING to the device.**  The
controlled capture (60 s tone, volume 100%→0%→100%) has ZERO control-
transfer records while the OUT ep 0x01 amplitude ramps 0.250 → 0.0004
→ 0.250 (smooth host-side fader ramp — re-extract with
`sysvol_amp.py`).  The 0x03E0/0x0004 AN1/2-master writes at the END of
cap_sysvol were the USER's own manual fader move (user-confirmed), NOT
the slider.  The "volume = AN1/2 output master" and "volume = PB
playback fader" readings both conflated that move with the slider.
So the system volume is a per-WDM-device HOST-SIDE stream gain; the
mixer stays untouched (the mic monitored through the same output
keeps its level).  Kernel model: PipeWire SOFTWARE volume, NOT the
Phones master (KERNEL-DRIVER.md "System-volume model — CORRECTED
AGAIN").
- **MIX** = direct-monitoring: OUT selects the destination, IN the
  source, MIX starts it (input LEDs flash), SELECT = L/R/both, wheel =
  monitoring level. Only covers ADAT ch 1/2; no pan.
- **SET with Opt selected** = switch optical output SPDIF ↔ ADAT (8 vs
  2 LEDs), SELECT toggles.
- **LED brightness 25/50/100%**: hold SELECT + press IN.
- **PC/CC mode toggle**: hold SELECT + DIM while plugging power.

### EQ Low Cut — structure captured (cap_eq6.pcap, 2026-08-23)

Low cut ranges: **freq 20..500 Hz, slope 6..24 dB/oct** (slope NOT
captured). Low cut blocks have **header byte1 = 0x03** (vs 0x00 for
the EQ bands) and the freq lives in the **0x38 value** (which is the
constant 0x00000004 for band blocks): 0x00038180 (≈105 Hz if
ω·2^24), 0x001168FE (≈520 Hz), 0x00523FA2 — the 3rd value does not
fit a linear ω mapping (off-scale), formula TBD. The band coefficients
stay constant while the low cut freq changes.

### EQ full-range calibration — captured (cap_eq4/5.pcap, 2026-08-23)

User-confirmed ranges: **gain -20..+20 dB, freq 20 Hz..20 kHz, Q
0.7..5** (bell ↔ shelf toggle available; low cut NOT yet captured).

cap_eq5.pcap = 21 settled states with gain fixed at +6 dB (freq/Q/type
sweeps) — ALL with nonzero coefficients. Structural lead: within a
settled state the coefficients satisfy c0 ≈ -2·c1, c2 ≈ -2·c3, c4 ≈
c1 ≈ c3 (Q1.31: c0=-0.12469, c1=+0.06232…) — the whole biquad is
nearly determined by ONE value (filter structure not yet identified;
does not obviously match the RBJ peaking-EQ normalization — needs a
dedicated fit). Exact band→slot and value→(gain/freq/Q) mapping: TBD
(offline analysis from the saved pcaps).

### EQ Low Cut slope — DECODED (cap_eq7.pcap, 2026-08-23)

Low cut slope = **header byte1 = 2^n − 1** (n = poles): 0x01 = 6 dB/oct
(1 pole), 0x03 = 12 dB/oct, 0x07 = 18 dB/oct, 0x0F = 24 dB/oct
(4 poles), 0x00 = off. Freq = the 0x38 value (varies with slope too —
formula TBD). Band coefficients constant while the low cut changes.

**Verified 2026-08-23 (eq_extract.py on cap_eq7)**: the byte1 table
above reproduces exactly, and the 0x38 values at a FIXED freq across
slopes are 0x000558FF (n=1), 0x00038180 (n=2), 0x0002D3B9 (n=3),
0x00027285 (n=4) — ratios 1.525/1.000/0.806/0.698, which match the
cascaded-pole compensation model (identical 1-pole HP stages, composite
−3 dB held constant: 1.554/1.000/0.792/0.676) within ~3%. So 0x38 ≈
the individual pole frequency; the exact freq→0x38 constant is still
open (the cap_eq6 12 dB/oct freqs give 0x38180 ≈ 100 Hz and 0x1168FE ≈
500 Hz — ratio 4.97, close to the 20-500 Hz range bounds).

### EQ coefficients — structure DECODED (cap_eq3.pcap, 2026-08-23)

EQ enabled with real parameter changes (user actions: enable, Low
+6/-6/0, Low freq 200→400, Mid +6, Mid freq, High +6, disable). The
64-byte bulk 0x0A blocks carry biquad coefficients:

- Header: `00 00 00 80` (L) / `01 00 01 80` (R); after EQ disable
  `01 00 01 00` (bit 0x80 clears).
- **3 band slots**, 5 × 32-bit each: slot 1 @ 0x04-0x13 (+0x34),
  slot 2 @ 0x14-0x23 (+0x34), slot 3 @ 0x24-0x33 (+0x34) — the
  4 coeffs at the slot offset + a 5th at 0x34 = biquad
  (b0,b1,b2,a1,a2). With multiple bands active, multiple slots fill.
- Coeffs are signed 32-bit (Q1.31-ish; e.g. 0xF02D9AE5 = -0.124).
- Coefficients RAMP smoothly (DSP interpolation) after each change
  and settle during pauses (use the last block of each settled group
  — see `_eq_settled.ps1`). +6 vs -6 dB shows a c0↔c2 swap (filter
  phase). Constant `00 00 00 04` @ 0x38.
- Settled-state extraction: `_eq_settled.ps1` groups blocks by >1 s
  gaps. Exact band→slot and dB→value mapping: TBD (needs a dedicated
  analysis correlating each settled state with the user's action).

### FX send / Reverb-Echo — captured (cap_fx2.pcap, 2026-08-23)

- **FX send (AN1/2 → reverb/echo)** = `0x12` wIdx=0x0138/0x0153
  (L/R pair, same value = mono send to stereo FX), ramp 0x0000 →
  0x1000. **CALIBRATION ANCHORS (cap_fx3.pcap, 2026-08-23): 0 dB =
  0x1000, -65 dB (displays -inf) = 0x0000** — the send range is
  -65..0 dB in 0.5 dB steps (user-observed), NOT the master's
  -inf..+6. The curve between is exponential-ish but the fast sweep
  missed steps (74 of 131 writes) — a slow labeled sweep is needed
  for the exact taper. The send potard (Bus settings) and the
  on-strip slider move together (linked). Device-side mixer param.
  **Second look (2026-08-23, eq-side analysis)**: the captured values
  land on the CALIBRATED crosspoint FADER curve at clean 1 dB points
  (0x0003 = -62, 0x000B = -54, 0x001D = -46, 0x0029 = -43, 0x006D =
  -34, 0x0122 = -26, 0x055C = -15 = FADER_CURVE) — the send may just
  be the fader curve over -65..0 dB (its 0 dB = the fader's unity).
  TuxMix `set_fx_send` uses `fader_db_to_raw`; confirm the top with a
  slow sweep.
- **Reverb/echo parameters (enable, type, pre delay, room scale, low/
  high cut, smooth, width, volume) = ZERO USB writes**, even with the
  send active — the reverb/echo engine runs HOST-SIDE in TotalMix
  (manual §21.6: "Reverb and Echo are calculated on the host CPU";
  the device FPGA runs only the mixer + EQ/Low Cut). The wet signal
  returns via the FX-return channels in the OUT stream. For TuxMix:
  sends are controllable; reverb/echo must be implemented in software
  (or ignored).
- **FX channels = frame ch12/13 CONFIRMED EMPIRICALLY (cap_fx_live /
  cap_fx_off.pcap, 2026-08-24 — A/B test)**: with the reverb ON and
  sound playing, OUT ch12/13 (host→dev) = full level (the wet return)
  AND IN ch12/13 (dev→host) = full level (the dry send). Toggling the
  reverb OFF kills OUT ch12/13 to exactly 0.0000 while IN ch12/13
  stays active. So the 14-ch frame layout = AN1-4 (0-3) + AS1/2 (4-5)
  + ADAT3-8 (6-11) + **FX send IN / FX return OUT (12-13)**. The mixer
  routes the FX return into the outputs (manual: 2 invisible ASIO
  channels; the FX return routing in the mixer = still open for the
  future TuxMix reverb implementation).
- Stream session start = `0x10 0x0000 0x8000` + `0x1D` + `0x14
  0x0C00`; end = `0x13 0x0C00`.

### Loopback / Stereo split / host-side controls — cap_ctrl3.pcap (2026-08-23)

- **Loopback = NEW bReq 0x15**: per-channel flag map — `0x15`
  wIdx=channel 0-29, wVal=0x0001 (ON) / 0x0000 (OFF). AN1/2 output
  loopback = channels 0/1. (30 channels = the full channel map.)
  **OFF SEQUENCE RESOLVED (cap_loopback_off.pcap, 2026-08-23):
  TotalMix ALWAYS writes the FULL 30-channel map on every toggle** —
  ON = ch 0/1 = 0x0001 + the other 28 = 0x0000; OFF = all 30 =
  0x0000. A partial (ch 0/1-only) OFF is NOT what TotalMix sends and
  sometimes does not disengage — TuxMix must clear all 30 on OFF.
  The hardware-output strips also have the stereo button: linked
  AN1/2 = ch 0/1 together; split = AN1 (ch 0) and AN2 (ch 1) each
  with their own loopback/talkback.
- **HARDWARE-VERIFIED 2026-08-23 (loopbacktest.c / loopback3.c)** —
  direction CONFIRMED as OUTPUT→INPUT (recording path): with a 440 Hz
  tone on PB1 → AN1/2 OUT, writing `0x15 0x0001 wIdx=0` makes the tone
  appear on the IN-stream channels 0/1 (AN1/2 input) at ≈ -24 dBFS
  (the source is -18 dBFS — the loopback tap sits ~6 dB below the
  digital output level). The flag engages from channel 0 alone (ch 1
  alone does nothing). **OFF quirk**: `0x15 0x0000` on ch 0/1 alone
  sometimes does NOT clear it (stays engaged), while a full 30-channel
  clear always does; one clear via ch 1 alone also worked — the exact
  OFF sequence needs a Windows capture. The session start (0x16 init)
  resets the map (each test began clean).
- **BUTTON→CHANNEL MAP + RECORD TARGET CONFIRMED (cap_loopback2.pcap,
  2026-08-25, Windows + live music)**: per-output loopback button →
  `2×out_index` channels — AN1/2 (out0) = wIdx 0/1, PH3/4 (out1) =
  wIdx 2/3, AS1/2 = 4/5, … (the full 30-channel map is written on
  EVERY toggle, ON and OFF — ON = the pair 0x0001 + the other 28
  0x0000). With the AN1/2 loopback ON, the AN1/2 OUTPUT BUS appears on
  the record stream **IN words 0/1** (the music at full level; 0 when
  OFF — the analog input is silent). The kernel driver's Linux symptom
  (loopback ON → IN ch0/1 = digital zero) is NOT a write difference —
  TotalMix's full-30-channel map on every toggle (vs the kernel's
  wIdx 0-11 only) is the remaining discrepancy to fix. Note: words
  12/13 (the output-monitor echo seen on Linux) are 0 in this Windows
  session — the echo is state-dependent, not unconditional.
- **LOOPBACK RECORD STAGING — RESOLVED: THERE WAS NO STAGING BUG.  The
  loopback records the output bus 1:1 with the master, on Windows AND
  on Linux (2026-08-26, live-verified with a proper test tone).**
  - Windows (aligned grid, cap_lbph/cap_lbcal): the loopback bus
    tracks the 16-bit master EXACTLY — master 0x2000 → IN RMS 0.06239
    (== OUT ch0 0.06239), 0x4000 → 2×, 0x0339 → 0.1×, mute → 0
    (`lbcap_ab.py`/`lbcap_ph.py`).
  - Linux (arecord through the kernel driver, mktone.py S16 tone):
    −20 dBFS tone at master 0 dB → record **−20.0 dBFS** (clean sine,
    odd harmonics at the −70 dB noise floor); −60 dBFS tone →
    **−60.2 dBFS**.  The record follows the master 1:1 (no fixed 2^-5).
  - **The old “record = playback + master − 30.1 dB (fixed 2^-5)” law,
    the “1-bit square” record, the −49.8 dBFS readings and the kernel
    ×32 stage were ALL ARTIFACTS OF A BROKEN TEST TONE**: mktone.py
    wrote a non-standard 24-bit-in-4-bytes wav; sndfile read it as
    32-bit → the whole playback chain ran ~48 dB low, so every
    lb_sweep/loopback test played a ~−68 dBFS tone, which the device's
    record quantizer turns into a coarse square (62k, odd harmonics,
    level-independent) whose fundamental the Goertzel measured at
    ≈ −49.8.  The ×32 stage (pcm.c) was therefore REVERTED (2026-08-26)
    — the loopback needs no staging.
- **PH3/4 (Phones) record = 1:1 too** (same artifact explanation for
  the old “−3 dB residual / ±3 dB drift”).  No per-output offset.
- **WORDS 12/13 (app ch10/11)**: ZERO in the aligned Windows captures;
  on Linux they carry a CLEAN playback echo at −3 dB (the latency
  anchor for looplat/reallat — the old “fixed-gain tap at −27 dB” was
  the same broken-tone artifact).  Keep the channel map as-is.
- **Tooling lesson (2026-08-26)**: mktone.py now writes a standard S16
  wav; ALWAYS use a proper tone (S16) for level tests.  The record
  path itself was never wrong.
- **Stereo split (playback strip → 2 mono buses)**: re-writes the
  playback crosspoints into out0: stereo pair = 0x1000 (-6 dB per
  side), split-mono = alternating 0x2000 (0 dB, own side) / 0x0000
  (muted, other side) per mono channel.
- **The split TOGGLE itself = ZERO USB writes (cap_split2.pcap,
  2026-08-24)**: clicking the stereo button on a Software Playback
  strip (split → re-link ×3, user-observed) emits NO vendor writes,
  and the pan display (100% L / 100% R when split, center when
  linked) is GUI-side only. The cap_ctrl3 crosspoint rewrite happens
  on the NEXT fader move after the split, not on the toggle. So the
  split is a MODEL/UI state for TuxMix; the device writes materialize
  on fader drags (set_stereo_split writing the 0x2000/0x0000 pattern
  immediately is consistent).
- **Talkback, Mono, ext. input, Mute FX = ZERO USB writes** (control
  AND bulk) — purely host-side TotalMix processing (not device DSP).
  (ext. input needs a source assigned; user confirmed it failed.)

### EQ Mid/High bands + slot map — cap_eq8e.pcap (2026-08-23)

**SLOT MAPPING CONFIRMED**: slot 1 @ 0x04 = Low, slot 2 @ 0x14 = Mid,
slot 3 @ 0x24 = High, 5th value @ 0x34 (shared). The bands were set
in SHELF mode by the user (Mid 200/2000/8000 Hz, then High 1000/4000/
10000 Hz, +6/-6). Key facts:
- **Slots are INDEPENDENT**: groups 8-13 have Mid (0x14) constant
  while High (0x24) changes — the two-bands-simultaneous case (the
  planned 8f) is covered by this capture.
- **The biquad formula is IDENTICAL for all 3 bands**: the same
  (freq, Q, gain, type) → the same 5 coefficients regardless of the
  slot (Mid states match Low states from cap_eq5/cap_eq8c exactly:
  `AA FF 26 F0 5A 64 DA 07 86 BC 4B F0 44 A4 B5 07` appears for both).
  The slot only selects the target band.

### EQ low-cut labeled freq — cap_eq9.pcap (2026-08-23)

Low cut ON, 12 dB/oct, band +6 dB; freq 20/30/50/75/100/150/200/300/
500 Hz with pauses (10 groups; all header byte1 = 0x03, coeffs zero —
the band was at +6 but the blocks show only the low-cut slots). 0x38
values: 20 Hz→0x00038180, 30→0x0005411A, 50→0x0008BE02, 75→0x000D15DD,
100→0x001168FE, 150→0x001A0131, 200→0x002286D9, 300→0x00335B73,
500→0x0054301C. **NOTE (2026-08-24): group 6 = 100 Hz, NOT group 5**
(0x00113CCC is the leftover from the previous session — the corrected
mapping was confirmed by the exact K/c fit below and by cap_eq6, whose
0x38180 = 20 Hz and 0x1168FE = 100 Hz).

**EXACT FORMULA (2026-08-24): `0x38 = round(K·f/(1+c·f))` with
K = 11508, c = 1/11656** — all 9 labeled points to ≤0.003% (the best
least-squares fit; the handoff's 1/11666 is ~15× worse). The slope
compensation scales the POLE frequency so the composite −3 dB stays
constant (cap_eq7 measured factors, exact to ±1 LSB: 6 dB/oct ×1.5267,
12 ×1.0000, 18 ×0.8061, 24 ×0.6977 — the theoretical cascade model
1.554/1/0.792/0.676 is ~1.9-3.3% off the measured values, use the
measured ones). 0x38 = 0x04000000 + byte1 = 0x00 = low cut OFF.
Header byte1 = 2^n − 1 (n = poles): 0x01 = 6, 0x03 = 12, 0x07 = 18,
0x0F = 24 dB/oct.

### EQ labeled shelf sweep — cap_eq8d.pcap (2026-08-23)

Low band, type SHELF (toggled from bell): gain +6/0/-6 @ 200 Hz, then
freq 100/1000/10000 @ +6 dB, then Q 0.7/2/5 @ 10000 Hz(?). 9 settled
groups; 0 dB = zeros; +/- swaps c0↔c2; the high-freq shelf coeffs go
near -1.0 (0xFF3BB8CB ≈ -0.985) — clearly distinct from bell. The
(type, freq, Q, gain) dataset for the Low band is now complete.

### EQ labeled gain sweep — cap_eq8c.pcap (2026-08-23)

Low band, bell, freq 200 Hz, Q 0.7 fixed; gain sweep -20/-15/-10/-6/-3/
-1/0/+1/+3/+6/+10/+15/+20 dB with pauses. 13 settled groups:
**0 dB = ALL ZEROS** (passthrough); **negative ↔ positive gain swaps
c0↔c2** (filter phase flip); group 10 (+6 dB) == cap_eq8a group 6 ==
cap_eq8b group 4 exactly (cross-check ✓). **The (freq, Q, gain) dataset
for the Low-band bell fit is COMPLETE** (cap_eq8a + 8b + 8c).

### EQ labeled Q sweep — cap_eq8b.pcap (2026-08-23)

Low band, bell, freq 200 Hz, gain +6 dB fixed; Q sweep 0.7/1/1.5/2/3/4/5
with pauses. 11 settled groups (group 2 = the leftover 10000 Hz state
from cap_eq8a; groups 3-11 = the Q steps). **c4 @0x34 decreases
monotonically with Q** (0x081950B0 → 0x0803C552) — a clean Q probe
for the fit. Group 4 (Q=1) matches cap_eq8a group 6 EXACTLY.

### EQ labeled freq sweep — cap_eq8a.pcap (2026-08-23)

Low band, bell, Q=0.7, gain +6 dB fixed; freq sweep 50/100/200/400/800/
1600/3200/6400/10000 Hz with pauses (the labeled states the fit needs;
sample rate to confirm — assume 48 kHz). Settled groups (eq_settled.ps1):
14 groups; the freq states are the coefficient sets that progress
monotonically (c0/c2 grow toward -1.0 in Q31 as freq rises, c1/c3
shrink toward 0, c4 @0x34 grows 0x0D6734→0x0AE405BF). NOTE the exact
group→freq mapping is by ORDER (group 3 = first change = 50 Hz, etc.);
the fit should also verify against group 2 (= the previous session's
leftover Low +6 dB @ 200 Hz state: 87 3D 13 F0… = cap_eq6/7 group 1).

### EQ band biquad — FORMULA FULLY DECODED (2026-08-24, eq_biquad.md)

**The band-EQ coefficient encoding is solved.** The 5 stored words per
slot (c0..c3 @ the slot offset, c4 @ 0x34) are the RBJ cookbook biquad
in a **normalized-numerator / normalized-denominator split**:

```
H(z) = c4·(1 + c2·z⁻¹ + c3·z⁻²) / (1 + c0·z⁻¹ + c1·z⁻²)
   c0 = a1′    c1 = a2′    c2 = b1′/b0′    c3 = b2′/b0′    c4 = b0′
```

where ′ = RBJ coeffs normalized by a0, and the stored word = value ×2²⁷
(signed i32 — the ×16 that confused earlier fits is 2³¹/2²⁷). The DSP
reconstructs b0 = c4, b1 = c2·c4, b2 = c3·c4, a1 = c0, a2 = c1.

**BELL = exact RBJ peaking**: A = 10^(dB/40), w0 = 2πf/fs, α =
sin(w0)/(2Q), b0 = 1+αA, b1 = −2cos(w0), b2 = 1−αA, a0 = 1+α/A, a1 =
−2cos(w0), a2 = 1−α/A (normalize by a0). **0 dB = all-zero slot + c4
= 0x08000000** (identity). Gain sign flip = c0↔c2, c1↔c3 mirror
(= the b1′=a1′ peaking identity under the two normalizations).

**Verified to ~1 LSB (2²⁷ scale)**: cap_eq8c gain sweep — the
reconstructed response peaks/notches at EXACTLY 200 Hz with EXACTLY the
labeled gain (−20..+20 dB, ±0.000 dB), DC & Nyquist = 0 dB (true bell);
cap_eq8b Q sweep (0.7..5) — +6.000 dB @ 200 Hz for every Q; cap_eq8a
freq sweep — ~1e-7 at low freq, drifting to ~1e-2 at 10 kHz (a mild
frequency-warping detail, see eq_biquad.md "Still open"). cap_eq8d
(shelf) uses the same structure.

`tuxmix-usb` now has `protocol::eq_band_storage` + `set_eq_band` (the
RBJ bell/shelf → the 5 stored words) and `set_low_cut` (the K/c
freq formula + slope compensation), plus `BabyfaceUsb::write_eq_block`
(bulk OUT ep 0x0A on interface 1) to upload them.

### Ref level (Instr 3/4) — LABELED (cap_reflevel2.pcap, 2026-08-24)

The ref level (+4dBu/-10dBV/Boost) is encoded in the COMBINATION of
`0x17 wIdx=0x003F` AND the `0x21` value (the 0x21 is NOT always the
0x0000 "commit" — it carries data here). LABELS CONFIRMED by a
labeled re-capture (started at +4dBu, 3 clicks with 3 s pauses):

| State | 0x17 state | 0x21 |
|---|---|---|
| +4dBu | 0x000F | 0x0000 |
| -10dBV | 0x0003 | 0x0000 |
| Boost | 0x0003 | 0x0003 |

The readback byte0 mirrors the 0x17 state (0x03/0x0F). The state bits
overlap the 48V/PAD region of the shared preamp register — Instr 3/4
has no phantom, so no collision in practice; composition with an
active 48V on AN1/2 is untested (TotalMix writes the raw state).
Implemented: `set_ref_level(state, commit)` + the REF_* codes
(0=unset, 1=+4dBu, 2=-10dBV, 3=Boost).

### Stereo link / split on the AN1/2 INPUT strip — cap_an2.pcap (2026-08-23)

User sequence: split (stereo button off), AN2 48V + gain + speak,
re-link, 48V toggle, stereo-mode gain changes. Decoded:

- **The input-strip stereo button = `0x17` wIdx=0x1000**: split
  (individual buses) = 0x0000 + crosspoint rewrite (`0x12` on
  0x0138/0x0152/0x0139/0x0153); linked base = 0x0400; + bit 0x1000 =
  the AN 1>2 copy mode (0x1400). Re-link rewrites the crosspoints
  (0x1000/-6 dB per side) without touching 0x1000 again.
- **48V is per-mic in split mode** (0x000E = AN2 only = 0x0C|0x02),
  but **in stereo-linked mode TotalMix writes BOTH bits (0x000F)** —
  a single 48V toggle in stereo mode = 0x0F ↔ 0x0C.
- **Gains are ALWAYS independent registers** (`0x1A` 0x0000 = AN1,
  0x0001 = AN2) even when the strip is a linked stereo pair — the
  shared potard shows both values (AN1 above AN2) but writes both
  registers separately (observed AN1 raw 17 + AN2 raw 11
  simultaneously).
- **Why AN2 was silent on Linux (2026-08-23)**: with the strip
  stereo-linked, TuxMix must write BOTH mics' preamp (48V 0x0F +
  both gains) AND route AN2's crosspoints — a one-mic-only write or a
  missing AN2 crosspoint leaves AN2 silent even with 48V engaged.

### Trim (T) — full-range sweep captured (cap_trim.pcap, 2026-08-23, re-fit 2026-08-24)

The T-panel trim slider writes the SAME crosspoint registers as the
fader: `0x12 0x0000` (low map AN1) + `0x12 0x0034` (standard
AN1→out0). Range = 0x0000 (-∞) → 0x4000 (+6.02 dB), center 0x2000 =
0 dB — the low map follows the master fader curve 0x2000·2^(dB/6)
EXACTLY. So the trim is a second gain stage applied through the
crosspoint, not a small ±dB trim.

**LINKED-STRIP behavior (cap_trim2.pcap, 2026-08-24 — labeled)**: on
an AN1/2 strip with the stereo link ON, TotalMix writes ALL EIGHT
registers — low map 0x0000/0x001A/0x0001/0x001B (AN1 L/R + AN2 L/R)
+ standard 0x0034/0x004E/0x0035/0x004F, the SAME value within each
map. User-confirmed: the trim display "0" = low map **0x2000** (held
2.3 s), bottom = 0x0000.

**FORMULA PINNED (2026-08-24 — regression on 188 labeled pairs of
cap_trim2, consistent to 0.0002)**: the two maps carry DIFFERENT
values — the low map is the **trim alone** on the master curve
(`low_raw = 0x2000·2^(trim_dB/6)`), the standard map is the
**combined fader × trim** on the fader curve:

```
standard_raw = 0x16A0 · 10^((fader_dB + trim_dB)/20)   (= fader curve at the summed dB)
low_raw      = 0x2000 · 10^(trim_dB/20)                (master curve, trim alone)
```

Verified: 137/188 pairs at one fader position (≈ -11.8 dB that
session) give `fader_lin = std_raw/0x16A0 / (low_raw/0x2000)` =
0.256-0.257 constant; the other pairs are the fader being moved
mid-capture (distinct plateaus -13.9 → -11.8 dB). cap_trim3/4
(2026-08-24) confirmed the low map alone follows the master curve
over a full -inf..0 dB sweep. `set_trim(src, trim_raw, standard_raw)`
now takes both values; the ×27/256 placeholder is gone.

⚠️ TotalMix writes the standard map only into out0 (AN1/2) + the low
map — the trim's effect on OTHER outputs' crosspoints is never
re-emitted in the captures (they keep their fader value); the DSP
applies the trim globally, so a full TuxMix implementation should
rewrite every output the strip routes to when the trim changes.

**BANK SELECTION FOLLOWS THE ACTIVE SUBMIX (cap_trim5.pcap,
2026-08-24)**: with PH3/4 as the clicked output, a TRIM drag writes
the STANDARD map (0x0034 family) with the master-curve trim values
(top 0x2000 = the "0" display) + the LOW map with small fader-region
values; with AN1/2 active (cap_trim3/4) it wrote the LOW map only;
cap_trim2 wrote BOTH (LOW = trim, STD = combined). Same rule as the
fader (cap_lowmap trigger analysis): the map written follows the
selected output. No PH3/4 crosspoint (0x0068 family) writes appeared
— TotalMix never re-emits the other outputs' crosspoints.

### Trim / MS / AN1>2 / Ref level — captured (cap_ctrl2.pcap, 2026-08-23)

- **T/Trim (AN 1/2)**: dragging the trim knob writes the AN1
  crosspoints on BOTH maps (low 0x0000 + standard 0x0034) over the
  full range — the trim is applied through the crosspoint registers
  (same as the fader). Wide range observed (0x0DF9 → 0x001E → back).
- **MS proc toggle**: AN2 crosspoints (low 0x0001 + standard 0x0035)
  set to 0x0000 when MS engaged, restored when disengaged (0x068E →
  0x0000 → 0x068E). MS mode repurposes/mutes the "side" channel
  routing.
  **EXACT SEQUENCE from cap_ctrl2.pcap (frames 11957-14481)**: engage =
  `0x12 0x0000 0x0001` + `0x12 0x0000 0x0035` (L side only, both maps,
  flag 0x0000); disengage = `0x12 0x068E 0xC001` + `0x12 0x00B1
  0x4035` (flagged, and the two maps hold DIFFERENT values: low map
  0x068E ≈ -16 dB vs standard 0x00B1 ≈ -51 dB — the maps are NOT a
  same-value mirror for the same fader). No R-side writes, no commit.
  **Validation caveat (2026-08-23)**: the loopback and the AN 1>2 route
  inject their signals POST-crosspoint (the loopback tone ignores the
  preamp gain and the crosspoint mute), so MS proc's mute of the
  PHYSICAL AN2 crosspoints is only audible with a real signal on the
  AN2 analog input — the user's AN2 XLR delivered no signal during the
  test (mic works on AN1), so the mute could not be confirmed by ear.
  TuxMix's `set_ms_proc` also mutes the R crosspoints (0x001B/0x004F)
  for a complete cut; the capture only shows the L side.
  **cap_ms2.pcap (2026-08-26, mic on AN2, ear-confirmed) — ON/OFF
  ORDER CORRECTED**: the engage is `0x0000` to ALL FOUR AN2
  crosspoints (0x0035/0x004F standard L/R + 0x0001/0x001B low L/R),
  the disengage restores the pre-mute values (standard → 0x1000 both
  sides, low → flagged 0x8001/0x801B at 0x0004).  Ear-confirmed: MS ON
  with the mic on AN2 only cuts the side path (silence), OFF restores.
  (An earlier Linux note had the two inverted.)  The kernel driver's
  `bf_ms_put` all-0x0000 engage pattern is CORRECT — no re-check
  needed.
- **AN 1>2 toggle**: `0x17` wIdx=0x1000, bit 0x1000 = flag
  (0x0400 ↔ 0x1400). NEW register 0x1000.
  **No 0x21 commit in the capture** (frames 15463-17729), but the
  Linux probe found the OFF without a commit leaves the AN1→AN2 route
  half-live (≈ -41 dBFS on IN ch1 instead of -103) — TuxMix sends the
  commit anyway (harmless, makes the OFF apply reliably).
- **Ref level (Instr 3/4, +4dBu/-10dBV/Boost) — FULLY DECODED
  (cap_reflevel3.pcap, 2026-08-26, labeled clicks)**: written via `0x17`
  wIdx=0x003F + `0x21` commit.  The 3 states (with 48V AN1+AN2 = 0x03
  live): +4dBu = `0x17 0x000F` / `0x21 0x0000`; −10dBV = `0x17
  0x0003` / `0x21 0x0000`; Boost = `0x17 0x0003` / `0x21 0x0003`.
  **The 0x0C "base" of the preamp state is the +4dBu REF bits (bits
  2-3), not a constant and NOT a PAD** (the Instr inputs have no PAD) —
  the preamp byte = 48V (bits 0/1) + ref (bits 2/3) + PAD AN1/2 (bits
  4/5); the always-0x0C in cap_padpan was the default +4dBu.  Boost is
distinguished only by the 0x21 wVal 0x0003.
- **FX send**: cap_fxsend.pcap (2026-08-26) sweeps the send level on
  `0x0138` (L) / `0x0153` (R) with the transaction flags (0x4000/
  0x8000/0xc000) — values 0x0003 → up (a level sweep), confirming the
  driver's FX-send registers in bf_state_apply_flags.

### Phase / Width — captured (cap_ctrl.pcap, 2026-08-23, partial)

- **Phase Ø toggle** (AN 1/2 hardware input) = NEGATE the crosspoint
  coefficient (Q15 signed) on BOTH maps: `0x12 0x0000` (low map AN1)
  and `0x12 0x0034` (standard AN1→out0): 0x0EA0 → 0xF15F (=-0x0EA1),
  0x018B → 0xFE74 (=-0x018C). **Note (2026-08-23): the negation is the
  bitwise NOT of the raw value** — `!0x0EA0 = 0xF15F`, `!0x018B =
  0xFE74` — not two's-complement (which would give 0xF160/0xFE75).
  Implemented as `protocol::set_phase` (L crosspoints, both maps).
- **Width knob drag** (bipolar -1.00..+1.00, 0.00 center per user)
  writes 4 register pairs that track together: `0x00AE/0x00AF`,
  `0x00C8/0x00C9`, `0x0046/0x0047`, `0x0060/0x0061` — L+R = 0x2000
  constant (0x1FD7+0x0029, 0x1C00+0x0400, …): a balance-style
  encoding (w encoded as L=0x2000·(1+w)/2, R=0x2000·(1-w)/2 — TBD).
  The two maps (0x0046/0x0060 = the low map? 0x00AE/0x00C8 = the
  standard map?) are written in sync, like the fader maps.
  **Encoding CONFIRMED 2026-08-23** (w=0.75 → 0x1C00/0x0400 exactly,
  L+R=0x2000), but the target strip is NOT PB1: writing the 4 pairs at
  +0.75 while a stereo 440 Hz(L)/660 Hz(R) tone played on PB1 → AN1/2
  produced NO audible change (ctrl9sound2.c). **cap_width2.pcap
  (2026-08-23)**: the width knob on the AN1/2-family strips writes the
  LOW MAP instead — `0x12 0x0000/0x001A` (AN1 L/R) + `0x0001/0x001B`
  (AN2 L/R) + standard `0x0034/0x0035`, L/R sweeping in opposition
  (sum ≈ 0x2000), values 0x00D8..0x1800. So the width register ADDRESS
  depends on the strip (ADAT3/4-PB → the 0x00AE family; AN1/2 → the
  low map) — the full strip→register map is still open.
- **Width on AN1/2 (2026-08-24 re-fit)**: at rest the low map reads
  0x1000 per side (the stereo-linked -6 dB pair) and the width spreads
  L/R in opposition (L+R = 0x2000) — the current `set_width` matches.
  The standard map (0x0034/0x0035) ALSO moves, in opposition, but at
  ~1/10 scale (±4 LSB around the fader value 0x01B0 vs ±0x29 on the
  low map) — `set_width` does NOT write it (minor; add for parity).
  **cap_width3-7 (2026-08-26)**: the width register family is
  STRIP-DEPENDENT.  cap_width3 (AN1/2 strip) writes the standard map
  pair 0x0034/0x0035 + 0x004E/0x004F (with the transaction flags);
  cap_width4 (another strip) writes a DIFFERENT family: 0x001C/0x001D,
  0x0036/0x0037, 0x0050/0x0051, 0x0106/0x0107, 0x0120/0x0121 — the
  per-strip width register offsets, mapping still open (5 more
  captures to diff).

### Width strip mapping — cap_width3-7 ANALYZED (2026-08-26)

Every width drag step writes 12 registers in a fixed order: 3 families
× 4 (the strip's src pair, L first then R, both on the L-map and its
+0x1A R-map).  The value ladder is identical in all five captures:
`0x1000/0x1000` (neutral) → `0x1029/0x0fd7` → `0x1052/0x0fae` →
`0x107b/0x0f85` → `0x10a3/0x0f5c` → … — i.e. **L = 0x1000·(1+w),
R = 0x1000·(1-w) with w += 0.01 per step** (0x29 = 0x1000·0.01; the
same encoding the kernel `bf_width_put` already uses for the AN1/2 low
map).  Sum L+R = 0x2000 always.

| Capture | Strip (src pair) | std block-0 map | low map | extra map |
|---|---|---|---|---|
| cap_width3 | AN1/2 (0/1) | 0x0034/0x0035 + 0x004E/0x004F | (static near −inf) | — |
| cap_width4 | AN3/4 (2/3) | 0x0036/0x0037 + 0x0050/0x0051 | 0x0002/0x0003 + 0x001C/0x001D | block-4 std 0x0106/0x0107 + 0x0120/0x0121 (state-dependent — NOT reproduced in cap_width7) |
| cap_width5 | AS1/2 (4/5) | 0x0038/0x0039 + 0x0052/0x0053 | 0x0004/0x0005 + 0x001E/0x001F | — |
| cap_width5 | ADAT3/4 (6/7), ADAT5/6 (8/9), ADAT7/8 (10/11) | same block-0 pattern per src | same low-map pattern | — |
| cap_width6 | PB1 (12/13) | 0x0040/0x0041 + 0x005A/0x005B | 0x000C/0x000D + 0x0026/0x0027 | — (like an input: out 0 + low) |
| cap_width6 | PB2 (14/15) | 0x0042/0x0043 + 0x005C/0x005D | — | — |
| cap_width6 | PB3 (16/17) | **block-1** 0x0078/0x0079 + 0x0092/0x0093 | — | — |
| cap_width6 | PB4 (18/19) | **block-2** 0x00AE/0x00AF + 0x00C8/0x00C9 | — | — (the old "0x00AE family" mystery) |
| cap_width6 | PB5 (20/21) | **block-3** 0x00E4/0x00E5 + 0x00FE/0x00FF | — | — |
| cap_width7 | **PB6 (22/23)** | **block-4** 0x011A/0x011B + 0x0134/0x0135 | — | — |

**IDENTIFIED (2026-08-26 Windows session): cap_width7's 0x011A family
= PB6 (ADAT 7/8 playback) width → out 4 (0x0104 + 22/23 L, 0x011E +
22/23 R).**  The playback pattern is **PB_n → out(n−2)** (PB2→out0,
PB3→out1, PB4→out2, PB5→out3, PB6→out4), PB1 special (out 0 + low
map, like an input).

- The std-map registers are the strip's crosspoints into **block 0
  (Phones)** (`0x0034 + src` L / `0x004E + src` R); the low map is the
  strip's own pair (`0x0000 + src` L / `0x001A + src` R).  The src
  indexes follow the crosspoint table (AN1..ADAT7/8 = 0..11,
  PB1..PB6 = 12..23).
- **The targets are FIXED per strip (user-confirmed: the submix view
  was NOT changed during cap_width6; each strip wrote a DIFFERENT
  target)** — NOT the viewed output.  cap_width4's extra block-4
  write did NOT reproduce in cap_width7 (same strip, hands off) →
  routing/state-dependent at capture time, not a fixed target.
- **cap_width7 = PB6 → out 4 (the playback pattern PB_n → out(n−2)).**
- **Linux gap**: the kernel `Width` control writes only the AN1/2 low
  map.  Full TotalMix parity = per-strip width controls writing the
  strip's src pair on the low map (inputs + PB1) + the std block
  (out 0 for inputs/PB1, out n−2 for PB2-6).  Width on a 0-routed
  strip writes NOTHING (needs non-zero crosspoints).
- Trim (T), MS proc, loopback, mono, ext input, mute FX, talkback,
  ref level, stereo-split: NOT yet captured (next capture).

### Front-panel button map — COMPLETE (cap_buttons2.pcap, 2026-08-23)

All buttons identified by their 0x17 readback flash (byte3) + side
writes. The readback is polled by the host (~50 Hz), byte0/1 = state,
byte2 = selection/counter, byte3 = button-down flash:

| Button | byte3 flash | 0x17 readback changes | Host writes |
|---|---|---|---|
| IN | 0x41 | byte2 cycles 0x4A/0x5A/0x6A (Ch1/2, 3/4, Opt) | — |
| OUT | 0x48 | byte1 cycles 0x04/0x05/0x06 (Ch1/2, Phones, Opt); byte2 → 0x8A | `0x17 0x0400 0x2000` poll |
| SET (A) | 0x42 | — | — (standalone: 48V; online: no USB effect) |
| MIX (B) | 0x44 | byte0/1 gain 0x80 bits (panel engaged); MIX-mode LED flash = byte1 bit 0x08 toggling | `0x17 0x8480 0x8C80` then `0x0400 0x8080/0x8000`; `0x17 0x0C00` during LED flash |
| SELECT | 0x50 | (0x51 pulses during LED-brightness cycle) | — |
| DIM | 0x60 | byte1 bit 0x20 sticky while dimmed | fader pair `0x1A 0x0006/7` + `0x12 0x03E2/3` + `0x17 0x2000` poll |
| wheel | — (no byte3 flash) | byte2 = wheel step counter (rotary only) | fader writes (selected context) |

- **NO push-button on the wheel — CONFIRMED (cap_wheelpress.pcap,
  2026-08-23)**: the manual says the unit has "a hi-precision rotary
  encoder, 6 buttons" — the 6 buttons are IN/SET/MIX/OUT/SELECT/DIM
  (each has a byte3 flash); the wheel has NONE. Test: user pressed the
  wheel several times; the ONLY flash was 0x50 (SELECT) at the end, and
  the ONLY byte2 movement was the rotary counter (0x4B → 0x42 = +7
  CW, then 0x42 → 0x47 = −11 CCW, 4-bit digit wrap 0x4F→0x40), with
  ZERO host writes in the idle menu state. A push, if it existed,
  would flash byte3 like every other button. Wheel = pure rotary
  encoder; "press" actions on the unit are done via the 6 buttons.

- **SPDIF/ADAT via SET+Opt (cap_spdif.pcap)**: ZERO USB writes — the
  panel format switch does not reach the host in online mode
  (standalone-only feature, like 48V via SET). The 8/2-LED display is
  device-local. The host-side Optical Out (SPDIF/ADAT) is only
  changeable via Fireface USB Settings (keepalive flag 0x0401).
- **LED brightness (hold SELECT + press IN, cap_buttons2.pcap)**: ZERO
  USB writes — device-local (byte3 0x50↔0x51 pulses during the
  cycle).
- **DIM ON** = `0x1A <current/8b> 0x0006/7` + `0x12 <current or
  dimmed/16b> 0x03E2/3` (0x001A or 0x000D observed — current fader
  position) + byte1 0x20 + `0x17 0x2000` poll. **DIM OFF / 2 s hold** =
  same pair with the SESSION-restore value (`0x1A 0x00AC` + `0x12
  0x0089` = the saved PH3/4 fader) + byte1 0x20 cleared. So DIM-2s
  undoes all wheel changes since DIM-on (manual §5.1 confirmed).
- Wheel on OUT (any output) = `0x12` master fader `0x03E0+2·out`
  (16-bit) with the 2-bit transaction counter cycling in the high
  bits; the `0x1A` 8-bit companion appears at gesture start/end.
  Wheel values follow the master curve 0x2000·2^(dB/6) (~0.5 dB/
  click).

### Front-panel readback — LIVE Linux verification (2026-08-26, kernel panel worker)

The kernel driver now polls the 0x17 readback at **wIdx=0x0000** (the
index the Windows driver polls — cap_buttons2.pcap) at 50 Hz and
mirrors it into read-only `Front Panel *` ALSA controls.  Live findings:

- **Idle readback on the live unit**: `01 01 81 40` — byte1 = 0x01
  (OUT base mode = Ch 1/2, matches cap_padpan's idle byte1), byte2 =
  0x81 (bit7 + wheel counter 1), byte3 = 0x40 (idle base).  The byte1
  OUT field carries TWO encodings depending on the display mode:
  gain-display 0x04/0x05/0x06 (cap_dim) vs base 0x01/0x02/0x00
  (cap_buttons; live idle = 0x01).  The driver accepts both.
- **The byte2 IN-position bits (4-6) are absent in the base mode**
  (live 0x81 → IN = 0/unknown); they only appear after an IN press
  (gain display 0x4A/0x5A/0x6A).  The `Front Panel In` control seeds
  on the first IN press.
- **byte3 idle base confirmed 0x40 live** (the flash values 0x41..0x60
  ride above it).
- **OPEN — preamp byte0 readback index**: with 48V Mic1 ON, a wIdx=
  0x0000 read returns byte0 = 0x01 (raw bit) but a wIdx=0x003F read
  returns byte0 = 0x00 — and interleaving the two (p48watch) made BOTH
  read 0x00 for the whole run while the control stayed ON, then a lone
  wIdx=0x0000 read returned 0x01 again.  The probe's preamp sync reads
  wIdx=0x003F (byte0 = 0x00 — masked in practice by the saved-state
  restore).  The 48V LED/audio path is unaffected (writes + saved
  state drive it); the byte0 readback semantics at 0x003F vs 0x0000
  need a controlled capture.

### Front-panel decode — LIVE hardware validation (2026-08-26, kernel panel worker)

Full button/wheel validation with the physical unit (panel_monitor.py + raw
0x17 watcher):

- **All six button flash codes confirmed live**: 0x41 IN, 0x42 SET,
  0x44 MIX, 0x48 OUT, 0x50 SELECT, 0x60 DIM (each press = a 100-200 ms
  byte3 flash over the 0x40 idle base; the flash persists several
  poll frames).
- **IN cycling** (byte2 bits 4-6, gain-display mode): Ch 1/2 (0x4x) →
  Ch 3/4 (0x5x) → Opt (0x6x) — live-verified; the base display mode
  drops the IN bits (byte2 = 0x8x/0x0x) so the enum keeps the last
  known selection.
- **OUT cycling**: byte1 = 0x00 in the idle/base mode (unknown here),
  a press steps to 0x04 = Ch 1/2 then 0x05 Phones / 0x06 Opt
  (cap_dim encoding); the cap_buttons base-mode 0x01/0x02/0x00 also
  accepted.  Live: OUT = Unknown → Ch 1/2 after the first press.
- **Wheel**: byte2 low nibble = the rotary counter, signed 4-bit wrap
  delta (CW +1, CCW -1 per detent); the driver accumulates it.
- **DESIGN FIX (consume-on-get is broken)**: the first version cleared
  the button/wheel value on get; wireplumber (which subscribes to
  every notifying control and re-reads it — stack-traced via
  dump_stack) ate the events at ~48 Hz before any other reader.  The
  controls now hold the LAST state (button = last press code, wheel =
  accumulated delta) and consumers track their own baseline.

### Front-panel MIX monitoring — captured (cap_mix.pcap, 2026-08-23)

User sequence: OUT presses, IN presses (mode + cycling), MIX press
(monitoring), wheel (level), SELECT (exit). Decoded:

- **IN cycling in readback byte2**: 0x4A = Ch1/2, 0x5A = Ch3/4, 0x6A =
  Opt (0x0A suffix = SELECT/gain display). User note: first IN/OUT
  press only switches the mode LED; only subsequent presses cycle.
- **MIX press**: byte0 AND byte1 gain their 0x80 bit (0x0C 0x05 →
  0x8C 0x85 = panel engaged) + byte3 0x44 flash; host writes
  `0x17 0x8480 0x8C80` + `0x1A 0x0000 0x000A/0x000B` (the AN1/2 gain
  regs written 0?).
- **MIX LED flashing**: byte1 toggles 0x85↔0x89 (bit 0x08) and the
  host alternates `0x17 0x8480 0x0C00` / `0x17 0x8880 0x0C00` — the
  previously-unmapped **0x0C00 wIdx = MIX-mode panel state**.
- **Wheel (monitoring level)**: `0x12` fader writes on the STANDARD
  crosspoint map 0x0034/0x004E (AN1→out0), 0x0003→0x0117, + `0x1A`
  companion writes on 0x000A/0x000B (values 0x0000→0x0005, only on
  some clicks — 8-bit shadow or gain regs; TBD). Readback byte2 =
  wheel counter 0x00+n (fader mode).
- **SELECT (exit)**: 0x80 bits cleared (0x8C 0x85 → 0x0C 0x05) +
  writes `0x17 0x0400 0x8000` + `0x17 0x0400 0x8080`.

### Front-panel DIM / Recall / wheel — captured (cap_dim.pcap, 2026-08-23)

User sequence (OUT on the Main Out = Phones 0x05): wheel, DIM-2s,
SELECT, wheel, DIM-2s. Decoded:

- **OUT position in 0x17 readback byte1**: 0x04 = Ch1/2, 0x05 =
  Phones, 0x06 = Opt (4 presses cycled 04→05→06→04→05, ended Phones).
- **DIM ON (short press)**: byte1 gains bit 0x20 (0x05→0x25, sticky
  while dimmed) + write `0x1A <8b> 0x0006/7` + `0x12 <16b> 0x03E2/3`
  (the DIMMED volume, 0x001A ≈ -48 dB here) + `0x17 0x2400 0x2000`.
- **DIM OFF / 2 s hold**: byte1 0x20 cleared + same write pair with the
  RESTORED "former" volume (0x0089 — the value when DIM was engaged)
  + `0x17 0x0400 0x2000`. So DIM-2s = undo all wheel changes since
  DIM-on (§5.1 confirmed on hardware).
- **SET-2s (store Recall)**: NO USB write at all (host-side TotalMix
  memory). Same for the wheel on the Main Out: `0x12 0x03E2/3` only
  (no 0x1A pair mid-gesture; the 0x1A companion appears at gesture
  start/end).
- **Wheel on Phones writes 0x03E2/0x03E3** → confirms the master-fader
  map 0x03E0+2·out (PH3/4 = out 1) with the 2-bit transaction counter
  in the high bits (0x00/0x40/0x80/0xC0 cycling).
- **SELECT press** flashes byte3 (0x42 observed; 0x50/0x60 also seen
  in cap_set2.pcap — full map TODO: SELECT state L/R/both vs DIM
  press both produce byte3 flashes).
- Wheel values follow the master curve 0x2000·2^(dB/6) exactly
  (0x2000 = 0 dB, 0x3C46 ≈ +5.5 dB, 0x019A ≈ -26 dB, ~0.5 dB/click).

TODO captures (Windows): ~~MIX monitoring~~ ✓ cap_mix.pcap,
~~SET+Opt SPDIF/ADAT~~ ✓ cap_spdif.pcap (no writes — standalone-only),
~~SELECT+IN brightness~~ ✓ cap_buttons2.pcap (no writes —
device-local), ~~byte3 flash map~~ ✓ cap_buttons2.pcap.

**SELECT PRESS IS VISIBLE (cap_select2.pcap, 2026-08-24 — the earlier
"SELECT not in the readback" note was wrong)**: the ep 0x82 interrupt
status stream carries the same panel state as the 0x17 control read:
- A SELECT press = a **byte3 flash 0x50** (`0F 05 44 50`...). IN =
  flash 0x41 + byte2 0x84→0x44 (OUT 0x8x → IN 0x4x).
- After IN, byte2 walks a 16-step counter `0x44..0x4F, 0x40..0x43`
  — the SELECT state (L/R/both/none) was thought to be **byte2
  low-nibble mod 4** (4 states per full cycle; the counter also
  advances on the wheel). **LINUX CHECK (2026-08-24, panelprobe)
  REJECTED this for the readback**: pressing SELECT 5× left byte2
  constant (0x47 in the test) — byte3 flashed 0x50 per press but the
  state is NOT in the 0x17 readback. The host-side counter stays (the
  L/R/both/none cycle with None = no target is hardware-verified by
  the user). The ep 0x82 status-stream encoding (if any) is still
  unverified — it only flows while streaming.
- MIX engaged: `8F 8D 03 44` (byte0/1 gain 0x80 bits, byte2 = 0x03
  fader counter) + ack `0x17 0x8c80 0x8c80`; the wheel in MIX writes
  the standard-map crosspoints only (0x12 0x0002→0x0003→0x0005→
  0x0008→0x000c on 0x0034/0x004E/0x0035/0x004F = fader steps from
  -inf) + 0x1A on 0x000a/0x000b (the panel gain registers).

## Resources

- **Official RME manual**: `tools/usbdump/manual.txt` (197 KB, clean
  text) + `bface_pro_fs_e.pdf` (EN). Manual text was extracted by the
  user with an external tool and copied into the repo 2026-08-23.
  `tools/usbdump/pdftext2.ps1` also works now (ToUnicode CMaps +
  position sort; fixed 2026-08-23: `Encoding::Latin1` is .NET-Core-
  only → use `GetEncoding('ISO-8859-1')`; `-eq` is case-insensitive →
  use `-ceq` for `Tj`/`TJ`; `$fnum` was never set in the `Tj` branch;
  page order now from the page-tree walk).

## TODO

- [x] Identify mute / solo / pan (targeted capture)
      → mute (0x1A 0x0006/7 + 0x12 0x03E2/3, or 0x12 0 on standard
      crosspoints), solo (0x12 0x2000/0x0000 on 0x000C+0x0027),
      pan = balance on the stereo crosspoint
- [x] Map the crosspoint addresses (input×output)
      → L = 0x0034 + 0x0034·out + idx, R = 0x004E + 0x0034·out + idx
- [ ] Calibrate the volume scale (8-bit vs 16-bit, dB per step) — partial:
      -40 dB = 0x0317, -20 dB = 0x139E (16-bit, tapered curve)
- [x] Decode the 480-byte bulk stream (VU) with audio
      → DSP telemetry, not meters (see above)
- [x] Map the ISO IN/OUT channels (AN1-4, ADAT, SPDIF)
      → ch0 = AN1, ch1 = AN2 confirmed by live test; AN3/4 = ch2/3
      probable; ADAT/SPDIF = ch6-13 (zeros, nothing connected)
- [x] Determine the role of the flags (0x4000/0x8000/0xC000)
      → 2-bit transaction counter cycling on every write
- [x] Preamp gain / PAD (cap_gain_solo.pcap) — see Preamp block above
- [x] Source indices (cap_srcmap.pcap): AN1-4 = 0-3, AS1/2 = 4/5,
      ADAT3/4 = 6/7, ADAT5/6 = 8/9, ADAT7/8 = 10/11, playbacks = 12/13…
- [x] Second crosspoint map found (0x0000+idx / 0x001A+idx) — written
      for some fader drags; trigger partially analyzed (cap_lowmap)
- [x] Solo: 0x12 0x2000/0x0000 on 0x0004+0x001F+0x000C+0x0027
      (cap_lowmap.pcap)
- [ ] When is the low map vs the standard map written (selected output?)
      → partial: clicking an output strip seems to flip low→standard
- [ ] Calibrate the gain scale (observed 0-10, registry default 32)
- [x] Mute of the selected channel → DECODED (cap_mute2.pcap,
      2026-08-24): mute = zero the strip's crosspoints (low map for
      inputs + the out0 pairs for playbacks, 0x2000/0x1000 restore
      markers), no dedicated flag. Implemented in set_mute.
- [ ] Confirm the master-fader register per output (0x03E0 stride?)
      → DONE: 0x03E0+2·out (16-bit) / 0x0004+2·out (8-bit), verified
      for AN1/2, PH3/4, AS1/2
- [ ] Confirm the output strip ORDER / names (strip PH3/4 and AS1/2)
      → DONE: base 0x34+0x34·out follows strip order (0x68 = PH3/4)
- [ ] Effect of the "stereo" button (split AN1/AN2 buses) on writes
- [~] Test: sample rate, sync, routing, SPDIF, ADAT, loopback
      → sample rate partially done (cap_rates.pcap: SET_INTERFACE-only);
      clock source / optical / pitch pending (cap_fus.pcap)
- [x] SELECT channel state (L/R/both/none) — **CONFIRMED NOT in the
      0x17 readback (cap_select.pcap, 2026-08-24)**: during SELECT
      presses byte2 = the wheel counter at that moment (0x41/0x46/
      0x48/0x4D/0x4C, correlated with wheel turns, no state code) and
      the high nibble = the IN selection only. The state is HOST-SIDE
      (TuxMix's counter can drift from the card's LED display —
      unavoidable). NEW FINDINGS from the same capture:
      - The panel gain wheel writes `0x1A` wIdx **0x000A+mic** (the
        "ADC gain" family — NOT 0x0000+mic the GUI uses), raw in bits
        0-4, no cycling counter; both channels written together in
        linked mode (`set_panel_gain` implemented). Relationship to
        the 0x0000+mic gain: Linux check.
      - The MIX wheel writes ALL FOUR crosspoints (AN1+AN2 × L/R) in
        linked mode (0x0034/0x004E/0x0035/0x004F together).
- [x] Verify the "streaming active" indicator in the 0x17 readback
      → byte 2 = 0x80 is the CLOCK NO-LOCK state (cap_clk.pcap:
      Optical clock source + no lock → 0x80; Internal → 0x40). NOT a
      streaming flag. Byte 0 = 0x0C/0x0D is the streaming-ish state.
- [x] Capture the 0x41 → 0x80 transition (never captured before)
      → 0x80 = clock no-lock (see "Clock source / no-lock state")
      → mid-session rate change = SET_INTERFACE(5, alt) ONLY, no 0x1B.
      COMPLETE alt table (cap_rates2 sweep): alt 1 = 32/44.1/48/64/88.2,
      alt 2 = 96/128, alt 3 = 176.4/192. 0x1B = 4-bank register upload
      (wIdx = addr<<8 | bank); the 48k clock quad is in the coldplug
      block. 0x11 readback encodes the alt as 2^alt (2/4/8).
