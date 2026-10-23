// pcmlate.c - does a late playback write come back as an xrun?
//
// Opens playback and capture on hw:<card>,0, plays a short click on
// ch 0/1 every quarter second and silence in between, and stalls the
// writer once, for longer than the playback buffer.  The PCM core has to
// report that stall as a playback xrun (-EPIPE): the device ran out of
// audio while the application was away.
//
// At the alt-1 rates (<= 48 kHz) the capture stream carries the AS1/2
// output loopback on ch 4 (device words 4/5), so the clicks can be
// followed through the device when that loopback is on; without it the
// slip reads n/a, which does not affect PASS.  Each click's capture frame
// minus its playback frame is
// constant while the two streams stay locked.  If the stall is not
// reported, the clicks written after it are still played, only later:
// the difference grows by the silence the device played meanwhile, and
// playback stays that much behind capture without the application
// knowing.  That shift is printed as "slip".
//
// Usage: pcmlate <card> <rate> <period> <buffer> [stall_ms]
//   stall_ms defaults to twice the buffer time plus 20 ms.
// Prints: rate=<n> period=<n> buffer=<n> stall_ms=<n> pb_xruns=<n>
//         cap_xruns=<n> clicks=<n> slip=<frames|n/a> PASS|FAIL
//   (exit 0 = PASS = the stall came back as a playback xrun)
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_CLICKS	64
#define CLICK_LEN	4
#define TAP_CH		4		/* device word 4, the AS1/2 loopback */

static snd_pcm_t *pb, *cap;
static int rate, cap_ch, tap_ok;
static snd_pcm_uframes_t period;
static long total, click_every, stall_at;
static int stall_ms;
static volatile int writer_done;
static int pb_xruns, cap_xruns, errors;

static long pb_click[MAX_CLICKS];	/* playback frame of each click */
static int n_pb_click;
static long pb_restart = -1;		/* first click written after an xrun */
static long cap_click[MAX_CLICKS];	/* capture frame of each tap onset */
static int n_cap_click;

static void *writer_thread(void *arg)
{
	int32_t *buf = calloc(period * 2, sizeof(int32_t));
	long written = 0;
	int stalled = 0;

	if (!buf)
		return NULL;
	while (written < total) {
		snd_pcm_sframes_t w;
		snd_pcm_uframes_t i;

		if (!stalled && written >= stall_at) {
			struct timespec ts = {
				.tv_sec = stall_ms / 1000,
				.tv_nsec = (stall_ms % 1000) * 1000000L,
			};

			nanosleep(&ts, NULL);
			stalled = 1;
		}
		memset(buf, 0, period * 2 * sizeof(int32_t));
		for (i = 0; i < period; i++) {
			long f = written + i;

			if (f % click_every < CLICK_LEN) {
				buf[i * 2] = 0x40000000;	/* -6 dBFS */
				buf[i * 2 + 1] = 0x40000000;
			}
		}
		w = snd_pcm_writei(pb, buf, period);
		if (w == -EPIPE) {
			/* The audio in the ring was dropped: what follows is
			 * a new start, not a continuation.
			 */
			pb_xruns++;
			if (pb_restart < 0)
				pb_restart = n_pb_click;
			snd_pcm_prepare(pb);
			continue;
		}
		if (w < 0) {
			fprintf(stderr, "write: %s\n", snd_strerror(w));
			errors++;
			break;
		}
		for (i = 0; i < (snd_pcm_uframes_t)w; i++)
			if ((written + i) % click_every == 0 &&
			    n_pb_click < MAX_CLICKS)
				pb_click[n_pb_click++] = written + i;
		written += w;
	}
	snd_pcm_drain(pb);
	writer_done = 1;
	free(buf);
	return NULL;
}

static void *reader_thread(void *arg)
{
	int32_t *buf = calloc(period * cap_ch, sizeof(int32_t));
	long frames = 0, quiet = click_every;

	if (!buf)
		return NULL;
	while (!writer_done) {
		snd_pcm_sframes_t r = snd_pcm_readi(cap, buf, period);
		snd_pcm_sframes_t i;

		if (r == -EPIPE) {
			cap_xruns++;
			snd_pcm_prepare(cap);
			snd_pcm_start(cap);
			continue;
		}
		if (r < 0) {
			fprintf(stderr, "read: %s\n", snd_strerror(r));
			errors++;
			break;
		}
		for (i = 0; tap_ok && i < r; i++) {
			int32_t v = buf[i * cap_ch + TAP_CH];

			/* The tap carries the playback about 6 dB down, so a
			 * -6 dBFS click arrives near -12 dBFS.  An onset is
			 * the first frame over -48 dBFS after a quiet stretch.
			 */
			if (v > 0x00800000 || v < -0x00800000) {
				if (quiet > click_every / 2 &&
				    n_cap_click < MAX_CLICKS)
					cap_click[n_cap_click++] = frames + i;
				quiet = 0;
			} else {
				quiet++;
			}
		}
		frames += r;
	}
	free(buf);
	return NULL;
}

static int setup(snd_pcm_t *p, int ch, snd_pcm_uframes_t buffer)
{
	snd_pcm_hw_params_t *hp;
	snd_pcm_sw_params_t *sp;
	int err;

	snd_pcm_hw_params_alloca(&hp);
	snd_pcm_hw_params_any(p, hp);
	if ((err = snd_pcm_hw_params_set_access(p, hp,
			SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
	    (err = snd_pcm_hw_params_set_format(p, hp, SND_PCM_FORMAT_S32_LE)) < 0 ||
	    (err = snd_pcm_hw_params_set_channels(p, hp, ch)) < 0 ||
	    (err = snd_pcm_hw_params_set_rate(p, hp, rate, 0)) < 0 ||
	    (err = snd_pcm_hw_params_set_period_size(p, hp, period, 0)) < 0 ||
	    (err = snd_pcm_hw_params_set_buffer_size(p, hp, buffer)) < 0 ||
	    (err = snd_pcm_hw_params(p, hp)) < 0)
		return err;
	/* Start playback once the ring is full, as a sound server does;
	 * stop on an xrun (the default stop threshold).
	 */
	snd_pcm_sw_params_alloca(&sp);
	snd_pcm_sw_params_current(p, sp);
	if ((err = snd_pcm_sw_params_set_start_threshold(p, sp, buffer)) < 0)
		return err;
	return snd_pcm_sw_params(p, sp);
}

int main(int argc, char **argv)
{
	snd_pcm_uframes_t buffer;
	pthread_t wt, rt;
	char dev[64];
	long slip = 0;
	int err, n, pass, measured;

	if (argc != 5 && argc != 6) {
		fprintf(stderr, "usage: %s card rate period buffer [stall_ms]\n",
			argv[0]);
		return 2;
	}
	rate = atoi(argv[2]);
	period = atoi(argv[3]);
	buffer = atoi(argv[4]);
	stall_ms = argc == 6 ? atoi(argv[5]) :
		   (int)(2000L * buffer / rate) + 20;
	tap_ok = rate <= 48000;
	cap_ch = tap_ok ? 12 : 2;
	click_every = rate / 4;
	stall_at = rate;			/* after one second */
	total = 3L * rate;
	snprintf(dev, sizeof(dev), "hw:%s,0", argv[1]);

	if ((err = snd_pcm_open(&pb, dev, SND_PCM_STREAM_PLAYBACK, 0)) < 0 ||
	    (err = snd_pcm_open(&cap, dev, SND_PCM_STREAM_CAPTURE, 0)) < 0 ||
	    (err = setup(pb, 2, buffer)) < 0 ||
	    (err = setup(cap, cap_ch, buffer)) < 0 ||
	    (err = snd_pcm_prepare(cap)) < 0 ||
	    (err = snd_pcm_start(cap)) < 0) {
		fprintf(stderr, "pcmlate: %s\n", snd_strerror(err));
		return 1;
	}

	/* Playback starts once the writer has filled the ring. */
	pthread_create(&rt, NULL, reader_thread, NULL);
	pthread_create(&wt, NULL, writer_thread, NULL);
	pthread_join(wt, NULL);
	pthread_join(rt, NULL);

	/* The first click gives the locked offset; compare the last click
	 * played with it.  Only meaningful while no xrun restarted
	 * playback, and only if every click came through in order.
	 */
	n = n_pb_click < n_cap_click ? n_pb_click : n_cap_click;
	measured = tap_ok && pb_restart < 0 && n_pb_click == n_cap_click &&
		   n > 1 && pb_click[n - 1] > stall_at;
	if (measured)
		slip = (cap_click[n - 1] - pb_click[n - 1]) -
		       (cap_click[0] - pb_click[0]);

	pass = errors == 0 && pb_xruns > 0;
	printf("rate=%-6d period=%-5lu buffer=%-5lu stall_ms=%-4d pb_xruns=%d "
	       "cap_xruns=%d clicks=%d/%d ", rate, period, buffer, stall_ms,
	       pb_xruns, cap_xruns, n_cap_click, n_pb_click);
	if (measured)
		printf("slip=%ld ", slip);
	else
		printf("slip=n/a ");
	printf("%s\n", pass ? "PASS" : "FAIL");

	snd_pcm_close(cap);
	snd_pcm_close(pb);
	return pass ? 0 : 1;
}
