// SPDX-License-Identifier: GPL-2.0-only
/*
 * RME Babyface Pro FS — proprietary-mode USB audio driver
 * PCM: interrupt-URB stream, copy, trigger, pointer.  See snd-usb-babyface-pro.h for the shared state.
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

/* ── stream (interrupt URBs, caiaq-style) ──────────────────── */

static bool babyface_capture_copy(struct snd_usb_babyface *chip,
				   struct snd_pcm_substream *subs,
				   const u8 *data, unsigned int frames)
{
	struct snd_pcm_runtime *rt = subs->runtime;
	unsigned int buf_frames = rt->buffer_size;
	unsigned int words = chip->frame_bytes / 4;
	unsigned int chans = rt->channels;
	unsigned int pos, f, i;
	unsigned long new_period;
	bool crossed = false;
	u8 *dst;

	spin_lock(&chip->lock);
	pos = chip->hw_ptr[SNDRV_PCM_STREAM_CAPTURE] % buf_frames;
	for (f = 0; f < frames; f++) {
		const u32 *w = (const u32 *)(data + f * chip->frame_bytes);

		dst = rt->dma_area + frames_to_bytes(rt, pos);
		for (i = 0; i < chans; i++) {
			/* Channel map: app ch0-3 = device words 0-3 (AN1-4);
			 * app ch4-9 = words 6-11 (ADAT/SPDIF); app ch10/11 =
			 * words 12/13 = a FIXED-GAIN playback tap (observed
			 * 2026-08-25: the playback echoes there at ~−27 dB,
			 * independent of the output masters — NOT the output
			 * bus; the ADAT/SPDIF range is words 6-11 only).  The
			 * device words 4/5 are a fixed marker, not audio —
			 * skipped.  At 96/192 kHz the frame has fewer words;
			 * missing ones read as zero.
			 */
			static const u8 map[12] = { 0, 1, 2, 3, 6, 7, 8, 9,
						   10, 11, 12, 13 };
			u8 wi = i < 12 ? map[i] : 0xff;
			s32 s = 0;

			if (wi < words) {
				/* 24-bit sample in bytes 1-3; arithmetic shift
				 * sign-extends from bit 23.  S24_LE container.
				 */
				s = (s32)le32_to_cpu(w[wi]) >> 8;
			}
			put_unaligned_le32((u32)s, dst + i * 4);
		}
		pos++;
		if (pos >= buf_frames)
			pos = 0;
	}
	chip->hw_ptr[SNDRV_PCM_STREAM_CAPTURE] += frames;
	new_period = chip->hw_ptr[SNDRV_PCM_STREAM_CAPTURE] / rt->period_size;
	if (new_period != chip->prev_period[SNDRV_PCM_STREAM_CAPTURE]) {
		chip->prev_period[SNDRV_PCM_STREAM_CAPTURE] = new_period;
		crossed = true;
	}
	spin_unlock(&chip->lock);

	return crossed;
}

static bool babyface_playback_copy(struct snd_usb_babyface *chip,
				   struct snd_pcm_substream *subs,
				   u8 *data, unsigned int frames)
{
	struct snd_pcm_runtime *rt = subs->runtime;
	unsigned int buf_frames = rt->buffer_size;
	unsigned int words = chip->frame_bytes / 4;
	unsigned int chans = rt->channels;
	unsigned int pos, f, i;
	unsigned long new_period;
	bool crossed = false;
	const u8 *src;

	spin_lock(&chip->lock);
	/* Clamp to what the app has actually written: the in-flight URBs
	 * (nurbs × frames_per_urb) can exceed the app ring, and without
	 * this the driver advances hw_ptr past appl_ptr — the ALSA core
	 * then flags a spurious XRUN on the next app interaction even
	 * though the app refills on schedule (seen at period 16-128 /
	 * 96-192 kHz with nurbs=16).  The device just repeats the last
	 * frames (stale audio) instead of corrupting the stream state.
	 * NB: subtract the unbounded counters directly — modulo arithmetic
	 * is ambiguous at exact buffer multiples (appl=512, hw=0 → both
	 * wrap to 0).
	 */
	{
		snd_pcm_sframes_t data =
			(snd_pcm_sframes_t)(rt->control->appl_ptr -
					    chip->hw_ptr[SNDRV_PCM_STREAM_PLAYBACK]);
		if (data < 0)
			data = 0;
		if (data > (snd_pcm_sframes_t)buf_frames)
			data = (snd_pcm_sframes_t)buf_frames;
		if ((unsigned int)data < frames)
			frames = (unsigned int)data;
	}
	pos = chip->hw_ptr[SNDRV_PCM_STREAM_PLAYBACK] % buf_frames;
	for (f = 0; f < frames; f++) {
		u32 *w = (u32 *)(data + f * chip->frame_bytes);

		src = rt->dma_area + frames_to_bytes(rt, pos);
		/* App ch n feeds the device word n (PB1-6 = words 0-11);
		 * words 12/13 stay zero.  At 96/192 kHz the frame is
		 * shorter — the extra app channels are dropped.
		 */
		for (i = 0; i < chans && i < words; i++) {
			u32 s = get_unaligned_le32(src + i * 4);

			/* 24-bit sample into bytes 1-3, byte 0 = 0. */
			w[i] = cpu_to_le32((s & 0x00ffffff) << 8);
		}
		for (; i < words; i++)
			w[i] = 0;
		pos++;
		if (pos >= buf_frames)
			pos = 0;
	}
	chip->hw_ptr[SNDRV_PCM_STREAM_PLAYBACK] += frames;
	new_period = chip->hw_ptr[SNDRV_PCM_STREAM_PLAYBACK] / rt->period_size;
	if (new_period != chip->prev_period[SNDRV_PCM_STREAM_PLAYBACK]) {
		chip->prev_period[SNDRV_PCM_STREAM_PLAYBACK] = new_period;
		crossed = true;
	}
	spin_unlock(&chip->lock);

	return crossed;
}

static void babyface_complete_in(struct urb *urb)
{
	struct snd_usb_babyface *chip = urb->context;
	struct snd_pcm_substream *subs;
	unsigned long flags;
	unsigned int frames;
	bool crossed = false;
	int ret;

	if (urb->status < 0) {
		if (urb->status == -ESHUTDOWN || urb->status == -ENOENT ||
		    urb->status == -ECONNRESET)
			return;		/* killed */
		dev_dbg_ratelimited(&chip->dev->dev, "IN urb status %d\n",
				    urb->status);
		if (atomic_inc_return(&chip->urb_err) >= BF_URB_ERR_STOP)
			schedule_work(&chip->stream_work);
		goto resubmit;
	}
	atomic_set(&chip->urb_err, 0);

	subs = READ_ONCE(chip->subs[SNDRV_PCM_STREAM_CAPTURE]);
	if (subs) {
		snd_pcm_stream_lock_irqsave(subs, flags);
		if (snd_pcm_running(subs)) {
			frames = urb->actual_length / chip->frame_bytes;
			if (frames)
				crossed = babyface_capture_copy(chip, subs,
							       urb->transfer_buffer,
							       frames);
		}
		snd_pcm_stream_unlock_irqrestore(subs, flags);
		if (crossed)
			snd_pcm_period_elapsed(subs);
	}
resubmit:
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret < 0) {
		dev_err_ratelimited(&chip->dev->dev,
				    "IN resubmit failed: %d\n", ret);
		if (atomic_inc_return(&chip->urb_err) >= BF_URB_ERR_STOP)
			schedule_work(&chip->stream_work);
	}
}

static void babyface_complete_out(struct urb *urb)
{
	struct snd_usb_babyface *chip = urb->context;
	struct snd_pcm_substream *subs;
	unsigned long flags;
	unsigned int frames;
	bool crossed = false;
	int ret;

	if (urb->status < 0) {
		if (urb->status == -ESHUTDOWN || urb->status == -ENOENT ||
		    urb->status == -ECONNRESET)
			return;		/* killed */
		dev_dbg_ratelimited(&chip->dev->dev, "OUT urb status %d\n",
				    urb->status);
		if (atomic_inc_return(&chip->urb_err) >= BF_URB_ERR_STOP)
			schedule_work(&chip->stream_work);
		goto resubmit;
	}
	atomic_set(&chip->urb_err, 0);

	subs = READ_ONCE(chip->subs[SNDRV_PCM_STREAM_PLAYBACK]);
	if (subs) {
		snd_pcm_stream_lock_irqsave(subs, flags);
		if (snd_pcm_running(subs)) {
			frames = chip->frames_per_urb;
			crossed = babyface_playback_copy(chip, subs,
							urb->transfer_buffer, frames);
		}
		snd_pcm_stream_unlock_irqrestore(subs, flags);
		if (crossed)
			snd_pcm_period_elapsed(subs);
	} else {
		/* No consumer: silence the OUT frames. */
		memset(urb->transfer_buffer, 0, urb->transfer_buffer_length);
	}
resubmit:
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret < 0) {
		dev_err_ratelimited(&chip->dev->dev,
				    "OUT resubmit failed: %d\n", ret);
		if (atomic_inc_return(&chip->urb_err) >= BF_URB_ERR_STOP)
			schedule_work(&chip->stream_work);
	}
}

void babyface_stream_kill(struct snd_usb_babyface *chip)
{
	int i;

	for (i = 0; i < chip->nurbs; i++) {
		usb_kill_urb(chip->urbs_in[i]);
		usb_kill_urb(chip->urbs_out[i]);
	}
	chip->streaming = false;
}

/* Stream start/stop run in process context (control transfers sleep).
 * The trigger only toggles stream_users and schedules this work.
 */

/* Stop both PCM substreams (if running) so apps blocked in read/write
 * wake with a clean error: XRUN for a recoverable stream error, or
 * DISCONNECTED when the card is going away.
 */
void babyface_pcm_stop_both(struct snd_usb_babyface *chip, int state)
{
	int s;

	for (s = 0; s < 2; s++) {
		struct snd_pcm_substream *subs = READ_ONCE(chip->subs[s]);

		if (subs && snd_pcm_running(subs))
			snd_pcm_stop(subs, state);
	}
}

/* Re-count stream_users from the substream running states.  The apps
 * can recover (re-prepare + trigger) while the stream work runs, so a
 * hard `= 0` would wipe a fresh increment and leave a RUNNING
 * substream with no URBs (hang).  Called on the error paths with the
 * mutex held.
 */
static void bf_recount_users(struct snd_usb_babyface *chip)
{
	unsigned long flags;
	int s, users = 0;

	for (s = 0; s < 2; s++) {
		struct snd_pcm_substream *subs = READ_ONCE(chip->subs[s]);

		if (subs && snd_pcm_running(subs))
			users++;
	}
	spin_lock_irqsave(&chip->lock, flags);
	chip->stream_users = users;
	spin_unlock_irqrestore(&chip->lock, flags);
}

void babyface_stream_work(struct work_struct *work)
{
	struct snd_usb_babyface *chip =
		container_of(work, struct snd_usb_babyface, stream_work);
	unsigned int urbsize = chip->frame_bytes * chip->frames_per_urb;
	unsigned long flags;
	int i, ret;
	int users;

	mutex_lock(&chip->mutex);

	if (chip->shutdown) {
		mutex_unlock(&chip->mutex);
		return;
	}

	/* Persistent URB errors (bad link, device wedged): stop the stream
	 * and wake the apps with -EPIPE.  stream_users is re-counted from
	 * the (now stopped) substreams so an app recovery (prepare+start)
	 * re-arms the session from a clean slate.
	 */
	if (atomic_read(&chip->urb_err) >= BF_URB_ERR_STOP) {
		dev_err(&chip->dev->dev,
			"stream error: %d consecutive bad URBs, stopping (apps re-arm)\n",
			BF_URB_ERR_STOP);
		babyface_pcm_stop_both(chip, SNDRV_PCM_STATE_XRUN);
		if (chip->streaming)
			babyface_stream_kill(chip);
		bf_recount_users(chip);
		atomic_set(&chip->urb_err, 0);
		mutex_unlock(&chip->mutex);
		return;
	}

	spin_lock_irqsave(&chip->lock, flags);
	users = chip->stream_users;
	spin_unlock_irqrestore(&chip->lock, flags);

	if (users > 0 && !chip->streaming) {
		/* The firmware only validates a stream session that is
		 * preceded by the full cold-init (the user-space reference
		 * sends streaming_init at every session start — without it
		 * the outputs stay silent).  The 0x16 clear wipes the mixer
		 * registers, so the cached state is re-applied after the arm.
		 */
		ret = bf_cold_init(chip);
		if (ret < 0)
			goto err;

		/* Stream trigger pair (cap_audio): 0x10 0x8000 + 0x1D. */
		ret = bf_vendor_write(chip, BF_REQ_KEEPALIVE, 0x0000, 0x8000);
		if (ret < 0)
			goto err;
		ret = bf_vendor_write(chip, BF_REQ_SESSION_START, 0x0000, 0x0000);
		if (ret < 0)
			goto err;

		for (i = 0; i < chip->nurbs; i++) {
			usb_fill_int_urb(chip->urbs_in[i], chip->dev,
					 usb_rcvintpipe(chip->dev, BF_EP_IN),
					 chip->buf_in[i], urbsize,
					 babyface_complete_in, chip, 1);
			usb_fill_int_urb(chip->urbs_out[i], chip->dev,
					 usb_sndintpipe(chip->dev, BF_EP_OUT),
					 chip->buf_out[i], urbsize,
					 babyface_complete_out, chip, 1);
		}
		for (i = 0; i < chip->nurbs; i++) {
			ret = usb_submit_urb(chip->urbs_in[i], GFP_KERNEL);
			if (ret < 0)
				goto err;
			ret = usb_submit_urb(chip->urbs_out[i], GFP_KERNEL);
			if (ret < 0)
				goto err;
		}
		/* Session arm (cap_audio frame 5829, after the URBs). */
		ret = bf_vendor_write(chip, BF_REQ_SESSION_ARM, 0x0000, 0xc000);
		if (ret < 0)
			goto err;

		/* The cold init above cleared the mixer registers; push the
		 * cached state back (preamp, gains, masters, crosspoints,
		 * pitch) so the session starts at the user's levels.
		 */
		ret = babyface_restore_state(chip);
		if (ret < 0)
			goto err;

		/* The 0x16 clear also wipes the flag registers (loopback,
		 * AN1>2, stereo link, width, FX send, MS) — re-apply them.
		 */
		ret = bf_state_apply_flags(chip);
		if (ret < 0)
			goto err;

		chip->streaming = true;
		dev_dbg(&chip->dev->dev, "stream started (%u frames/URB, %u URBs)\n",
			chip->frames_per_urb, chip->nurbs);
	} else if (users == 0 && chip->streaming) {
		babyface_stream_kill(chip);
		dev_dbg(&chip->dev->dev, "stream stopped\n");
	}

	mutex_unlock(&chip->mutex);
	return;

err:
	dev_err(&chip->dev->dev, "failed to start stream: %d\n", ret);
	babyface_stream_kill(chip);
	/* The apps already got a successful trigger — wake them with an
	 * XRUN so a failed start (device wedged, cold-init error) does not
	 * leave them hung in read/write with no URBs in flight.
	 */
	babyface_pcm_stop_both(chip, SNDRV_PCM_STATE_XRUN);
	bf_recount_users(chip);
	mutex_unlock(&chip->mutex);
}

/* ── PCM ───────────────────────────────────────────────────── */

static const struct snd_pcm_hardware babyface_pcm_hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S24_LE,
	.rate_min = 32000,
	.rate_max = 192000,
	.channels_min = 2,
	.channels_max = 12,
	.buffer_bytes_max = 1 << 20,
	.period_bytes_max = 1 << 18,
	.periods_min = 2,
	.periods_max = 16,
};

static int babyface_pcm_open(struct snd_pcm_substream *subs)
{
	struct snd_usb_babyface *chip = snd_pcm_substream_chip(subs);
	struct snd_pcm_runtime *rt = subs->runtime;
	unsigned long flags;
	int ret;

	rt->hw = babyface_pcm_hw;
	ret = snd_pcm_hw_constraint_list(rt, 0, SNDRV_PCM_HW_PARAM_RATE,
					 &bf_rates_constraint);
	if (ret < 0)
		return ret;
	/* One URB delivers frames_per_urb frames per interrupt; a period must
	 * span at least one URB so a completion crosses at most one period
	 * boundary.  Constrain in frames (not bytes) so the minimum period
	 * does not balloon at low channel counts: 2 ch @ 48 kHz → 256
	 * frames (5.3 ms) instead of 1536 frames from a 12-ch byte clamp.
	 */
	ret = snd_pcm_hw_constraint_minmax(rt, SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
					   chip->frames_per_urb, 1 << 18);
	if (ret < 0)
		return ret;

	spin_lock_irqsave(&chip->lock, flags);
	chip->subs[subs->stream] = subs;
	spin_unlock_irqrestore(&chip->lock, flags);
	return 0;
}

static int babyface_pcm_close(struct snd_pcm_substream *subs)
{
	struct snd_usb_babyface *chip = snd_pcm_substream_chip(subs);
	unsigned long flags;

	/* Wait for the stream stop work so the URB callbacks (which
	 * touch subs) are done before the substream can be freed.
	 */
	flush_work(&chip->stream_work);
	spin_lock_irqsave(&chip->lock, flags);
	chip->subs[subs->stream] = NULL;
	spin_unlock_irqrestore(&chip->lock, flags);
	return 0;
}

static int babyface_pcm_hw_params(struct snd_pcm_substream *subs,
				  struct snd_pcm_hw_params *params)
{
	struct snd_usb_babyface *chip = snd_pcm_substream_chip(subs);
	const struct bf_rate *r;
	int ret = 0;

	r = bf_rate_lookup(params_rate(params));
	if (!r)
		return -EINVAL;

	/* The stream URBs must be at least one alt packet wide: the device
	 * delivers its IN data in alt-sized packets (448/640/1024 B for
	 * alt 1/2/3), and a smaller URB buffer makes the host controller
	 * discard the transfer with -EOVERFLOW (babble) — seen at
	 * 176.4/192 kHz with frames_per_urb below 32.  Return a clean
	 * error instead of a silently dead capture stream.
	 */
	if (chip->frames_per_urb < r->min_fpu) {
		dev_err(&chip->dev->dev,
			"rate %u Hz needs frames_per_urb >= %u (module has %u)\n",
			r->rate, r->min_fpu, chip->frames_per_urb);
		return -EINVAL;
	}

	mutex_lock(&chip->mutex);
	if (r->rate != chip->rate) {
		/* Both directions share one clock, so a rate change must not
		 * race live transfers.  Stop the URBs, re-point the bandwidth
		 * class and let the stream work restart the session at the
		 * new rate — the other running substream briefly sees a rate
		 * step (PipeWire re-negotiates via its resampler) instead of
		 * this open failing with -EBUSY (which killed the PW sink).
		 */
		if (chip->streaming) {
			unsigned long flags;

			babyface_stream_kill(chip);
			spin_lock_irqsave(&chip->lock, flags);
			if (chip->stream_users > 0)
				schedule_work(&chip->stream_work);
			spin_unlock_irqrestore(&chip->lock, flags);
		}
		ret = usb_set_interface(chip->dev, BF_IFACE, r->alt);
		if (ret < 0)
			goto out;
		chip->rate = r->rate;
		chip->alt = r->alt;
		chip->frame_bytes = r->frame_bytes;
		dev_dbg(&chip->dev->dev, "rate %u Hz (alt %u)\n",
			chip->rate, chip->alt);
	}
out:
	mutex_unlock(&chip->mutex);
	return ret;
}

static int babyface_pcm_hw_free(struct snd_pcm_substream *subs)
{
	/* The device buffer is host-side; nothing to release here. */
	return 0;
}

static int babyface_pcm_prepare(struct snd_pcm_substream *subs)
{
	struct snd_usb_babyface *chip = snd_pcm_substream_chip(subs);
	unsigned long flags;

	spin_lock_irqsave(&chip->lock, flags);
	chip->hw_ptr[subs->stream] = 0;
	chip->prev_period[subs->stream] = 0;
	spin_unlock_irqrestore(&chip->lock, flags);
	return 0;
}

static int babyface_pcm_trigger(struct snd_pcm_substream *subs, int cmd)
{
	struct snd_usb_babyface *chip = snd_pcm_substream_chip(subs);
	unsigned long flags;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		spin_lock_irqsave(&chip->lock, flags);
		chip->hw_ptr[subs->stream] = 0;
		chip->prev_period[subs->stream] = 0;
		/* stream_users is shared by the two substreams (separate
		 * locks) — serialize the ++/-- so a concurrent trigger on
		 * the other direction can't lose an increment (which would
		 * stop the stream while a substream still runs).
		 */
		if (chip->stream_users++ == 0)
			schedule_work(&chip->stream_work);
		spin_unlock_irqrestore(&chip->lock, flags);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
		spin_lock_irqsave(&chip->lock, flags);
		if (chip->stream_users > 0 && --chip->stream_users == 0)
			schedule_work(&chip->stream_work);
		spin_unlock_irqrestore(&chip->lock, flags);
		return 0;
	}
	return -EINVAL;
}

static snd_pcm_uframes_t babyface_pcm_pointer(struct snd_pcm_substream *subs)
{
	struct snd_usb_babyface *chip = snd_pcm_substream_chip(subs);
	unsigned long flags;
	snd_pcm_uframes_t pos;

	spin_lock_irqsave(&chip->lock, flags);
	pos = chip->hw_ptr[subs->stream] % subs->runtime->buffer_size;
	spin_unlock_irqrestore(&chip->lock, flags);
	return pos;
}

static const struct snd_pcm_ops babyface_pcm_ops = {
	.open = babyface_pcm_open,
	.close = babyface_pcm_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = babyface_pcm_hw_params,
	.hw_free = babyface_pcm_hw_free,
	.prepare = babyface_pcm_prepare,
	.trigger = babyface_pcm_trigger,
	.pointer = babyface_pcm_pointer,
};
