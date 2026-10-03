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
// Usage: looplat [options] <card> <pb_ch> <period> <buffer> <dur_s>
//        (capture is always 12 ch — the loopback lands on ch10/11)
// Prints: latency_frames=<n> latency_ms=<x> xruns=<n>
//
// Options (see usage() below) set the rate, the capture channel the
// impulse is detected on (ch2 = IN3 with PH3/4 cabled into it), a capture
// period and buffer of their own, and which direction is set up first.
// With a capture period that differs from the playback one, playback runs
// in its own thread and the xruns are counted per direction.
#include <alsa/asoundlib.h>
#include <getopt.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [options] card pb_ch period buffer dur_s\n"
		"\n"
		"Round-trip latency from an impulse at playback frame 0, found on\n"
		"a capture channel.  Playback and capture start linked, on the\n"
		"same device frame.\n"
		"\n"
		"  card      ALSA card number or id (hw:<card>,0)\n"
		"  pb_ch     playback channels; the impulse goes to ch0/1\n"
		"  period    period in frames (playback, and capture unless -c)\n"
		"  buffer    buffer in frames (the same)\n"
		"  dur_s     seconds to run\n"
		"\n"
		"options:\n"
		"  -r RATE      sample rate in Hz (default 48000)\n"
		"  -d CH        capture channel to detect on (default 10, the\n"
		"               internal playback tap; 2 = IN3 via a cable)\n"
		"  -c P/B       capture period/buffer (default: as playback)\n"
		"  -f pb|cap    set up this direction first (default pb)\n"
		"  -D, --delay  check snd_pcm_delay() instead: see below\n"
		"  -h           this help\n"
		"\n"
		"prints: latency_frames=N latency_ms=X xruns=N; with a capture\n"
		"        period other than the playback one, pb_xruns=N\n"
		"        cap_xruns=N instead of xruns=N\n"
		"\n"
		"--delay: stream silence, and every half second write an impulse.\n"
		"Just before, read snd_pcm_delay() on both streams: the impulse\n"
		"should appear at capture frame (frames read + capture delay +\n"
		"playback delay).  extra = where it appears minus that, i.e. the\n"
		"latency the delays do not report (converters, USB).  Needs the\n"
		"same period in both directions.\n"
		"prints: impulses=N extra_min=N extra_max=N extra_avg=X\n"
		"        reported_avg=X measured_avg=X xruns=N\n",
		prog);
}

/* With -c, playback is fed on its own period, independent of the
 * capture loop. */
static snd_pcm_t *w_pb;
static int32_t *w_z;
static snd_pcm_uframes_t w_period;
static volatile int w_stop, w_xruns;

static void *writer(void *arg)
{
	(void)arg;
	while (!w_stop) {
		snd_pcm_sframes_t w = snd_pcm_writei(w_pb, w_z, w_period);

		if (w == -EPIPE) {
			w_xruns++;
			snd_pcm_prepare(w_pb);
			snd_pcm_start(w_pb);
		} else if (w < 0 && w != -EAGAIN) {
			break;
		}
	}
	return NULL;
}

/* Set up one direction. */
static int setup(snd_pcm_t *p, snd_pcm_hw_params_t *hp, int ch, int rate,
		 snd_pcm_uframes_t period, snd_pcm_uframes_t buffer)
{
	int err;

	snd_pcm_hw_params_any(p, hp);
	snd_pcm_hw_params_set_access(p, hp, SND_PCM_ACCESS_RW_INTERLEAVED);
	snd_pcm_hw_params_set_format(p, hp, SND_PCM_FORMAT_S32_LE);
	snd_pcm_hw_params_set_channels(p, hp, ch);
	snd_pcm_hw_params_set_rate(p, hp, rate, 0);
	err = snd_pcm_hw_params_set_period_size(p, hp, period, 0);
	if (err < 0) {
		fprintf(stderr, "%s period %lu refused\n",
			snd_pcm_stream(p) == SND_PCM_STREAM_CAPTURE ?
			"capture" : "playback", (unsigned long)period);
		return err;
	}
	snd_pcm_hw_params_set_buffer_size(p, hp, buffer);
	return snd_pcm_hw_params(p, hp);
}

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

/* --delay: the check a DAW's latency compensation relies on.  The
 * streams run in lockstep, one capture period read, one playback period
 * written.  Before the write that starts an impulse, the playback delay
 * says in how many frames that impulse is heard, and the capture delay
 * how far the frame being captured now is ahead of what has been read. */
#define IMPULSE_W	60
#define IMPULSE_AMP	0x40000000

static int run_delay(snd_pcm_t *pb, snd_pcm_t *cap, int pb_ch, int cap_ch,
		     int det_ch, int rate, snd_pcm_uframes_t period, int dur)
{
	int32_t *wbuf = calloc(period * pb_ch, sizeof(int32_t));
	int32_t *cbuf = calloc(period * cap_ch, sizeof(int32_t));
	long every = ((long)rate / 2 / period) * period;	/* ~0.5 s */
	long npred = (long)dur * 2 + 2, n = 0, hits = 0;
	long *pred = calloc(npred, sizeof(long));
	long *rep = calloc(npred, sizeof(long));
	long *wpos = calloc(npred, sizeof(long));
	long frame = 0, pulse = -1, last_edge = -rate;
	long emin = 0, emax = 0;
	double esum = 0, rsum = 0, msum = 0;
	int xruns = 0, prev_hi = 0;

	if (every < (long)period)
		every = period;
	while (frame < (long)rate * dur) {
		snd_pcm_sframes_t r = snd_pcm_readi(cap, cbuf, period);

		if (r < 0) {
			xruns++;
			break;		/* the lockstep is gone: stop */
		}
		for (long i = 0; i < r; i++) {
			int32_t v = cbuf[i * cap_ch + det_ch];
			int hi = abs(v) > IMPULSE_AMP / 64;
			long at = frame + i;

			/* The first rising edge after each prediction. */
			if (hi && !prev_hi && at - last_edge > rate / 10) {
				last_edge = at;
				while (hits < n && pred[hits] + rate / 4 < at)
					hits++;	/* impulse lost: skip it */
				if (hits < n && at > pred[hits] - rate / 10) {
					long e = at - pred[hits];

					if (!hits || e < emin)
						emin = e;
					if (!hits || e > emax)
						emax = e;
					esum += e;
					rsum += rep[hits];
					msum += at - wpos[hits];
					hits++;
				}
			}
			prev_hi = hi;
		}
		frame += r;

		/* Start an impulse every ~0.5 s, after the first second. */
		if (pulse < 0 && frame >= rate && frame % every == 0 && n < npred) {
			snd_pcm_sframes_t dp, dc;

			if (snd_pcm_delay(pb, &dp) < 0 || snd_pcm_delay(cap, &dc) < 0) {
				xruns++;
				break;
			}
			pred[n] = frame + dc + dp;
			rep[n] = dc + dp;
			wpos[n] = frame;
			n++;
			pulse = 0;
		}
		for (snd_pcm_uframes_t f = 0; f < period; f++) {
			int32_t a = pulse >= 0 && pulse < IMPULSE_W ? IMPULSE_AMP : 0;

			for (int c = 0; c < pb_ch; c++)
				wbuf[f * pb_ch + c] = c < 2 ? a : 0;
			if (pulse >= 0 && ++pulse >= IMPULSE_W)
				pulse = -1;
		}
		snd_pcm_sframes_t w = snd_pcm_writei(pb, wbuf, period);
		if (w < 0) {
			xruns++;
			break;
		}
	}
	if (!hits)
		fprintf(stderr, "no impulse detected\n");
	else
		printf("impulses=%ld extra_min=%ld extra_max=%ld extra_avg=%.1f "
		       "reported_avg=%.1f measured_avg=%.1f xruns=%d\n",
		       hits, emin, emax, esum / hits, rsum / hits, msum / hits,
		       xruns);
	free(wbuf);
	free(cbuf);
	free(pred);
	free(rep);
	free(wpos);
	return hits ? 0 : 1;
}

int main(int argc, char **argv)
{
	int rate = 48000;
	int det_ch = 10;		/* loopback = words 12/13 = ch10/11 */
	int cap_first = 0, split = 0, delay = 0;
	static const struct option lopts[] = {
		{ "delay", no_argument, NULL, 'D' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	snd_pcm_uframes_t cperiod = 0, cbuffer = 0;
	int opt;

	while ((opt = getopt_long(argc, argv, "r:d:c:f:Dh", lopts, NULL)) != -1) {
		switch (opt) {
		case 'r':
			rate = atoi(optarg);
			break;
		case 'd':
			det_ch = atoi(optarg);
			break;
		case 'c':
			if (sscanf(optarg, "%lu/%lu", &cperiod, &cbuffer) != 2) {
				usage(argv[0]);
				return 2;
			}
			break;
		case 'f':
			cap_first = optarg[0] == 'c';
			break;
		case 'D':
			delay = 1;
			break;
		default:
			usage(argv[0]);
			return opt == 'h' ? 0 : 2;
		}
	}
	if (argc - optind != 5 || det_ch < 0 || det_ch > 11) {
		usage(argv[0]);
		return 2;
	}
	const char *card = argv[optind];
	int pb_ch = atoi(argv[optind + 1]);
	snd_pcm_uframes_t period = atoi(argv[optind + 2]);
	snd_pcm_uframes_t buffer = atoi(argv[optind + 3]);
	int dur = atoi(argv[optind + 4]);
	int cap_ch = 12;
	char dev[32];
	snd_pcm_t *pb = NULL, *cap = NULL;
	snd_pcm_hw_params_t *hp;
	snd_pcm_sw_params_t *sw;
	int err;

	if (!cperiod) {
		cperiod = period;
		cbuffer = buffer;
	}
	split = cperiod != period;
	if (delay && split) {
		fprintf(stderr, "--delay needs the same period in both directions\n");
		return 2;
	}
	snprintf(dev, sizeof(dev), "hw:%s,0", card);
	snd_pcm_hw_params_alloca(&hp);
	snd_pcm_sw_params_alloca(&sw);

	if ((err = snd_pcm_open(&pb, dev, SND_PCM_STREAM_PLAYBACK, 0)) < 0)
		goto fail;
	if ((err = snd_pcm_open(&cap, dev, SND_PCM_STREAM_CAPTURE, 0)) < 0)
		goto fail;

	/* The driver takes the URB size from the first direction set up,
	 * so the order matters when the periods differ. */
	for (int step = 0; step < 2; step++) {
		if ((step == 0) == cap_first)
			err = setup(cap, hp, cap_ch, rate, cperiod, cbuffer);
		else
			err = setup(pb, hp, pb_ch, rate, period, buffer);
		if (err < 0)
			goto fail;
	}

	/* Playback buffer: impulse train (period 4800 frames, width 60) from
	 * frame 0 on ch 0/1, silence elsewhere. */
	int32_t *pbuf = calloc(buffer * pb_ch, sizeof(int32_t));
	int32_t *z = calloc(period * pb_ch, sizeof(int32_t));
	int32_t *cbuf = calloc(cperiod * cap_ch, sizeof(int32_t));
	for (int i = 0; i < (int)buffer && !delay; i++)
		if ((i % 4800) < 60)
			for (int c = 0; c < pb_ch && c < 2; c++)
				pbuf[i * pb_ch + c] = 0x40000000;

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

	if (delay) {
		int rc = run_delay(pb, cap, pb_ch, cap_ch, det_ch, rate, period, dur);

		snd_pcm_drop(pb);
		snd_pcm_close(cap);
		snd_pcm_close(pb);
		return rc;
	}

	/* With different periods, started together; from here each
	 * direction recovers on its own. */
	pthread_t wt;
	if (split) {
		if (linked)
			snd_pcm_unlink(cap);
		w_pb = pb;
		w_z = z;
		w_period = period;
		pthread_create(&wt, NULL, writer, NULL);
	}

	long first = -1, frame = 0;
	int xruns = 0;
	while (frame < (long)rate * dur) {
		snd_pcm_sframes_t r = snd_pcm_readi(cap, cbuf, cperiod);
		if (r < 0) {
			if (r == -EPIPE) {
				xruns++;
				snd_pcm_prepare(cap);
				if (split) {
					snd_pcm_start(cap);
					continue;
				}
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
			int32_t v = cbuf[i * cap_ch + det_ch];	/* 10: word 12 */
			/* The tap echoes the impulse ~27 dB down (0x40000000 -> ~0x01C00000),
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
			if (first < 0 && abs(v) > 0x40000000 / 64)
				first = frame + i;
		}
		frame += r;
		if (split)
			continue;
		snd_pcm_sframes_t w = snd_pcm_writei(pb, z, period);
		if (w < 0 && w == -EPIPE) {
			xruns++;
			snd_pcm_prepare(pb);
		}
	}

	if (split) {
		w_stop = 1;
		pthread_join(wt, NULL);
	}
	if (first < 0)
		fprintf(stderr, "no impulse detected\n");
	else if (split)
		printf("latency_frames=%ld latency_ms=%.2f pb_xruns=%d cap_xruns=%d\n",
		       first, (double)first * 1000.0 / rate, w_xruns, xruns);
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
