// looplat.c — loopback round-trip latency + xrun measurement, phase-anchored.
//
// Principle: open playback + capture on the same device, LINK them so the
// kernel triggers both on the same instant (shared device clock), pre-fill
// the playback buffer with an impulse train starting at frame 0, and detect
// the first rising edge in the loopback capture (device words 12/13 =
// capture ch10/11 in the 12-ch frame).  Because both streams start at
// device frame 0 together, the first-impulse position IS the loopback
// round-trip latency — no start-phase ambiguity.
//
// Usage: looplat <card> <pb_ch> <period> <buffer> <dur_s>
//        (capture is always 12 ch — the loopback lands on ch10/11)
// Prints: latency_frames=<n> latency_ms=<x> xruns=<n>
#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Keep the pre-filled impulse buffer from auto-starting playback (the
 * default start_threshold = buffer_size would).  The linked start below
 * must be the only thing that triggers the streams. */
static int no_autostart(snd_pcm_t *p)
{
	snd_pcm_sw_params_t *s;
	int r;

	snd_pcm_sw_params_alloca(&s);
	r = snd_pcm_sw_params_current(p, s);
	if (r < 0)
		return r;
	r = snd_pcm_sw_params_set_start_threshold(p, s,
		(snd_pcm_uframes_t)-1);
	if (r < 0)
		return r;
	return snd_pcm_sw_params(p, s);
}

int main(int argc, char **argv)
{
	if (argc != 6) {
		fprintf(stderr, "usage: %s card pb_ch period buffer dur_s\n", argv[0]);
		return 2;
	}
	int card = atoi(argv[1]);
	int pb_ch = atoi(argv[2]);
	snd_pcm_uframes_t period = atoi(argv[3]);
	snd_pcm_uframes_t buffer = atoi(argv[4]);
	int dur = atoi(argv[5]);
	int rate = 48000;
	int cap_ch = 12;		/* loopback = words 12/13 = ch10/11 */
	char dev[32];
	snd_pcm_t *pb = NULL, *cap = NULL;
	snd_pcm_hw_params_t *hp;
	snd_pcm_sw_params_t *sw;
	int err;

	snprintf(dev, sizeof(dev), "hw:%d,0", card);
	snd_pcm_hw_params_alloca(&hp);
	snd_pcm_sw_params_alloca(&sw);

	if ((err = snd_pcm_open(&pb, dev, SND_PCM_STREAM_PLAYBACK, 0)) < 0)
		goto fail;
	if ((err = snd_pcm_open(&cap, dev, SND_PCM_STREAM_CAPTURE, 0)) < 0)
		goto fail;

	snd_pcm_hw_params_any(pb, hp);
	snd_pcm_hw_params_set_access(pb, hp, SND_PCM_ACCESS_RW_INTERLEAVED);
	snd_pcm_hw_params_set_format(pb, hp, SND_PCM_FORMAT_S24_LE);
	snd_pcm_hw_params_set_channels(pb, hp, pb_ch);
	snd_pcm_hw_params_set_rate(pb, hp, rate, 0);
	snd_pcm_hw_params_set_period_size(pb, hp, period, 0);
	snd_pcm_hw_params_set_buffer_size(pb, hp, buffer);
	if ((err = snd_pcm_hw_params(pb, hp)) < 0)
		goto fail;

	snd_pcm_hw_params_any(cap, hp);
	snd_pcm_hw_params_set_access(cap, hp, SND_PCM_ACCESS_RW_INTERLEAVED);
	snd_pcm_hw_params_set_format(cap, hp, SND_PCM_FORMAT_S24_LE);
	snd_pcm_hw_params_set_channels(cap, hp, cap_ch);
	snd_pcm_hw_params_set_rate(cap, hp, rate, 0);
	snd_pcm_hw_params_set_period_size(cap, hp, period, 0);
	snd_pcm_hw_params_set_buffer_size(cap, hp, buffer);
	if ((err = snd_pcm_hw_params(cap, hp)) < 0)
		goto fail;

	/* Playback buffer: impulse train (period 4800 frames, width 60) from
	 * frame 0 on ch 0/1, silence elsewhere. */
	int32_t *pbuf = calloc(buffer * pb_ch, sizeof(int32_t));
	int32_t *z = calloc(period * pb_ch, sizeof(int32_t));
	int32_t *cbuf = calloc(period * cap_ch, sizeof(int32_t));
	for (int i = 0; i < (int)buffer; i++)
		if ((i % 4800) < 60)
			for (int c = 0; c < pb_ch && c < 2; c++)
				pbuf[i * pb_ch + c] = 0x400000;

	/* Linked start: both streams trigger on the same device frame.
	 * The prefill above may already have auto-started the linked pair
	 * (default start_threshold) — either way the group starts from
	 * device frame 0 with the impulse at the head of the buffer, so the
	 * anchor holds.  EBADFD = already running, which is fine. */
	snd_pcm_unlink(cap);
	int linked = (snd_pcm_link(cap, pb) == 0);

	if ((err = snd_pcm_prepare(pb)) < 0)
		goto fail;
	if ((err = snd_pcm_prepare(cap)) < 0)
		goto fail;
	/* prepare() resets sw_params to the defaults (start_threshold =
	 * buffer_size) — set them again so the prefill does not auto-start. */
	if ((err = no_autostart(pb)) < 0 || (err = no_autostart(cap)) < 0)
		goto fail;
	{
		long queued = 0;
		while (queued < (long)buffer) {
			int32_t *src = pbuf + queued * pb_ch;
			snd_pcm_sframes_t w = snd_pcm_writei(pb, src,
							   (snd_pcm_uframes_t)(buffer - queued));
			if (w < 0) {
				fprintf(stderr, "prefill writei: %s (queued=%ld)\n",
					snd_strerror(w), queued);
				err = w;
				goto fail;
			}
			queued += w;
		}
	}

	err = snd_pcm_start(cap);		/* linked: starts both */
	if (!linked && err >= 0)
		err = snd_pcm_start(pb);
	if (err == -EBADFD)
		err = 0;			/* already running */
	if (err < 0) {
		fprintf(stderr, "start: %s (cap state=%s)\n", snd_strerror(err),
			snd_pcm_state_name(snd_pcm_state(cap)));
		goto fail;
	}

	long first = -1, frame = 0;
	int xruns = 0;
	while (frame < (long)rate * dur) {
		snd_pcm_sframes_t r = snd_pcm_readi(cap, cbuf, period);
		if (r < 0) {
			if (r == -EPIPE) {
				xruns++;
				snd_pcm_prepare(cap);
				snd_pcm_prepare(pb);
				snd_pcm_start(cap);
				if (!linked)
					snd_pcm_start(pb);
				continue;
			}
			fprintf(stderr, "read: %s\n", snd_strerror(r));
			break;
		}
		for (int i = 0; i < r; i++) {
			int32_t v = cbuf[i * cap_ch + 10];	/* word 12 = ch10 */
			/* The tap echoes the impulse ~27 dB down (0x400000 -> ~0x01C000),
			 * so the old threshold 0x400000/2 never matched and the first
			 * crossing found was a spurious one.  Use 1/64 — the noise
			 * floor is far below.
			 *
			 * NOTE: the driver submits the 8 IN URBs BEFORE the session
			 * arm (validated protocol order), so the first ~2048 capture
			 * frames are pre-arm backlog (no tap) and the first impulse
			 * lands at ~2048 + 42 frames for periods >= 512.  The real
			 * loopback latency is the small-period value: 42 frames
			 * (0.88 ms).  For the large periods, subtract the pre-arm
			 * backlog (8 * frames_per_urb). */
			if (first < 0 && abs(v) > 0x400000 / 64)
				first = frame + i;
		}
		frame += r;
		snd_pcm_sframes_t w = snd_pcm_writei(pb, z, period);
		if (w < 0 && w == -EPIPE) {
			xruns++;
			snd_pcm_prepare(pb);
		}
	}

	if (first < 0)
		fprintf(stderr, "no impulse detected\n");
	else
		printf("latency_frames=%ld latency_ms=%.2f xruns=%d\n",
		       first, (double)first * 1000.0 / rate, xruns);

	snd_pcm_drain(pb);
	snd_pcm_close(cap);
	snd_pcm_close(pb);
	return 0;

fail:
	fprintf(stderr, "looplat: %s\n", snd_strerror(err));
	if (cap) snd_pcm_close(cap);
	if (pb) snd_pcm_close(pb);
	return 1;
}
