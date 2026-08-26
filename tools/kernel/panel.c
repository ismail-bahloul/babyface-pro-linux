// SPDX-License-Identifier: GPL-2.0-only
/*
 * RME Babyface Pro FS — proprietary-mode USB audio driver
 * Front-panel worker: polls the 0x17 readback (wIdx 0x0000, 50 Hz) and
 * exposes the decoded panel state as read-only ALSA controls.  The host
 * is "in the loop" (cap_panel.pcap): TotalMix translates the physical
 * wheel/buttons into mixer writes.  The driver plays that TotalMix role
 * for the MIX (fader) mode — host-acks the 0x44 flash (0x17 0x8480
 * 0x8C80, the missing piece that made "MIX do nothing") and drives the
 * wheel on the calibrated fader curve — and MIRRORS everything else
 * (IN/OUT/gain-mode wheel, DIM, SELECT) for the user-space consumer.
 *
 * Readback layout (cap_buttons2.pcap, hardware-verified):
 *   byte 0  preamp 48V/PAD bits (base 0x0C) | 0x80 = MIX engaged
 *   byte 1  OUT selection 0x04/0x05/0x06 (Ch1/2, Phones, Opt)
 *           | 0x20 = DIM sticky | 0x80 = MIX engaged
 *   byte 2  IN selection (bits 4-6: 4/5/6 = Ch1/2, Ch3/4, Opt) + wheel
 *           counter (low nibble) | 0x80 = clock no-lock
 *   byte 3  button flash over the 0x40 idle base: 0x41 IN, 0x42 SET,
 *           0x44 MIX, 0x48 OUT, 0x50 SELECT, 0x60 DIM
 */
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/unaligned.h>
#include <linux/usb.h>
#include <linux/workqueue.h>
#include <sound/control.h>
#include <sound/tlv.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>

#include "snd-usb-babyface-pro.h"

/* Windows polls the 5-register status set at ~50 cycles/s; match that. */
#define BF_PANEL_POLL_MS		20

/* Control indices in chip->panel_kctl[] (for snd_ctl_notify). */
enum {
	BF_PANEL_KCTL_BUTTON,
	BF_PANEL_KCTL_WHEEL,
	BF_PANEL_KCTL_IN,
	BF_PANEL_KCTL_OUT,
	BF_PANEL_KCTL_MIX,
	BF_PANEL_KCTL_DIM,
	BF_PANEL_KCTL_SELECT,
	BF_PANEL_KCTL_NUM,
};

static const char *const bf_panel_in_texts[] = {
	"Unknown", "Ch 1/2", "Ch 3/4", "Opt", NULL
};

static const char *const bf_panel_out_texts[] = {
	"Unknown", "Ch 1/2", "Phones", "Opt", NULL
};

static const char *const bf_panel_select_texts[] = {
	"Left", "Right", "Both", "None", NULL
};

/* byte3 button flash → event code (0 = none).  The idle byte3 is 0x40;
 * a press flashes the value below the base for one or two poll frames.
 */
static int bf_panel_button_decode(u8 flash)
{
	switch (flash) {
	case BF_PANEL_FLASH_IN:		return BF_PANEL_BTN_IN;
	case BF_PANEL_FLASH_SET:	return BF_PANEL_BTN_SET;
	case BF_PANEL_FLASH_MIX:	return BF_PANEL_BTN_MIX;
	case BF_PANEL_FLASH_OUT:	return BF_PANEL_BTN_OUT;
	case BF_PANEL_FLASH_SELECT:	return BF_PANEL_BTN_SELECT;
	case BF_PANEL_FLASH_DIM:	return BF_PANEL_BTN_DIM;
	default:			return BF_PANEL_BTN_NONE;
	}
}

/* (byte2 >> 4) & 7 = IN position 4/5/6 → enum index (0 = not in range). */
static int bf_panel_in_decode(u8 nib)
{
	switch (nib) {
	case BF_PANEL_IN_CH12:		return 1;
	case BF_PANEL_IN_CH34:		return 2;
	case BF_PANEL_IN_OPT:		return 3;
	default:			return 0;
	}
}

/* byte1 & 7 = OUT position.  Two encodings seen in captures: the
 * gain-display mode 0x04/0x05/0x06 (cap_dim.pcap, cap_buttons2.pcap)
 * and the base mode 0x01/0x02/0x00 (cap_buttons.pcap; 0x01 is also the
 * idle byte1 of cap_padpan.pcap and the live device).  Accept both;
 * 0x00 is ambiguous (could be Opt or no selection) so keep previous.
 */
static int bf_panel_out_decode(u8 v)
{
	switch (v) {
	case BF_PANEL_OUT_CH12:		return 1;
	case BF_PANEL_OUT_PHONES:	return 2;
	case BF_PANEL_OUT_OPT:		return 3;
	case 0x01:			return 1;	/* base-mode Ch 1/2 */
	case 0x02:			return 2;	/* base-mode Phones */
	default:			return 0;
	}
}

/* ── MIX-mode monitoring level (fader curve) ────────────────
 * Calibrated crosspoint-fader curve (AN1→AN1/2, cap_calib.pcap
 * 2026-08-22; the same table as tuxmix-core/src/usb.rs FADER_CURVE).
 * dB stored ×2 (half-dB grid): the MIX wheel steps ±0.5 dB per click
 * on this curve (cap_mix.pcap).  0x0000 = −inf (digital mute),
 * 0x0003 = −62 dB, … 0x2D41 = +6 dB.  Raw values interpolate linearly
 * between the 1-dB points.
 */
#define BF_FADER_DB2_INF	(-130)	/* −65 dB = the wheel's −inf floor */

static const struct bf_fader_pt {
	s16 db2;	/* dB × 2 */
	u16 raw;
} bf_fader_curve[] = {
	{ -124, 0x0003 }, { -122, 0x0004 }, { -120, 0x0005 },
	{ -118, 0x0006 }, { -116, 0x0007 }, { -114, 0x0008 },
	{ -112, 0x0009 }, { -110, 0x000a }, { -108, 0x000b },
	{ -106, 0x000d }, { -104, 0x000e }, { -102, 0x0010 },
	{ -100, 0x0012 }, {  -98, 0x0014 }, {  -96, 0x0017 },
	{  -94, 0x0019 }, {  -92, 0x001d }, {  -90, 0x0020 },
	{  -88, 0x0024 }, {  -86, 0x0029 }, {  -84, 0x002e },
	{  -82, 0x0033 }, {  -80, 0x003a }, {  -78, 0x0041 },
	{  -76, 0x0049 }, {  -74, 0x0051 }, {  -72, 0x005b },
	{  -70, 0x0067 }, {  -68, 0x0073 }, {  -66, 0x0081 },
	{  -64, 0x0091 }, {  -62, 0x00a3 }, {  -60, 0x00b7 },
	{  -58, 0x00cd }, {  -56, 0x00e6 }, {  -54, 0x0102 },
	{  -52, 0x0122 }, {  -50, 0x0145 }, {  -48, 0x016d },
	{  -46, 0x019a }, {  -44, 0x01cc }, {  -42, 0x0204 },
	{  -40, 0x0243 }, {  -38, 0x028a }, {  -36, 0x02d9 },
	{  -34, 0x0332 }, {  -32, 0x0396 }, {  -30, 0x0406 },
	{  -28, 0x0483 }, {  -26, 0x0510 }, {  -24, 0x05af },
	{  -22, 0x0660 }, {  -20, 0x0727 }, {  -18, 0x0807 },
	{  -16, 0x0902 }, {  -14, 0x0a1b }, {  -12, 0x0b57 },
	{  -10, 0x0cb9 }, {   -8, 0x0e47 }, {   -6, 0x1004 },
	{   -4, 0x11f9 }, {   -2, 0x142a }, {    0, 0x16a0 },
	{    2, 0x1963 }, {    4, 0x1c7c }, {    6, 0x1ff6 },
	{    8, 0x23dc }, {   10, 0x283d }, {   12, 0x2d41 },
};

/* Fader raw → dB×2 (linear interpolation; raw 0 = −inf). */
static int bf_fader_raw_to_db2(u16 raw)
{
	int i;

	if (raw == 0 || raw < bf_fader_curve[0].raw)
		return BF_FADER_DB2_INF;
	for (i = 0; i < ARRAY_SIZE(bf_fader_curve) - 1; i++) {
		if (raw <= bf_fader_curve[i + 1].raw) {
			u32 num = (u32)(raw - bf_fader_curve[i].raw) *
				  (u32)(bf_fader_curve[i + 1].db2 - bf_fader_curve[i].db2);
			u32 den = bf_fader_curve[i + 1].raw - bf_fader_curve[i].raw;

			return bf_fader_curve[i].db2 + (int)((num + den / 2) / den);
		}
	}
	return bf_fader_curve[ARRAY_SIZE(bf_fader_curve) - 1].db2;
}

/* dB×2 → fader raw (linear interpolation; below −62 dB = mute 0). */
static u16 bf_fader_db2_to_raw(int db2)
{
	int i;

	if (db2 <= bf_fader_curve[0].db2)
		return db2 < bf_fader_curve[0].db2 ? 0 : bf_fader_curve[0].raw;
	for (i = 0; i < ARRAY_SIZE(bf_fader_curve) - 1; i++) {
		if (db2 <= bf_fader_curve[i + 1].db2) {
			u32 num = (u32)(db2 - bf_fader_curve[i].db2) *
				  (u32)(bf_fader_curve[i + 1].raw - bf_fader_curve[i].raw);
			u32 den = bf_fader_curve[i + 1].db2 - bf_fader_curve[i].db2;

			return bf_fader_curve[i].raw + (u16)((num + den / 2) / den);
		}
	}
	return bf_fader_curve[ARRAY_SIZE(bf_fader_curve) - 1].raw;
}

/* MIX-mode VU display law — monitoring dB×2 → the 0x1A 0x000A display
 * value.  Piecewise-linear through the captured (dB, display) points
 * (cap_mix.pcap 2026-08-23: (−62,0) (−54,1) (−48,2) (−42.5,3)
 * (−35,4) (−28.4,5); cap_panel.pcap: (−7.4,10) (−6.7,11)
 * (−4.6,12)) — a log-ish VU scale (coarse at the bottom, ~1.4 dB/step
 * near 0).  The −28..−8 dB middle is interpolated; the exact law is
 * pending the cap_mixdisp.pcap full-range sweep (TODO 0g).
 */
static int bf_mix_display(int db2)
{
	static const struct {
		s16 db2;
		u8 disp;
	} pts[] = {
		{ -124, 0 }, { -108, 1 }, {  -96, 2 }, {  -85, 3 },
		{  -70, 4 }, {  -57, 5 }, {  -15, 10 }, {  -13, 11 },
		{   -9, 12 },
	};
	int i;

	if (db2 <= pts[0].db2)
		return 0;
	for (i = 0; i < ARRAY_SIZE(pts) - 1; i++) {
		if (db2 <= pts[i + 1].db2) {
			u32 num = (u32)(db2 - pts[i].db2) *
				  (u32)(pts[i + 1].disp - pts[i].disp);
			u32 den = pts[i + 1].db2 - pts[i].db2;

			return pts[i].disp + (int)((num + den / 2) / den);
		}
	}
	/* Above −4.6 dB: keep the last slope (2 dB/step) up to +6 dB. */
	return pts[ARRAY_SIZE(pts) - 1].disp +
	       clamp((db2 - pts[ARRAY_SIZE(pts) - 1].db2) / 4, 0, 12);
}

/* The kernel driver plays the TotalMix role for the MIX button (the
 * standalone emulator is hardware-validated in tuxmix-core/src/panel.rs
 * + usb.rs): one wheel click in fader mode = ±0.5 dB on the SELECT-
 * chosen channel(s) of the IN-selected pair, into the OUT-selected
 * output's crosspoint block — the STANDARD map only (cap_mix.pcap /
 * cap_select2.pcap, no low-map mirror).  Mirrors the change into the
 * xpoint cache so the ALSA controls follow the wheel.  Takes the mutex
 * (the 0x12 writes cycle the transaction flag like the mixer puts).
 */
static void bf_panel_mix_wheel(struct snd_usb_babyface *chip, int delta)
{
	/* Canonical output of the OUT selection (enum 1 = Ch1/2,
	 * 2 = Phones, 3 = Opt): AN1/2, PH3/4, ADAT7/8 (the optical
	 * output) respectively.
	 */
	int out = chip->panel_out == 3 ? 5 :
		  chip->panel_out == 2 ? 1 : 0;
	unsigned int blk = bf_xpoint_block[out];
	u8 targets[2];
	int n = 0;
	int db2;
	u16 raw, flag;
	int i;

	/* SELECT-chosen channel(s) of the IN pair (manual §5.1: SELECT
	 * steps left/right/both; none = nothing selected = no-op wheel).
	 * Source indices: AN1/AN2 = 0/1, AN3/AN4 = 2/3, AS1/2 = 4.
	 */
	if (chip->panel_in == 3) {
		targets[0] = 4;		/* Opt: the AS1/2 pair */
		n = 1;
	} else if (chip->panel_select != 3) {
		int base = chip->panel_in == 2 ? 2 : 0;

		targets[0] = base + (chip->panel_select == 1 ? 1 : 0);
		n = 1;
		if (chip->panel_select == 2)
			targets[n++] = base + 1;
	}

	mutex_lock(&chip->mutex);
	db2 = bf_fader_raw_to_db2(chip->panel_mix_raw);
	db2 = clamp(db2 + delta, BF_FADER_DB2_INF, 12);
	raw = bf_fader_db2_to_raw(db2);
	chip->panel_mix_raw = raw;
	for (i = 0; i < n; i++) {
		const struct bf_source *s = &bf_sources[targets[i]];

		flag = bf_flag_cycle[chip->flag_cnt];
		chip->flag_cnt = (chip->flag_cnt + 1) & 3;
		bf_vendor_write(chip, BF_REQ_CROSSPOINT, raw,
				(BF_REG_CROSS_BASE_L + BF_REG_CROSS_STRIDE * blk +
				 s->idx_l) | flag);
		bf_vendor_write(chip, BF_REQ_CROSSPOINT, raw,
				(BF_REG_CROSS_BASE_R + BF_REG_CROSS_STRIDE * blk +
				 s->idx_r) | flag);
		chip->xpoint[out][targets[i]][0] = raw;
		chip->xpoint[out][targets[i]][1] = raw;
		/* MIX-mode VU display shadow (0x1A 0x000A+mic): TotalMix
		 * mirrors the monitoring level into the panel display family
		 * (cap_mix/cap_panel.pcap) — the input VU segments follow it.
		 * Written only on change (the captures show TotalMix updating
		 * it on segment crossings).  Law = bf_mix_display (TODO 0g
		 * pending the exact full-range capture).
		 */
		if (targets[i] < 4) {
			int disp = bf_mix_display(db2);

			if (disp != chip->panel_mix_disp[targets[i]]) {
				bf_vendor_write(chip, BF_REQ_GAIN,
						 (u16)disp,
						 BF_REG_PANEL_GAIN + targets[i]);
				chip->panel_mix_disp[targets[i]] = disp;
			}
		}
	}
	mutex_unlock(&chip->mutex);
}

/* Write an output's L/R masters (8-bit companions + 16-bit with the
 * transaction flag) and mirror into the cache — shared by the OUT
 * volume wheel and the balance wheel.  Caller holds the mutex.
 */
static void bf_panel_write_master(struct snd_usb_babyface *chip, int out,
				  u16 l, u16 r)
{
	u16 flag;

	flag = bf_flag_cycle[chip->flag_cnt];
	chip->flag_cnt = (chip->flag_cnt + 1) & 3;
	bf_vendor_write(chip, BF_REQ_GAIN, bf_master_8bit(l),
			BF_REG_MASTER_8 + 2 * out);
	bf_vendor_write(chip, BF_REQ_GAIN, bf_master_8bit(r),
			BF_REG_MASTER_8 + 2 * out + 1);
	bf_vendor_write(chip, BF_REQ_CROSSPOINT, l,
			(BF_REG_MASTER_16 + 2 * out) | flag);
	bf_vendor_write(chip, BF_REQ_CROSSPOINT, r,
			(BF_REG_MASTER_16 + 2 * out + 1) | flag);
	chip->master[out][0] = l;
	chip->master[out][1] = r;
	chip->muted[out] = false;
	/* A Phones change while DIM is engaged re-bases the restore. */
	if (chip->dim && out == 1) {
		chip->dim_saved[0] = l;
		chip->dim_saved[1] = r;
	}
}

/* OUT-mode wheel: the master fader of the OUT-selected output, ±0.5 dB
 * per click (cap_set2/cap_dim.pcap: the wheel writes the 16-bit master
 * 0x03E0+2·out on the master curve 0x2000·2^(dB/6); the driver keeps
 * the 8-bit companion in sync like bf_master_put — the 8-bit is the
 * real volume).  BOTH sides move by the same dB so an existing
 * balance (hold-SELECT) is preserved.  Same output mapping as the MIX
 * wheel (Phones = canon 1, Opt = ADAT7/8 = canon 5, else AN1/2).
 */
static void bf_panel_out_wheel(struct snd_usb_babyface *chip, int delta)
{
	int out = chip->panel_out == 3 ? 5 :
		  chip->panel_out == 2 ? 1 : 0;
	int hl, hr;
	u16 l, r;

	mutex_lock(&chip->mutex);
	hl = bf_master_half_db(chip->master[out][0]) + delta;
	hr = bf_master_half_db(chip->master[out][1]) + delta;
	l = bf_master_16bit(clamp(hl, -128, 12));
	r = bf_master_16bit(clamp(hr, -128, 12));
	bf_panel_write_master(chip, out, l, r);
	mutex_unlock(&chip->mutex);
}

/* IN-mode wheel: the gain of the SELECT-chosen channel(s) of the
 * IN-selected pair, ±1 dB per click (manual §5.1: SELECT steps
 * left/right/both, then the wheel changes the gain).  Writes the PANEL
 * gain registers 0x1A 0x000A+mic (cap_select.pcap 2026-08-24 — the
 * "ADC gain" family, which drives the same preamp as the GUI
 * 0x0000+mic; the cache tracks the raw either way).  Opt has no
 * preamp and SELECT None = no target.
 */
static void bf_panel_gain_wheel(struct snd_usb_babyface *chip, int delta)
{
	u8 mics[2];
	int n = 0;
	int i;

	if (chip->panel_in == 3 || chip->panel_select == 3)
		return;
	{
		int base = chip->panel_in == 2 ? 2 : 0;

		mics[0] = base + (chip->panel_select == 1 ? 1 : 0);
		n = 1;
		if (chip->panel_select == 2)
			mics[n++] = base + 1;
	}

	mutex_lock(&chip->mutex);
	for (i = 0; i < n; i++) {
		int mic = mics[i];
		int db = clamp((int)chip->gain[mic] + delta,
				0, bf_gain_max_db(mic));
		u8 raw = bf_gain_raw(mic, db);

		bf_vendor_write(chip, BF_REQ_GAIN, raw, BF_REG_PANEL_GAIN + mic);
		chip->gain[mic] = db;
	}
	mutex_unlock(&chip->mutex);
}

/* OUT-balance wheel (hold SELECT + wheel — manual §5.1 "Output
 * Balance"): moves the stereo image of the OUT-selected output by
 * attenuating ONE side, linear in raw (cap_pan_stereo.pcap: the varied
 * side = fixed·(1−|pan|), ~0x9C raw step per click at 0 dB — the PAN
 * of the stereo hardware output in TotalMix).  The balance position is
 * derived from the L/R master ratio (the louder side is the fixed
 * one), so the gesture needs no extra state — and the OUT wheel below
 * moves both sides by the same dB to preserve an existing balance.
 */
static void bf_panel_balance_wheel(struct snd_usb_babyface *chip, int delta)
{
	int out = chip->panel_out == 3 ? 5 :
		  chip->panel_out == 2 ? 1 : 0;
	u16 l = chip->master[out][0];
	u16 r = chip->master[out][1];
	int bal;		/* −100..+100; + = image right (left varies) */
	u16 fixed, varied;

	mutex_lock(&chip->mutex);
	/* Balance from the L/R ratio: the louder side is the fixed one. */
	if (l >= r) {
		bal = r ? -(100 - (100 * r) / l) : -100;
		fixed = l;
	} else {
		bal = l ? (100 - (100 * l) / r) : 100;
		fixed = r;
	}
	bal = clamp(bal + delta * 2, -100, 100);
	varied = (u16)((u32)fixed * (100 - abs(bal)) / 100);
	l = bal >= 0 ? varied : fixed;
	r = bal >= 0 ? fixed : varied;

	bf_panel_write_master(chip, out, l, r);
	mutex_unlock(&chip->mutex);
}

/* SET press (byte3 0x42 flash): toggle 48V phantom on the
 * SELECT-chosen mic(s) of the IN-selected pair.  The hardware only
 * does this in standalone mode (online, TotalMix ignores SET — no USB
 * write in the captures), but the driver IS the host: it writes the
 * preamp state itself and the P48 LEDs follow (the tuxmix-core
 * emulator, hardware-verified).  Restricted to IN mode + Ch1/2 (the
 * phantom-capable pair); Opt/Ch3/4 and SELECT None = no target.
 */
static void bf_panel_set_phantom(struct snd_usb_babyface *chip)
{
	u16 bits = 0;
	int m;

	if (chip->panel_mix || chip->panel_in != 1 ||
	    chip->panel_select == 3)
		return;
	if (chip->panel_select != 1)
		bits |= BF_PREAMP_48V_MIC1;
	if (chip->panel_select != 0)
		bits |= BF_PREAMP_48V_MIC2;

	mutex_lock(&chip->mutex);
	/* Toggle on the first target, mirror to the others. */
	if (bits & BF_PREAMP_48V_MIC1)
		chip->preamp ^= BF_PREAMP_48V_MIC1;
	if (bits & BF_PREAMP_48V_MIC2)
		chip->preamp ^= BF_PREAMP_48V_MIC2;
	bf_preamp_state_write(chip);
	for (m = 0; m < 4; m++)
		chip->panel_mix_disp[m] = 0;
	mutex_unlock(&chip->mutex);
}

static void bf_panel_notify(struct snd_usb_babyface *chip, int ctl)
{
	if (chip->panel_kctl[ctl])
		snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE,
			       &chip->panel_kctl[ctl]->id);
}

/* One 0x17 read + decode.  Called from the poll work; no locking needed —
 * the worker is the only writer and the control get callbacks run under
 * the ALSA controls lock (chip->panel_button/wheel are consumed there).
 */
static void bf_panel_tick(struct snd_usb_babyface *chip)
{
	u8 st[4];
	int delta, in, out;
	bool dim;
	u8 cls, pcls;
	int btn;
	bool mix_flash, fader_now;

	if (bf_vendor_read(chip, BF_REQ_PREAMP, BF_REG_PANEL_READ, st) < 0)
		return;	/* device gone / busy — retry next tick */

	if (!chip->panel_seen) {
		chip->panel_seen = true;
		memcpy(chip->panel_prev, st, sizeof(st));
		/* Seed the state controls from the first snapshot. */
		in = bf_panel_in_decode((st[2] >> BF_PANEL_IN_SHIFT) & 0x7);
		if (in)
			chip->panel_in = in;
		out = bf_panel_out_decode(st[1] & 0x07);
		if (out)
			chip->panel_out = out;
		chip->panel_mix = !!(st[0] & 0x80);
		chip->panel_saw_fader = (st[2] >> 4) == 0x0;
		chip->panel_dim = !!(st[1] & 0x20);
		return;
	}

	/* Button flash (byte3 over the 0x40 idle base). */
	btn = bf_panel_button_decode(st[3]);
	if (btn)
		chip->panel_button = btn;

	/* Wheel: signed 4-bit wrap delta of the byte2 low nibble — only
	 * while the mode class is unchanged.  A mode switch (IN 0x4x →
	 * fader 0x0x on a MIX press, or the OUT counter carrying 0x8F →
	 * 0x90 — the OUT counter is a full byte, cap_set2.pcap) must not
	 * be read as a wheel jump.  Class: 0 = fader (0x0x), 1 = OUT
	 * (0x8x/0x9x), 2 = IN (0x4x/0x5x/0x6x).
	 */
	cls = (st[2] >> 4) == 0x8 || (st[2] >> 4) == 0x9 ? 1 :
	      (st[2] >> 4) == 0x0 ? 0 : 2;
	pcls = (chip->panel_prev[2] >> 4) == 0x8 ||
	       (chip->panel_prev[2] >> 4) == 0x9 ? 1 :
	       (chip->panel_prev[2] >> 4) == 0x0 ? 0 : 2;
	delta = (int)(st[2] & 0x0f) - (int)(chip->panel_prev[2] & 0x0f);
	if (delta > 8)
		delta -= 16;
	else if (delta < -8)
		delta += 16;
	if (delta && cls == pcls) {
		chip->panel_wheel = clamp(chip->panel_wheel + delta,
					  SHRT_MIN, SHRT_MAX);
		bf_panel_notify(chip, BF_PANEL_KCTL_WHEEL);
		/* Wheel by mode (LINUX-VALIDATION §12, the TotalMix
		 * emulator): MIX → monitoring level, OUT (0x8x/0x9x) → the
		 * selected output master (or its balance while SELECT is
		 * held), IN (0x4x/0x5x/0x6x) → the SELECT-chosen preamp
		 * gain.
		 */
		if (chip->panel_mix)
			bf_panel_mix_wheel(chip, delta);
		else if (chip->panel_sel_hold >= 10 && cls == 1)
			bf_panel_balance_wheel(chip, delta);
		else if (cls == 1)
			bf_panel_out_wheel(chip, delta);
		else if (cls == 2)
			bf_panel_gain_wheel(chip, delta);
	}

	/* Selections — keep the previous when the field is not in range
	 * (the fader-mode readback drops the IN position bits).
	 */
	in = bf_panel_in_decode((st[2] >> BF_PANEL_IN_SHIFT) & 0x7);
	if (in && in != chip->panel_in) {
		chip->panel_in = in;
		bf_panel_notify(chip, BF_PANEL_KCTL_IN);
	}
	out = bf_panel_out_decode(st[1] & 0x07);
	if (out && out != chip->panel_out) {
		chip->panel_out = out;
		bf_panel_notify(chip, BF_PANEL_KCTL_OUT);
	}

	/* SELECT press cycles the channel selection L → R → both → none
	 * → L (manual §5.1).  The state is NOT in the readback
	 * (panelprobe 2026-08-24), so it is tracked host-side.
	 */
	if (st[3] == BF_PANEL_FLASH_SELECT &&
	    chip->panel_prev[3] != BF_PANEL_FLASH_SELECT) {
		chip->panel_select = (chip->panel_select + 1) & 3;
		bf_panel_notify(chip, BF_PANEL_KCTL_SELECT);
	}
	/* SELECT hold (the OUT-balance gesture, manual §5.1 "Output
	 * Balance"): a tap flashes byte3 0x50 for ~2-3 frames at 20 Hz
	 * (~100-150 ms — selhold_probe2), a hold keeps it sustained, and
	 * byte0 does NOT gain the 0x80 engaged bit — so the duration is
	 * the only discriminator: >= 10 ticks (200 ms at 50 Hz) = held.
	 */
	if (st[3] == BF_PANEL_FLASH_SELECT)
		chip->panel_sel_hold++;
	else
		chip->panel_sel_hold = 0;

	/* SET (A) press: host-side 48V phantom toggle on the
	 * SELECT-chosen mic(s) (see bf_panel_set_phantom).
	 */
	if (st[3] == BF_PANEL_FLASH_SET &&
	    chip->panel_prev[3] != BF_PANEL_FLASH_SET)
		bf_panel_set_phantom(chip);

	/* MIX (fader mode) — HOST-latched, like TotalMix (cap_mix.pcap,
	 * cap_select2.pcap): the raw press readback is `0D 0D 41 44` —
	 * byte3 flash 0x44, NO engaged bit, byte2 still in the current
	 * mode.  The host acks the flash with `0x17 0x8480 0x8C80` → the
	 * device latches fader mode (byte0/1 gain the 0x80 bit, byte2 =
	 * 0x00+n counter) and STAYS there after the physical release; the
	 * SECOND 0x44 flash exits it (`0x17 0x0400 0x8000` + `0x8080`).
	 * A mode button (IN/OUT/SET) pressed during MIX makes the device
	 * leave fader mode by itself → same exit writes (the user: IN
	 * must return to gain control).  `panel_saw_fader` gates the
	 * device-driven exit so a pre-ack readback (byte2 still 0x4x
	 * while the 0x44 flash shows) never ends MIX before it started.
	 */
	mix_flash = st[3] == BF_PANEL_FLASH_MIX &&
		    chip->panel_prev[3] != BF_PANEL_FLASH_MIX;
	fader_now = (st[2] >> 4) == 0x0;

	if (mix_flash) {
		if (chip->panel_mix) {
			bf_vendor_write(chip, BF_REQ_PREAMP, 0x0400, 0x8000);
			bf_vendor_write(chip, BF_REQ_PREAMP, 0x0400, 0x8080);
			chip->panel_mix = false;
			chip->panel_saw_fader = false;
		} else {
			int ref, out;
			int m;

			bf_vendor_write(chip, BF_REQ_PREAMP, 0x8480, 0x8c80);
			chip->panel_mix = true;
			/* Seed the monitoring level at the reference
			 * crosspoint's current value so the first wheel
			 * click doesn't jump from −inf (the reference =
			 * the first SELECT-chosen channel of the IN pair;
			 * Opt = the AS1/2 pair).
			 */
			out = chip->panel_out == 3 ? 5 :
			      chip->panel_out == 2 ? 1 : 0;
			ref = chip->panel_in == 3 ? 4 :
			      (chip->panel_in == 2 ? 2 : 0) +
			      (chip->panel_select == 1 ? 1 : 0);
			chip->panel_mix_raw = chip->xpoint[out][ref][0];
			/* Seed the VU display shadow at the CURRENT level
			 * (cap_panel.pcap: TotalMix writes the display value of
			 * the current fader on engage — 10 in that session —
			 * not a hard 0; cap_mix's 0 was because the fader sat
			 * at the bottom).  Only the channels the wheel can move.
			 */
			for (m = 0; m < 4; m++)
				chip->panel_mix_disp[m] = 0;
			if (ref < 4) {
				int disp = bf_mix_display(
					bf_fader_raw_to_db2(chip->panel_mix_raw));

				bf_vendor_write(chip, BF_REQ_GAIN, (u16)disp,
						BF_REG_PANEL_GAIN + ref);
				chip->panel_mix_disp[ref] = disp;
			}
		}
		bf_panel_notify(chip, BF_PANEL_KCTL_MIX);
	}
	if (fader_now) {
		chip->panel_saw_fader = true;
	} else if (chip->panel_mix && chip->panel_saw_fader &&
		   st[3] != BF_PANEL_FLASH_MIX) {
		/* device left fader mode by itself (IN/OUT/SET press) */
		bf_vendor_write(chip, BF_REQ_PREAMP, 0x0400, 0x8000);
		bf_vendor_write(chip, BF_REQ_PREAMP, 0x0400, 0x8080);
		chip->panel_mix = false;
		chip->panel_saw_fader = false;
		bf_panel_notify(chip, BF_PANEL_KCTL_MIX);
	}

	dim = !!(st[1] & 0x20);
	if (dim != chip->panel_dim) {
		chip->panel_dim = dim;
		bf_panel_notify(chip, BF_PANEL_KCTL_DIM);
	}

	memcpy(chip->panel_prev, st, sizeof(st));
}

void babyface_panel_work(struct work_struct *work)
{
	struct snd_usb_babyface *chip = container_of(work,
			struct snd_usb_babyface, panel_work.work);

	if (chip->shutdown)
		return;
	bf_panel_tick(chip);
	schedule_delayed_work(&chip->panel_work,
			      msecs_to_jiffies(BF_PANEL_POLL_MS));
}

void babyface_panel_start(struct snd_usb_babyface *chip)
{
	chip->panel_seen = false;
	schedule_delayed_work(&chip->panel_work, 0);
}

void babyface_panel_stop(struct snd_usb_babyface *chip)
{
	cancel_delayed_work_sync(&chip->panel_work);
}

/* ── controls ────────────────────────────────────────────────── */

/* The button/wheel controls hold the LATEST state and are NOT consumed
 * on read: wireplumber subscribes to every notifying control and reads
 * it, so a clear-on-get would let another reader eat the event.  Each
 * consumer tracks its own baseline and acts on changes (the button is a
 * last-press code, the wheel an accumulated signed delta).  VOLATILE
 * keeps alsactl from caching them.
 */
static int bf_panel_button_info(struct snd_kcontrol *kctl,
				struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = BF_PANEL_BTN_DIM;
	uinfo->value.integer.step = 1;
	return 0;
}

static int bf_panel_button_get(struct snd_kcontrol *kctl,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);

	ucontrol->value.integer.value[0] = chip->panel_button;
	return 0;
}

static int bf_panel_wheel_info(struct snd_kcontrol *kctl,
			       struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = SHRT_MIN;
	uinfo->value.integer.max = SHRT_MAX;
	uinfo->value.integer.step = 1;
	return 0;
}

static int bf_panel_wheel_get(struct snd_kcontrol *kctl,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);

	ucontrol->value.integer.value[0] = chip->panel_wheel;
	return 0;
}

static int bf_panel_in_info(struct snd_kcontrol *kctl,
			    struct snd_ctl_elem_info *uinfo)
{
	return snd_ctl_enum_info(uinfo, 1, 4, bf_panel_in_texts);
}

static int bf_panel_in_get(struct snd_kcontrol *kctl,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);

	ucontrol->value.enumerated.item[0] = chip->panel_in;
	return 0;
}

static int bf_panel_out_info(struct snd_kcontrol *kctl,
			     struct snd_ctl_elem_info *uinfo)
{
	return snd_ctl_enum_info(uinfo, 1, 4, bf_panel_out_texts);
}

static int bf_panel_out_get(struct snd_kcontrol *kctl,
			    struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);

	ucontrol->value.enumerated.item[0] = chip->panel_out;
	return 0;
}

static int bf_panel_select_info(struct snd_kcontrol *kctl,
				   struct snd_ctl_elem_info *uinfo)
{
	return snd_ctl_enum_info(uinfo, 1, 4, bf_panel_select_texts);
}

static int bf_panel_select_get(struct snd_kcontrol *kctl,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);

	ucontrol->value.enumerated.item[0] = chip->panel_select;
	return 0;
}

/* Shared boolean get — private_value selects mix (0) / dim (1). */
static int bf_panel_bool_get(struct snd_kcontrol *kctl,
			     struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);

	ucontrol->value.integer.value[0] =
		kctl->private_value ? chip->panel_dim : chip->panel_mix;
	return 0;
}

int babyface_create_panel(struct snd_usb_babyface *chip)
{
	struct snd_kcontrol *kctl;
	int err;

	memset(chip->panel_kctl, 0, sizeof(chip->panel_kctl));

	kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Front Panel Button",
		.access = SNDRV_CTL_ELEM_ACCESS_READ |
			  SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.info = bf_panel_button_info,
		.get = bf_panel_button_get,
	}, chip);
	err = snd_ctl_add(chip->card, kctl);
	if (err < 0)
		return err;
	chip->panel_kctl[BF_PANEL_KCTL_BUTTON] = kctl;

	kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Front Panel Wheel",
		.access = SNDRV_CTL_ELEM_ACCESS_READ |
			  SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.info = bf_panel_wheel_info,
		.get = bf_panel_wheel_get,
	}, chip);
	err = snd_ctl_add(chip->card, kctl);
	if (err < 0)
		return err;
	chip->panel_kctl[BF_PANEL_KCTL_WHEEL] = kctl;

	kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Front Panel In",
		.access = SNDRV_CTL_ELEM_ACCESS_READ |
			  SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.info = bf_panel_in_info,
		.get = bf_panel_in_get,
	}, chip);
	err = snd_ctl_add(chip->card, kctl);
	if (err < 0)
		return err;
	chip->panel_kctl[BF_PANEL_KCTL_IN] = kctl;

	kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Front Panel Out",
		.access = SNDRV_CTL_ELEM_ACCESS_READ |
			  SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.info = bf_panel_out_info,
		.get = bf_panel_out_get,
	}, chip);
	err = snd_ctl_add(chip->card, kctl);
	if (err < 0)
		return err;
	chip->panel_kctl[BF_PANEL_KCTL_OUT] = kctl;

	kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Front Panel Mix",
		.access = SNDRV_CTL_ELEM_ACCESS_READ |
			  SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.info = snd_ctl_boolean_mono_info,
		.get = bf_panel_bool_get,
	}, chip);
	err = snd_ctl_add(chip->card, kctl);
	if (err < 0)
		return err;
	chip->panel_kctl[BF_PANEL_KCTL_MIX] = kctl;

	kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Front Panel Dim",
		.access = SNDRV_CTL_ELEM_ACCESS_READ |
			  SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.info = snd_ctl_boolean_mono_info,
		.get = bf_panel_bool_get,
		.private_value = 1,
	}, chip);
	err = snd_ctl_add(chip->card, kctl);
	if (err < 0)
		return err;
	chip->panel_kctl[BF_PANEL_KCTL_DIM] = kctl;

	kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Front Panel Select",
		.access = SNDRV_CTL_ELEM_ACCESS_READ |
			  SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.info = bf_panel_select_info,
		.get = bf_panel_select_get,
	}, chip);
	err = snd_ctl_add(chip->card, kctl);
	if (err < 0)
		return err;
	chip->panel_kctl[BF_PANEL_KCTL_SELECT] = kctl;

	return 0;
}
