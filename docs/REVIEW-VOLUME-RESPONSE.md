# Reply to Takashi — "do we have to handle the volume conversions in the driver?"

Draft ready to adapt/send in response to:

> About the basic code design: do we have to handle the volume
> conversions in the driver? Doing such in the kernel driver is usually
> avoided as much as possible.

---

## Proposed reply

> Short answer: the driver does **not** do any host-side volume
> conversion or PCM scaling in the audio path. The "conversions" that
> remain in the driver are of three kinds, and each is either standard
> ALSA practice or protocol-mandated:

### 1. System volume = PipeWire *software* volume (no hardware write)

The stream/system volume is **not** handled in the kernel at all. It is
PipeWire's host-side software gain, exactly matching the Windows model
(`cap_sysvol2.pcap`: the Windows volume slider issues zero USB writes —
it is a host-side stream gain).

The kernel exposes the output masters as per-output named controls
(`AN1/2 Playback Volume`, `PH3/4 Playback Volume`, …). Because there is
no generic `Master` element, WirePlumber/SPA automatically falls back to
software volume — verified that `wpctl set-volume` moves **no** hardware
register and the data path is untouched.

### 2. dB TLV metadata + register-law mapping (standard mixer-driver work)

- The output masters carry a dB **TLV** (`bf_master_tlv`: the hardware
  law `20·log10(v/0x2000)`, `0x2000`=0 dB, `0x4000`=+6 dB). This is
  read-only metadata so mixers/GUIs display dB and WirePlumber maps
  volume correctly. It is not audio-domain scaling.
- The preamp gains have non-1 dB hardware steps (mic = 3.25 dB/step,
  instrument = 0.5 dB/step), so the control space is dB and the driver
  does a small **integer** `dB ↔ register` map (`bf_gain_raw`/`bf_gain_db`:
  `raw*13/4` and `db*8/26`). This is the same value-translation every
  hardware mixer driver does (control space → register), not stream math.

### 3. Fixed-point DSP math for the hardware EQ (protocol-mandated)

The one place real math lives in-kernel is the EQ: `bf_eq_band_words()`
computes the biquad coefficients (Q27 fixed-point, CORDIC sin/cos,
`bf_exp2`) because the device only accepts pre-computed coefficient
words on the bulk EP (0x0A). There is no firmware/library to offload
this to, so the kernel must synthesize the coefficients to program the
hardware DSP. This is device programming, not signal conversion, and it
is not in the streaming data path.

> I'm happy to trim or re-shape any of these if you'd prefer a
> different split, but I'd argue none of them is a "volume conversion in
> the driver" in the sense that's usually avoided (host-side scaling of
> the PCM stream).

---

## Facts verified in the code

| Item | File | What |
|---|---|---|
| System volume = software (no generic master) | `babyfacepro-ctl.c` `babyface_create_controls()` | per-output named controls → SPA software volume |
| Master dB TLV | `babyfacepro-ctl.c` `bf_master_tlv` (L267) | `DECLARE_TLV_DB_RANGE` + `TLV_DB_LINEAR_ITEM`, `TLV_READ` access |
| Preamp gain laws | `babyfacepro-ctl.c` `bf_gain_raw`/`bf_gain_db` (L1178-1186) | mic `db*8/26` → raw ; instr `db*2` |
| DSP EQ math (unavoidable) | `babyfacepro-ctl.c` `bf_eq_band_words` (L2329) | Q27, CORDIC, `bf_exp2` ; bulk EP 0x0A |
