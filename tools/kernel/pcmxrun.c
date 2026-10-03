// pcmxrun.c — regression probe: one PCM config, full-duplex, xrun-counted.
//
// Opens playback + capture on hw:<card>,0 at the given rate/period/buffer,
// plays a continuous 440 Hz sine at -18 dBFS (phase-continuous), captures
// concurrently, and counts xruns on both sides (recovering after each one,
// like a real app would).  Playback and capture run on SEPARATE THREADS
// (the real-app pattern — PipeWire/DAWs refill asynchronously); a
// serialized read→write loop would underrun at high rates even when the
// driver is fine.  If the capture stream can carry 12 ch and the rate is
// an alt-1 rate (<= 88.2 kHz), the device words 12/13 (= capture ch10/11)
// echo the playback at a fixed ~-27 dB — that RMS is a signal-integrity
// check that needs no microphone.
//
// With PCMX_MMAP=1 in the environment both streams are opened for mmap
// access and filled through snd_pcm_mmap_begin()/commit(), the way JACK
// does it; the default is read/write access.  PCMX_PBCH=n sets the playback
// channel count (default 2; 12 is what JACK opens), the tone stays on ch 0/1.
//
// Usage: [PCMX_MMAP=1] pcmxrun <card> <rate> <period> <buffer> <dur_s>
// Prints: rate=<n> period=<n> buffer=<n> pb_xruns=<n> cap_xruns=<n>
//         tap_rms=<dBFS|-inf> PASS|FAIL   (exit 0 = PASS)
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static snd_pcm_t *pb, *cap;
static int rate, pb_ch, cap_ch, tap_ok, use_mmap;
static snd_pcm_uframes_t period;
static long total;			/* frames per direction for the run */
static volatile long written = 0, read_total = 0;
static volatile int pb_xruns = 0, cap_xruns = 0, err_count = 0;
static double tap_sum = 0.0;		/* reader-owned */
static long tap_n = 0;

/* Fill buf (interleaved, ch channels, frames frames) with a 440 Hz sine
 * at -18 dBFS on ch 0/1, silence elsewhere.  Phase-continuous across
 * chunks; the phase step tracks the rate so the tone stays 440 Hz. */
static void gen(snd_pcm_uframes_t frames, int ch, int32_t *buf)
{
	static double phase = 0.0;
	/* S32_LE with 24 msbits: the valid bits are LEFT-justified, so
	 * full scale is 2^31, not 2^23 as it was under S24_LE. */
	const double amp = 2147483648.0 * 0.125;	/* -18 dBFS */
	const double step = 2.0 * M_PI * 440.0 / (double)rate;
	for (snd_pcm_uframes_t i = 0; i < frames; i++) {
		int32_t v = (int32_t)(amp * sin(phase));
		phase += step;
		for (int c = 0; c < ch; c++)
			buf[i * ch + c] = (c < 2) ? v : 0;
	}
}

/* One chunk through snd_pcm_mmap_begin()/commit(): returns the frames moved,
 * or a negative error as readi/writei would (-EPIPE on an xrun).
 */
static snd_pcm_sframes_t xfer_mmap(snd_pcm_t *p, int32_t *buf, snd_pcm_uframes_t n,
				   int ch, int is_write)
{
	snd_pcm_uframes_t done = 0;
	int idle = 0;

	while (done < n) {
		const snd_pcm_channel_area_t *areas;
		snd_pcm_uframes_t off, frames = n - done;
		snd_pcm_sframes_t avail = snd_pcm_avail_update(p), c;
		char *base;
		int e;

		if (avail < 0)
			return avail;
		if (avail == 0) {
			e = snd_pcm_wait(p, 1000);
			if (e < 0)
				return e;
			if (e == 0 && ++idle > 5)
				return -ETIMEDOUT;
			continue;
		}
		if (frames > (snd_pcm_uframes_t)avail)
			frames = avail;
		e = snd_pcm_mmap_begin(p, &areas, &off, &frames);
		if (e < 0)
			return e;
		base = (char *)areas[0].addr + areas[0].first / 8 +
		       off * (areas[0].step / 8);
		if (is_write)
			memcpy(base, buf + done * ch, frames * ch * sizeof(int32_t));
		else
			memcpy(buf + done * ch, base, frames * ch * sizeof(int32_t));
		c = snd_pcm_mmap_commit(p, off, frames);
		if (c < 0)
			return c;
		done += c;
		idle = 0;
	}
	return done;
}

static snd_pcm_sframes_t xwrite(snd_pcm_t *p, int32_t *buf, snd_pcm_uframes_t n, int ch)
{
	return use_mmap ? xfer_mmap(p, buf, n, ch, 1) : snd_pcm_writei(p, buf, n);
}

static snd_pcm_sframes_t xread(snd_pcm_t *p, int32_t *buf, snd_pcm_uframes_t n, int ch)
{
	return use_mmap ? xfer_mmap(p, buf, n, ch, 0) : snd_pcm_readi(p, buf, n);
}

static void *writer_thread(void *arg)
{
	int32_t *chunk = calloc(period * pb_ch, sizeof(int32_t));
	if (!chunk)
		return NULL;
	while (written < total) {
		gen(period, pb_ch, chunk);
		snd_pcm_sframes_t w = xwrite(pb, chunk, period, pb_ch);
		if (w < 0) {
			if (w == -EPIPE) {
				pb_xruns++;
				snd_pcm_prepare(pb);
				snd_pcm_start(pb);
				continue;
			}
			fprintf(stderr, "write: %s\n", snd_strerror(w));
			err_count++;
			break;
		}
		written += w;
	}
	free(chunk);
	return NULL;
}

static void *reader_thread(void *arg)
{
	int32_t *cbuf = calloc(period * cap_ch, sizeof(int32_t));
	if (!cbuf)
		return NULL;
	while (read_total < total) {
		snd_pcm_sframes_t r = xread(cap, cbuf, period, cap_ch);
		if (r < 0) {
			if (r == -EPIPE) {
				cap_xruns++;
				snd_pcm_prepare(cap);
				snd_pcm_start(cap);
				continue;
			}
			fprintf(stderr, "read: %s\n", snd_strerror(r));
			err_count++;
			break;
		}
		if (tap_ok) {
			for (snd_pcm_sframes_t i = 0; i < r; i++) {
				int32_t v = cbuf[i * cap_ch + 10];	/* word 12 */
				tap_sum += (double)v * (double)v;
			}
			tap_n += r;
		}
		read_total += r;
	}
	free(cbuf);
	return NULL;
}

int main(int argc, char **argv)
{
	if (argc != 6) {
		fprintf(stderr, "usage: [PCMX_MMAP=1] %s card rate period buffer dur_s\n", argv[0]);
		return 2;
	}
	int card = atoi(argv[1]);
	rate = atoi(argv[2]);
	period = atoi(argv[3]);
	snd_pcm_uframes_t buffer = atoi(argv[4]);
	int dur = atoi(argv[5]);
	pb_ch = getenv("PCMX_PBCH") ? atoi(getenv("PCMX_PBCH")) : 2;
	use_mmap = getenv("PCMX_MMAP") && atoi(getenv("PCMX_MMAP"));
	/* alt-1 rates have the 14-word frame (56 B): words 12/13 = the
	 * playback tap at ch10/11.  alt 2/3 frames are 10/8 words, with no
	 * tap channels, so only the alt-1 rates (<= 48 kHz) carry it. */
	tap_ok = (rate <= 48000);
	cap_ch = tap_ok ? 12 : 2;
	total = (long)rate * dur;
	char dev[32];
	snd_pcm_hw_params_t *hp;
	int err;
	pthread_t wt, rt;

	snprintf(dev, sizeof(dev), "hw:%d,0", card);
	snd_pcm_hw_params_alloca(&hp);

	if ((err = snd_pcm_open(&pb, dev, SND_PCM_STREAM_PLAYBACK, 0)) < 0)
		goto fail;
	if ((err = snd_pcm_open(&cap, dev, SND_PCM_STREAM_CAPTURE, 0)) < 0)
		goto fail;

	snd_pcm_hw_params_any(pb, hp);
	if ((err = snd_pcm_hw_params_set_access(pb, hp, use_mmap ?
			SND_PCM_ACCESS_MMAP_INTERLEAVED :
			SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
		fprintf(stderr, "pcmxrun: %s access not available on playback\n",
			use_mmap ? "mmap" : "read/write");
		goto fail;
	}
	snd_pcm_hw_params_set_format(pb, hp, SND_PCM_FORMAT_S32_LE);
	snd_pcm_hw_params_set_channels(pb, hp, pb_ch);
	snd_pcm_hw_params_set_rate(pb, hp, rate, 0);
	snd_pcm_hw_params_set_period_size(pb, hp, period, 0);
	snd_pcm_hw_params_set_buffer_size(pb, hp, buffer);
	if ((err = snd_pcm_hw_params(pb, hp)) < 0)
		goto fail;

	snd_pcm_hw_params_any(cap, hp);
	if ((err = snd_pcm_hw_params_set_access(cap, hp, use_mmap ?
			SND_PCM_ACCESS_MMAP_INTERLEAVED :
			SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
		fprintf(stderr, "pcmxrun: %s access not available on capture\n",
			use_mmap ? "mmap" : "read/write");
		goto fail;
	}
	snd_pcm_hw_params_set_format(cap, hp, SND_PCM_FORMAT_S32_LE);
	snd_pcm_hw_params_set_channels(cap, hp, cap_ch);
	snd_pcm_hw_params_set_rate(cap, hp, rate, 0);
	snd_pcm_hw_params_set_period_size(cap, hp, period, 0);
	snd_pcm_hw_params_set_buffer_size(cap, hp, buffer);
	if ((err = snd_pcm_hw_params(cap, hp)) < 0)
		goto fail;

	if ((err = snd_pcm_prepare(pb)) < 0)
		goto fail;
	if ((err = snd_pcm_prepare(cap)) < 0)
		goto fail;

	/* Prefill the FULL buffer (phase-continuous sine) so the device
	 * never starves before the writer thread's first write. */
	int32_t *chunk = calloc(period * pb_ch, sizeof(int32_t));
	if (!chunk) {
		err = -ENOMEM;
		goto fail;
	}
	{
		long queued = 0;
		while (queued < (long)buffer) {
			snd_pcm_uframes_t n = period;
			if (queued + (long)n > (long)buffer)
				n = (snd_pcm_uframes_t)(buffer - queued);
			gen(n, pb_ch, chunk);
			snd_pcm_sframes_t w = xwrite(pb, chunk, n, pb_ch);
			if (w < 0) {
				err = (int)w;
				free(chunk);
				goto fail;
			}
			queued += w;
		}
	}
	free(chunk);

	/* Either stream may already be RUNNING (auto-start on the prefill
	 * reaching start_threshold) — EBADFD = already running, fine. */
	err = snd_pcm_start(cap);
	if (err == -EBADFD)
		err = 0;
	if (err < 0)
		goto fail;
	err = snd_pcm_start(pb);
	if (err == -EBADFD)
		err = 0;
	if (err < 0)
		goto fail;

	pthread_create(&wt, NULL, writer_thread, NULL);
	pthread_create(&rt, NULL, reader_thread, NULL);
	pthread_join(wt, NULL);
	pthread_join(rt, NULL);

	double tap_db = -INFINITY;
	if (tap_n > 0)
		tap_db = 20.0 * log10(sqrt(tap_sum / tap_n) / 2147483648.0);

	int pass = (err_count == 0 && pb_xruns == 0 && cap_xruns == 0) &&
		   (!tap_ok || tap_db > -55.0);
	printf("rate=%-6d period=%-5ld buffer=%-5ld pb_xruns=%d cap_xruns=%d "
	       "err=%d tap_rms=%.1f %s\n", rate, (long)period, (long)buffer,
	       pb_xruns, cap_xruns, err_count, tap_db, pass ? "PASS" : "FAIL");

	snd_pcm_drain(pb);
	snd_pcm_close(cap);
	snd_pcm_close(pb);
	return pass ? 0 : 1;

fail:
	fprintf(stderr, "pcmxrun: %s\n", snd_strerror(err));
	if (cap) snd_pcm_close(cap);
	if (pb) snd_pcm_close(pb);
	return 1;
}
