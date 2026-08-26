// SPDX-License-Identifier: GPL-2.0-only
/*
 * RME Babyface Pro FS — proprietary-mode USB audio driver
 * Mixer-state persistence across re-probes + resume re-apply.  See snd-usb-babyface-pro.h for the shared state.
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

/* ── mixer-state persistence across interface re-probes ────────
 * A userspace client can claim the proprietary interface via usbfs
 * (USBDEVFS_DISCONNECT_CLAIM — seen with PipeWire grabbing the
 * device when a stream targets the sink, and with the TuxMix
 * user-space daemon's libusb).  That detaches us and the card
 * disappears for the duration; on release the interface re-probes.
 * The device keeps its registers across the detach, but our cold
 * init clears them — so save the mixer state at disconnect and
 * restore it at the next probe.
 */

static LIST_HEAD(bf_saved_list);
static DEFINE_MUTEX(bf_saved_mutex);

/* Re-apply the whole cached mixer state after a resume (the device
 * lost its registers across a system suspend — TotalMix does the same
 * re-apply).  Caller holds chip->mutex.
 */
int babyface_restore_state(struct snd_usb_babyface *chip)
{
	int out, src, mic, ret;
	u16 flag;

	/* Preamp state + commit. */
	ret = bf_preamp_state_write(chip);
	if (ret < 0)
		return ret;

	/* The four mic gains (the counter restarts). */
	for (mic = 0; mic < 4; mic++) {
		u8 counter = (mic % 3 == 0) ? 0x20 : (mic % 3 == 1) ? 0x00 : 0x40;

		ret = bf_vendor_write(chip, BF_REQ_GAIN,
				      (u16)((bf_gain_raw(mic, chip->gain[mic]) & 0x1f) |
					    counter),
				      BF_REG_GAIN + mic);
		if (ret < 0)
			return ret;
	}
	chip->gain_cycle = 1;

	/* Masters (8-bit = the real volume) + mutes. */
	ret = bf_apply_masters(chip);
	if (ret < 0)
		return ret;

	/* Crosspoints (canonical out → register block). */
	for (out = 0; out < 6; out++) {
		unsigned int blk = bf_xpoint_block[out];

		for (src = 0; src < 14; src++) {
			flag = bf_flag_cycle[chip->flag_cnt];
			chip->flag_cnt = (chip->flag_cnt + 1) & 3;
			ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT,
					      chip->xpoint[out][src][0],
					      (BF_REG_CROSS_BASE_L +
					       BF_REG_CROSS_STRIDE * blk +
					       bf_sources[src].idx_l) | flag);
			if (ret < 0)
				return ret;
			ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT,
					      chip->xpoint[out][src][1],
					      (BF_REG_CROSS_BASE_R +
					       BF_REG_CROSS_STRIDE * blk +
					       bf_sources[src].idx_r) | flag);
			if (ret < 0)
				return ret;
		}
		ret = bf_crosspoint_clear_cross(chip, blk);
		if (ret < 0)
			return ret;
	}

	/* Pitch (the DDS quad) + the clock keepalive. */
	if (chip->pitch) {
		u32 dds24 = (12800000000u + (u32)(1000 + chip->pitch) / 2) /
			    (u32)(1000 + chip->pitch);
		u16 dds16 = dds24 >> 8;
		u16 frac = dds24 & 0xff;

		ret = bf_vendor_write(chip, BF_REQ_DDS, dds16, (frac << 8) | 0);
		if (ret < 0)
			return ret;
		ret = bf_vendor_write(chip, BF_REQ_DDS,
				      (u16)((dds16 * 72562ull + 50000) / 100000), 0x0001);
		if (ret < 0)
			return ret;
		ret = bf_vendor_write(chip, BF_REQ_DDS, (u16)((dds16 * 2 + 1) / 3),
				      0x0002);
		if (ret < 0)
			return ret;
		ret = bf_vendor_write(chip, BF_REQ_DDS, 0x7cff, 0x0003);
		if (ret < 0)
			return ret;
	}
	return bf_vendor_write(chip, BF_REQ_KEEPALIVE, 0x0001,
			       BF_REG_KEEPALIVE_SETTINGS);
}

/* Re-apply the non-master flags (loopback / AN1>2 / link / width /
 * FX send / MS) after a state restore.  The write patterns mirror the
 * corresponding _put() handlers.  Caller holds chip->mutex.
 */
int bf_state_apply_flags(struct snd_usb_babyface *chip)
{
	int out, ret, on_out = -1;
	u16 l, r;

	/* Loopback: the full 30-channel map from the cached state (the
	 * single-active invariant keeps at most one pair at 0x0001).
	 */
	for (out = 0; out < 6; out++) {
		if (chip->loopback[out]) {
			on_out = out;
			break;
		}
	}
	ret = bf_loopback_write_map(chip, on_out, on_out >= 0);
	if (ret < 0)
		return ret;

	ret = bf_vendor_write(chip, BF_REQ_PREAMP,
			      (chip->linked ? 0x0400 : 0x0000) |
			      (chip->an12 ? 0x1000 : 0x0000), 0x1000);
	if (ret < 0)
		return ret;
	ret = bf_vendor_write(chip, BF_REQ_PREAMP_COMMIT, 0x0000, 0x0000);
	if (ret < 0)
		return ret;

	l = (u16)(((0x2000 * (100 + chip->width) / 2) + 50) / 100);
	r = 0x2000 - l;
	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, l, 0x0000);
	if (ret < 0)
		return ret;
	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, r, 0x001a);
	if (ret < 0)
		return ret;
	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, r, 0x0001);
	if (ret < 0)
		return ret;
	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, l, 0x001b);
	if (ret < 0)
		return ret;

	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, chip->fx_send, 0x0138);
	if (ret < 0)
		return ret;
	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, chip->fx_send, 0x0153);
	if (ret < 0)
		return ret;

	if (chip->ms_proc) {
		/* Same ON pattern as bf_ms_put (cap_ms2.pcap): mute the AN2
		 * (side) crosspoints, both maps.
		 */
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, 0x0000, 0x0035);
		if (ret < 0)
			return ret;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, 0x0000, 0x004f);
		if (ret < 0)
			return ret;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, 0x0000, 0x0001);
		if (ret < 0)
			return ret;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, 0x0000, 0x001b);
		if (ret < 0)
			return ret;
	}

	/* Re-apply an engaged DIM (the fixed -20 dB Phones pair + flag). */
	if (chip->dim) {
		ret = bf_vendor_write(chip, BF_REQ_GAIN, 0xcb,
				      BF_REG_MASTER_8 + 2 * 1);
		if (ret < 0)
			return ret;
		ret = bf_vendor_write(chip, BF_REQ_GAIN, 0xcb,
				      BF_REG_MASTER_8 + 2 * 1 + 1);
		if (ret < 0)
			return ret;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, 0x0333,
				      (BF_REG_MASTER_16 + 2 * 1) |
				      bf_flag_cycle[chip->flag_cnt]);
		if (ret < 0)
			return ret;
		chip->flag_cnt = (chip->flag_cnt + 1) & 3;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, 0x0333,
				      (BF_REG_MASTER_16 + 2 * 1 + 1) |
				      bf_flag_cycle[chip->flag_cnt]);
		if (ret < 0)
			return ret;
		chip->flag_cnt = (chip->flag_cnt + 1) & 3;
		ret = bf_vendor_write(chip, BF_REQ_PREAMP, 0x2000, 0x2000);
		if (ret < 0)
			return ret;
	}
	return 0;
}

void bf_state_save(struct snd_usb_babyface *chip)
{
	struct bf_saved *s;
	const char *key = chip->dev->serial ? chip->dev->serial :
			  dev_name(&chip->dev->dev);
	bool found = false;

	mutex_lock(&bf_saved_mutex);
	list_for_each_entry(s, &bf_saved_list, list) {
		if (strcmp(s->key, key))
			continue;
		found = true;
		break;
	}
	if (!found) {
		s = kzalloc_obj(*s, GFP_KERNEL);
		if (!s) {
			mutex_unlock(&bf_saved_mutex);
			return;
		}
		strscpy(s->key, key, sizeof(s->key));
		list_add_tail(&s->list, &bf_saved_list);
	}

	s->preamp = chip->preamp;
	memcpy(s->gain, chip->gain, sizeof(s->gain));
	s->gain_cycle = chip->gain_cycle;
	s->flag_cnt = chip->flag_cnt;
	memcpy(s->master, chip->master, sizeof(s->master));
	memcpy(s->muted, chip->muted, sizeof(s->muted));
	memcpy(s->xpoint, chip->xpoint, sizeof(s->xpoint));
	s->pitch = chip->pitch;
	memcpy(s->loopback, chip->loopback, sizeof(s->loopback));
	s->an12 = chip->an12;
	s->linked = chip->linked;
	s->ms_proc = chip->ms_proc;
	s->width = chip->width;
	s->fx_send = chip->fx_send;
	s->dim = chip->dim;
	mutex_unlock(&bf_saved_mutex);
}

/* Copy a saved state (if any) into a freshly probed chip and push it
 * to the device.  Returns 1 when restored, -ENOENT when there is none,
 * or a negative error from the vendor writes.
 */
int bf_state_restore(struct snd_usb_babyface *chip)
{
	struct bf_saved *s;
	const char *key = chip->dev->serial ? chip->dev->serial :
			  dev_name(&chip->dev->dev);
	int ret = -ENOENT;

	mutex_lock(&bf_saved_mutex);
	list_for_each_entry(s, &bf_saved_list, list) {
		if (strcmp(s->key, key))
			continue;
		chip->preamp = s->preamp;
		memcpy(chip->gain, s->gain, sizeof(chip->gain));
		chip->gain_cycle = s->gain_cycle;
		chip->flag_cnt = s->flag_cnt;
		memcpy(chip->master, s->master, sizeof(chip->master));
		memcpy(chip->muted, s->muted, sizeof(chip->muted));
		memcpy(chip->xpoint, s->xpoint, sizeof(chip->xpoint));
		chip->pitch = s->pitch;
		memcpy(chip->loopback, s->loopback, sizeof(chip->loopback));
		chip->an12 = s->an12;
		chip->linked = s->linked;
		chip->ms_proc = s->ms_proc;
		chip->width = s->width;
		chip->fx_send = s->fx_send;
		chip->dim = s->dim;
		ret = 1;
		break;
	}
	mutex_unlock(&bf_saved_mutex);
	if (ret != 1)
		return ret;

	mutex_lock(&chip->mutex);
	ret = babyface_restore_state(chip);
	if (ret == 0)
		ret = bf_state_apply_flags(chip);
	mutex_unlock(&chip->mutex);
	return ret ? ret : 1;
}

void bf_state_purge(void)
{
	struct bf_saved *s, *tmp;

	mutex_lock(&bf_saved_mutex);
	list_for_each_entry_safe(s, tmp, &bf_saved_list, list) {
		list_del(&s->list);
		kfree(s);
	}
	mutex_unlock(&bf_saved_mutex);
}
