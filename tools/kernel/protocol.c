// SPDX-License-Identifier: GPL-2.0-only
/*
 * RME Babyface Pro FS — proprietary-mode USB audio driver
 * USB vendor requests, cold init, rate table.  See snd-usb-babyface-pro.h for the shared state.
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

/* The transaction-flag counter cycle on 16-bit writes. */
const u16 bf_flag_cycle[4] = { 0xc000, 0x4000, 0x8000, 0x0000 };

/* ── sample-rate / alt classes ─────────────────────────────── */


static const struct bf_rate bf_rates[] = {
	{  32000, BF_ALT_1, 56,  8 },
	{  44100, BF_ALT_1, 56,  8 },
	{  48000, BF_ALT_1, 56,  8 },
	{  64000, BF_ALT_1, 56,  8 },
	{  88200, BF_ALT_1, 56,  8 },
	{  96000, BF_ALT_2, 40, 16 },
	{ 128000, BF_ALT_2, 40, 16 },
	{ 176400, BF_ALT_3, 32, 32 },
	{ 192000, BF_ALT_3, 32, 32 },
};

static const unsigned int bf_rate_list[ARRAY_SIZE(bf_rates)] = {
	32000, 44100, 48000, 64000, 88200,
	96000, 128000, 176400, 192000,
};

const struct snd_pcm_hw_constraint_list bf_rates_constraint = {
	.count = ARRAY_SIZE(bf_rate_list),
	.list = bf_rate_list,
	.mask = 0,
};

const struct bf_rate *bf_rate_lookup(unsigned int rate)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(bf_rates); i++)
		if (bf_rates[i].rate == rate)
			return &bf_rates[i];
	return NULL;
}

/* ── vendor requests ───────────────────────────────────────── */

int bf_vendor_write(struct snd_usb_babyface *chip, u8 req, u16 val, u16 idx)
{
	return usb_control_msg_send(chip->dev, 0, req,
				    USB_DIR_OUT | USB_TYPE_VENDOR |
				    USB_RECIP_DEVICE,
				    val, idx, NULL, 0, 1000, GFP_KERNEL);
}

int bf_vendor_read(struct snd_usb_babyface *chip, u8 req, u16 idx, u8 *buf)
{
	return usb_control_msg_recv(chip->dev, 0, req,
				    USB_DIR_IN | USB_TYPE_VENDOR |
				    USB_RECIP_DEVICE,
				    0, idx, buf, 4, 1000, GFP_KERNEL);
}

/* The cold-start session init (cap_coldplug.pcap), verbatim from the
 * user-space reference (protocol::streaming_init).  Without it the
 * firmware never validates a stream.
 */
int bf_cold_init(struct snd_usb_babyface *chip)
{
	int ret, i;

	for (i = 0; i <= 0x3d; i++) {
		if (i == 0x1e || i == 0x1f)
			continue;
		ret = bf_vendor_write(chip, BF_REQ_REG_CLEAR, 0x0000, i);
		if (ret < 0)
			return ret;
	}
	/* 48-kHz DDS clock quads (banked 0x1B). */
	ret = bf_vendor_write(chip, BF_REQ_DDS, 0xc350, 0x0000);
	if (ret < 0)
		return ret;
	ret = bf_vendor_write(chip, BF_REQ_DDS, 0x8db8, 0xd201);
	if (ret < 0)
		return ret;
	ret = bf_vendor_write(chip, BF_REQ_DDS, 0x8234, 0xd302);
	if (ret < 0)
		return ret;
	ret = bf_vendor_write(chip, BF_REQ_DDS, 0x7cff, 0xf803);
	if (ret < 0)
		return ret;
	/* 0x1C status — the hardware-validated reference (protocol::
	 * streaming_init) sends it as an OUT write; Windows reads it.
	 * Both are tolerated; match the validated path.
	 */
	ret = bf_vendor_write(chip, BF_REQ_STATUS_2, 0x0000, 0x0000);
	if (ret < 0)
		return ret;
	ret = bf_vendor_write(chip, BF_REQ_KEEPALIVE, 0x0021, BF_REG_KEEPALIVE_INIT);
	if (ret < 0)
		return ret;
	/* 0x17 wIdx=0x0000 does NOT touch the preamp state (0x003F). */
	ret = bf_vendor_write(chip, BF_REQ_PREAMP, 0x000c, 0x0000);
	if (ret < 0)
		return ret;
	ret = bf_vendor_write(chip, BF_REQ_PREAMP_COMMIT, 0x0000, 0x0000);
	if (ret < 0)
		return ret;
	for (i = 0; i < 2; i++) {
		ret = bf_vendor_write(chip, BF_REQ_KEEPALIVE, 0x0000, 0x3000);
		if (ret < 0)
			return ret;
	}
	for (i = 0; i < 3; i++) {
		ret = bf_vendor_write(chip, BF_REQ_KEEPALIVE, 0x0800, 0x0800);
		if (ret < 0)
			return ret;
	}
	return 0;
}

/* The 0x16 cold-init clear covers only 0x00-0x3D — the "cross"
 * registers of a block (L-reg odd / R-reg even of the stereo
 * sources) survive from the previous session and would sum L+R into
 * BOTH channels of the output (mono).  Zero them explicitly: 10 odd
 * L-registers (5,7,…23) + 10 even R-registers (4,6,…22).
 */
int bf_crosspoint_clear_cross(struct snd_usb_babyface *chip,
				     unsigned int blk)
{
	int ret, k;
	u16 flag;

	for (k = 5; k < 24; k += 2) {
		flag = bf_flag_cycle[chip->flag_cnt];
		chip->flag_cnt = (chip->flag_cnt + 1) & 3;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, 0x0000,
				      (BF_REG_CROSS_BASE_L +
				       BF_REG_CROSS_STRIDE * blk + k) | flag);
		if (ret < 0)
			return ret;
	}
	for (k = 4; k < 24; k += 2) {
		flag = bf_flag_cycle[chip->flag_cnt];
		chip->flag_cnt = (chip->flag_cnt + 1) & 3;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, 0x0000,
				      (BF_REG_CROSS_BASE_R +
				       BF_REG_CROSS_STRIDE * blk + k) | flag);
		if (ret < 0)
			return ret;
	}
	return 0;
}
