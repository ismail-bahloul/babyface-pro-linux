// pcmreneg.c — regression probe: period/buffer changes inside one PCM
// session (the issue-#5 shape), full-duplex, with the signal tap.
//
// Opens playback + capture ONCE on hw:<card>,0, then walks a list of
// period:buffer configs doing the exact client sequence a sound server
// uses when the user moves the buffer-size slider:
//
//     snd_pcm_hw_params() -> prepare -> prefill -> start -> run
//         -> drain -> snd_pcm_hw_free()  ->  next config, SAME handles
//
// issue #5 was this renegotiation leaving the stream "started" but
// silent (recovery needed a module reload or a replug).  Audio flow is
// checked the same way pcmxrun does: at an alt-1 rate (<= 48 kHz) the
// AS1/2 output loopback on capture ch4/5 (device words 4/5) echoes the
// playback, so a config that goes silent after a change reads
// tap_rms=-inf and FAILs even though no xrun is reported.  regress.sh
// enables the AS1/2 loopback for the run.
//
// Usage: pcmreneg <card> <rate> [period:buffer ...]
//   default configs: 256:512 512:1024 1024:2048 2048:4096 512:1024 256:512
// Prints one line per config, then "== renegotiation: N/M configs PASS =="
// (exit 0 = every config PASS).
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static snd_pcm_t *pb, *cap;
static int rate, pb_ch, cap_ch, tap_ok;
static snd_pcm_uframes_t period;
static long total;			/* frames per direction for a config */
static volatile long written, read_total;
static volatile int pb_xruns, cap_xruns, err_count;
static double tap_sum;			/* reader-owned */
static long tap_n;

/* Fill buf with a phase-continuous 440 Hz sine at -18 dBFS on ch 0/1.
 * S32_LE with 24 msbits is LEFT-justified, so full scale is 2^31. */
static void gen(snd_pcm_uframes_t frames, int ch, int32_t *buf)
{
	static double phase = 0.0;
	const double amp = 2147483648.0 * 0.125;
	const double step = 2.0 * M_PI * 440.0 / (double)rate;

	for (snd_pcm_uframes_t i = 0; i < frames; i++) {
		int32_t v = (int32_t)(amp * sin(phase));

		phase += step;
		for (int c = 0; c < ch; c++)
			buf[i * ch + c] = (c < 2) ? v : 0;
	}
}

static void *writer_thread(void *arg)
{
	int32_t *chunk = calloc(period * pb_ch, sizeof(int32_t));

	(void)arg;

	while (chunk && written < total) {
		snd_pcm_sframes_t w;

		gen(period, pb_ch, chunk);
		w = snd_pcm_writei(pb, chunk, period);
		if (w < 0) {
			if (w == -EPIPE) {
				pb_xruns++;
				snd_pcm_prepare(pb);
				snd_pcm_start(pb);
				continue;
			}
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

	(void)arg;

	while (cbuf && read_total < total) {
		snd_pcm_sframes_t r = snd_pcm_readi(cap, cbuf, period);

		if (r < 0) {
			if (r == -EPIPE) {
				cap_xruns++;
				snd_pcm_prepare(cap);
				snd_pcm_start(cap);
				continue;
			}
			err_count++;
			break;
		}
		if (tap_ok) {
			for (snd_pcm_sframes_t i = 0; i < r; i++) {
				int32_t v = cbuf[i * cap_ch + 4];	/* word 4 */

				tap_sum += (double)v * (double)v;
			}
			tap_n += r;
		}
		read_total += r;
	}
	free(cbuf);
	return NULL;
}

static int run_config(snd_pcm_uframes_t buffer)
{
	snd_pcm_hw_params_t *hp;
	int err;
	pthread_t wt, rt;

	snd_pcm_hw_params_alloca(&hp);

	snd_pcm_hw_params_any(pb, hp);
	snd_pcm_hw_params_set_access(pb, hp, SND_PCM_ACCESS_RW_INTERLEAVED);
	snd_pcm_hw_params_set_format(pb, hp, SND_PCM_FORMAT_S32_LE);
	snd_pcm_hw_params_set_channels(pb, hp, pb_ch);
	snd_pcm_hw_params_set_rate(pb, hp, rate, 0);
	snd_pcm_hw_params_set_period_size(pb, hp, period, 0);
	snd_pcm_hw_params_set_buffer_size(pb, hp, buffer);
	if ((err = snd_pcm_hw_params(pb, hp)) < 0) {
		printf("period=%-5ld buffer=%-5ld hw_params pb: %s  FAIL\n",
		       (long)period, (long)buffer, snd_strerror(err));
		return 1;
	}

	snd_pcm_hw_params_any(cap, hp);
	snd_pcm_hw_params_set_access(cap, hp, SND_PCM_ACCESS_RW_INTERLEAVED);
	snd_pcm_hw_params_set_format(cap, hp, SND_PCM_FORMAT_S32_LE);
	snd_pcm_hw_params_set_channels(cap, hp, cap_ch);
	snd_pcm_hw_params_set_rate(cap, hp, rate, 0);
	snd_pcm_hw_params_set_period_size(cap, hp, period, 0);
	snd_pcm_hw_params_set_buffer_size(cap, hp, buffer);
	if ((err = snd_pcm_hw_params(cap, hp)) < 0) {
		printf("period=%-5ld buffer=%-5ld hw_params cap: %s  FAIL\n",
		       (long)period, (long)buffer, snd_strerror(err));
		snd_pcm_hw_free(pb);
		return 1;
	}

	snd_pcm_prepare(pb);
	snd_pcm_prepare(cap);

	/* Prefill the whole buffer so the device never starves before the
	 * writer thread's first write (phase-continuous). */
	int32_t *chunk = calloc(period * pb_ch, sizeof(int32_t));
	long queued = 0;

	while (chunk && queued < (long)buffer) {
		snd_pcm_uframes_t n = period;
		snd_pcm_sframes_t w;

		if (queued + (long)n > (long)buffer)
			n = buffer - queued;
		gen(n, pb_ch, chunk);
		w = snd_pcm_writei(pb, chunk, n);
		if (w < 0)
			break;
		queued += w;
	}
	free(chunk);

	/* Either stream may already be RUNNING (auto-start once the prefill
	 * reaches start_threshold) — -EBADFD is fine, ignore it. */
	snd_pcm_start(cap);
	snd_pcm_start(pb);

	pthread_create(&wt, NULL, writer_thread, NULL);
	pthread_create(&rt, NULL, reader_thread, NULL);
	pthread_join(wt, NULL);
	pthread_join(rt, NULL);

	double tap_db = -INFINITY;

	if (tap_n > 0)
		tap_db = 20.0 * log10(sqrt(tap_sum / tap_n) / 2147483648.0);

	int pass = (err_count == 0 && pb_xruns == 0 && cap_xruns == 0) &&
		   (!tap_ok || tap_db > -55.0);

	printf("period=%-5ld buffer=%-5ld pb_xruns=%d cap_xruns=%d err=%d "
	       "tap_rms=%.1f %s\n", (long)period, (long)buffer, pb_xruns,
	       cap_xruns, err_count, tap_db, pass ? "PASS" : "FAIL");

	snd_pcm_drain(pb);
	snd_pcm_drop(cap);
	snd_pcm_hw_free(pb);
	snd_pcm_hw_free(cap);
	return pass ? 0 : 1;
}

int main(int argc, char **argv)
{
	int card, ncfg = 0, fails = 0, err;
	struct cfg { snd_pcm_uframes_t p, b; } cfgs[64];
	char dev[32];

	if (argc < 3) {
		fprintf(stderr, "usage: %s card rate [period:buffer ...]\n",
			argv[0]);
		return 2;
	}
	card = atoi(argv[1]);
	rate = atoi(argv[2]);
	pb_ch = 2;
	/* alt-1 rates carry the 14-word frame (words 4/5 = AS1/2, the
	 * loopback tap at ch4/5); the check stays on the alt-1 rates, as
	 * in pcmxrun. */
	tap_ok = (rate <= 48000);
	cap_ch = tap_ok ? 12 : 2;

	if (argc > 3) {
		for (int i = 3; i < argc && ncfg < 64; i++) {
			unsigned long p, b;

			if (sscanf(argv[i], "%lu:%lu", &p, &b) == 2) {
				cfgs[ncfg].p = p;
				cfgs[ncfg].b = b;
				ncfg++;
			}
		}
	} else {
		static const snd_pcm_uframes_t d[][2] = {
			{ 256, 512 }, { 512, 1024 }, { 1024, 2048 },
			{ 2048, 4096 }, { 512, 1024 }, { 256, 512 },
		};

		for (unsigned i = 0; i < sizeof(d) / sizeof(d[0]); i++) {
			cfgs[ncfg].p = d[i][0];
			cfgs[ncfg].b = d[i][1];
			ncfg++;
		}
	}
	if (ncfg == 0) {
		fprintf(stderr, "pcmreneg: no config parsed\n");
		return 2;
	}

	snprintf(dev, sizeof(dev), "hw:%d,0", card);
	if ((err = snd_pcm_open(&pb, dev, SND_PCM_STREAM_PLAYBACK, 0)) < 0)
		goto fail;
	if ((err = snd_pcm_open(&cap, dev, SND_PCM_STREAM_CAPTURE, 0)) < 0)
		goto fail;

	for (int c = 0; c < ncfg; c++) {
		period = cfgs[c].p;
		written = read_total = 0;
		pb_xruns = cap_xruns = err_count = 0;
		tap_sum = 0.0;
		tap_n = 0;
		total = (long)rate;	/* ~1 s per config */
		fails += run_config(cfgs[c].b);
	}

	printf("== renegotiation: %d/%d configs PASS ==\n", ncfg - fails, ncfg);
	snd_pcm_close(cap);
	snd_pcm_close(pb);
	return fails ? 1 : 0;

fail:
	fprintf(stderr, "pcmreneg: %s\n", snd_strerror(err));
	if (cap)
		snd_pcm_close(cap);
	if (pb)
		snd_pcm_close(pb);
	return 1;
}
