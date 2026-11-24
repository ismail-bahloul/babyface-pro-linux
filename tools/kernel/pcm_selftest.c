/* pcm_selftest.c - the PCM completion path without the card: the
 * driver's own URB completion handlers and ring copies, fed simulated
 * URBs.  It checks the ring wrap and the period accounting in both
 * directions at every frame size, the silence sent when nothing plays,
 * and the error handling: a run of bad URBs stops the session, a good
 * one in between resets the count, a killed URB is neither counted nor
 * resubmitted, and a failed resubmit is fatal at once.
 *
 * The functions are the driver's, pulled out of babyfacepro.c by
 * extract_laws.py against the kernel stand-ins in pcm_shim.h - run
 * through selftests.sh.
 *
 * laws: babyface_complete_in babyface_complete_out BF_URB_ERR_STOP
 */
#include <stdio.h>
#include <stdlib.h>

static int fails;

static void chk(const char *name, long got, long want)
{
	if (got != want) {
		printf("FAIL %-44s got %ld want %ld\n", name, got, want);
		fails++;
	} else {
		printf("ok   %-44s %ld\n", name, got);
	}
}

/* Frame sizes of the three alternate settings: 14 words at 32-48 kHz,
 * 10 at 64-96 kHz, 8 at 128-192 kHz (babyfacepro.h).
 */
static const unsigned int frame_words[] = { 14, 10, 8 };

#define CHANNELS	12
#define RING_FRAMES	80	/* not a multiple of the URB size: wraps mid-URB */
#define PERIOD		40
#define URB_FRAMES	32

struct rig {
	struct usb_device udev;
	struct snd_usb_babyface chip;
	struct snd_pcm_runtime rt;
	struct snd_pcm_substream subs;
	struct urb urb;
	u8 ring[RING_FRAMES * CHANNELS * 4];
	u8 buf[URB_FRAMES * 14 * 4];
};

static void rig_init(struct rig *r, int dir, unsigned int words)
{
	memset(r, 0, sizeof(*r));
	r->chip.dev = &r->udev;
	r->chip.frame_bytes = words * 4;
	r->chip.streaming = true;
	r->rt.dma_area = r->ring;
	r->rt.buffer_size = RING_FRAMES;
	r->rt.period_size = PERIOD;
	r->rt.channels = CHANNELS;
	r->subs.runtime = &r->rt;
	r->subs.running = true;
	r->chip.subs[dir] = &r->subs;
	r->urb.context = &r->chip;
	r->urb.transfer_buffer = r->buf;
	r->urb.transfer_buffer_length = URB_FRAMES * r->chip.frame_bytes;
	r->urb.actual_length = r->urb.transfer_buffer_length;
}

/* A sample value that names its frame and word. */
static uint32_t sample(unsigned int frame, unsigned int word)
{
	return (frame << 16 | word << 8 | 0x5a) & 0xffffffff;
}

static uint32_t word_at(const u8 *p, unsigned int i)
{
	uint32_t v;

	memcpy(&v, p + i * 4, 4);
	return v;
}

static void capture_wraps(unsigned int words)
{
	struct rig r;
	unsigned int urb, f, w, frame = 0, bad = 0, ring_pos;
	char name[64];

	rig_init(&r, SNDRV_PCM_STREAM_CAPTURE, words);
	/* Five URBs: 160 frames through an 80-frame ring. */
	for (urb = 0; urb < 5; urb++) {
		for (f = 0; f < URB_FRAMES; f++)
			for (w = 0; w < words; w++) {
				uint32_t v = sample(frame + f, w);

				memcpy(r.buf + (f * words + w) * 4, &v, 4);
			}
		babyface_complete_in(&r.urb);
		frame += URB_FRAMES;
	}
	/* The ring holds the last 80 frames, each at frame % 80. */
	for (f = frame - RING_FRAMES; f < frame; f++) {
		ring_pos = f % RING_FRAMES;
		for (w = 0; w < CHANNELS; w++) {
			uint32_t want = w < words ? sample(f, w) : 0;

			if (word_at(r.ring + ring_pos * CHANNELS * 4, w) != want)
				bad++;
		}
	}
	snprintf(name, sizeof(name), "capture %u words: ring content", words);
	chk(name, bad, 0);
	snprintf(name, sizeof(name), "capture %u words: hw_ptr", words);
	chk(name, r.chip.hw_ptr[SNDRV_PCM_STREAM_CAPTURE], 160);
	/* Boundaries at 40, 80, 120 and 160: the URBs ending at 64, 96,
	 * 128 and 160 cross one each.
	 */
	snprintf(name, sizeof(name), "capture %u words: periods elapsed", words);
	chk(name, r.subs.periods_elapsed, 4);
	snprintf(name, sizeof(name), "capture %u words: resubmitted", words);
	chk(name, r.urb.submitted, 5);
}

static void playback_wraps(unsigned int words)
{
	struct rig r;
	unsigned int urb, f, w, frame = 0, bad = 0;
	char name[64];

	rig_init(&r, SNDRV_PCM_STREAM_PLAYBACK, words);
	for (f = 0; f < RING_FRAMES; f++)
		for (w = 0; w < CHANNELS; w++) {
			uint32_t v = sample(f, w);

			memcpy(r.ring + (f * CHANNELS + w) * 4, &v, 4);
		}
	for (urb = 0; urb < 5; urb++) {
		memset(r.buf, 0xee, sizeof(r.buf));
		babyface_complete_out(&r.urb);
		for (f = 0; f < URB_FRAMES; f++)
			for (w = 0; w < words; w++) {
				/* App channels past the frame are dropped;
				 * words past the channels are zero.
				 */
				uint32_t want = w < CHANNELS ?
					sample((frame + f) % RING_FRAMES, w) : 0;

				if (word_at(r.buf, f * words + w) != want)
					bad++;
			}
		frame += URB_FRAMES;
	}
	snprintf(name, sizeof(name), "playback %u words: URB content", words);
	chk(name, bad, 0);
	snprintf(name, sizeof(name), "playback %u words: hw_ptr", words);
	chk(name, r.chip.hw_ptr[SNDRV_PCM_STREAM_PLAYBACK], 160);
	snprintf(name, sizeof(name), "playback %u words: periods elapsed", words);
	chk(name, r.subs.periods_elapsed, 4);
}

static void playback_idle_sends_silence(void)
{
	struct rig r;
	unsigned int i, nonzero = 0;

	rig_init(&r, SNDRV_PCM_STREAM_PLAYBACK, 14);
	memset(r.ring, 0x77, sizeof(r.ring));
	r.subs.running = false;		/* set up, not started */
	memset(r.buf, 0xee, sizeof(r.buf));
	babyface_complete_out(&r.urb);
	for (i = 0; i < r.urb.transfer_buffer_length; i++)
		nonzero += r.buf[i] != 0;
	chk("idle playback: URB is silence", nonzero, 0);
	chk("idle playback: hw_ptr stays", r.chip.hw_ptr[0], 0);
	chk("idle playback: resubmitted", r.urb.submitted, 1);

	r.chip.subs[SNDRV_PCM_STREAM_PLAYBACK] = NULL;	/* closed */
	memset(r.buf, 0xee, sizeof(r.buf));
	babyface_complete_out(&r.urb);
	nonzero = 0;
	for (i = 0; i < r.urb.transfer_buffer_length; i++)
		nonzero += r.buf[i] != 0;
	chk("closed playback: URB is silence", nonzero, 0);
}

static void error_runs(void)
{
	struct rig r;
	int i;

	rig_init(&r, SNDRV_PCM_STREAM_CAPTURE, 14);
	/* Two bad URBs, then a good one: the run is forgiven. */
	r.urb.status = -EPROTO;
	babyface_complete_in(&r.urb);
	babyface_complete_in(&r.urb);
	chk("2 bad IN URBs: counted", atomic_read(&r.chip.urb_err), 2);
	chk("2 bad IN URBs: still resubmitted", r.urb.submitted, 2);
	r.urb.status = 0;
	babyface_complete_in(&r.urb);
	chk("then a good one: count cleared", atomic_read(&r.chip.urb_err), 0);
	chk("then a good one: no stop", r.chip.stream_work.scheduled, 0);

	/* BF_URB_ERR_STOP in a row: stop, and no resubmit of the last. */
	r.urb.status = -EPROTO;
	r.urb.submitted = 0;
	for (i = 0; i < BF_URB_ERR_STOP; i++)
		babyface_complete_in(&r.urb);
	chk("a full run of bad URBs: stop queued", r.chip.stream_work.scheduled, 1);
	chk("a full run of bad URBs: last not resubmitted", r.urb.submitted,
	    BF_URB_ERR_STOP - 1);
	/* A good URB after the stop must not clear the count. */
	r.urb.status = 0;
	babyface_complete_in(&r.urb);
	chk("good URB after stop: count stands", atomic_read(&r.chip.urb_err),
	    BF_URB_ERR_STOP);
}

static void killed_urbs(void)
{
	static const int killed[] = { -ESHUTDOWN, -ENOENT, -ECONNRESET, -ENODEV };
	struct rig r;
	unsigned int i;

	rig_init(&r, SNDRV_PCM_STREAM_CAPTURE, 14);
	for (i = 0; i < 4; i++) {
		r.urb.status = killed[i];
		babyface_complete_in(&r.urb);
		babyface_complete_out(&r.urb);
	}
	chk("killed URBs: not counted", atomic_read(&r.chip.urb_err), 0);
	chk("killed URBs: not resubmitted", r.urb.submitted, 0);

	/* A session being stopped: completions retire their URBs. */
	r.urb.status = 0;
	r.chip.streaming = false;
	babyface_complete_in(&r.urb);
	babyface_complete_out(&r.urb);
	chk("stopping session: not resubmitted", r.urb.submitted, 0);
	chk("stopping session: not an error", atomic_read(&r.chip.urb_err), 0);
}

static void resubmit_failure(void)
{
	struct rig r;

	rig_init(&r, SNDRV_PCM_STREAM_PLAYBACK, 14);
	r.chip.out_inflight = 2;
	usb_submit_ret = -EPERM;
	babyface_complete_out(&r.urb);
	usb_submit_ret = 0;
	chk("failed OUT resubmit: fatal at once", atomic_read(&r.chip.urb_err),
	    BF_URB_ERR_STOP);
	chk("failed OUT resubmit: stop queued", r.chip.stream_work.scheduled, 1);
	chk("failed OUT resubmit: URB leaves the queue", r.chip.out_inflight, 1);

	/* A run of bad OUT URBs that stops the session also retires it. */
	rig_init(&r, SNDRV_PCM_STREAM_PLAYBACK, 14);
	r.chip.out_inflight = 2;
	r.urb.status = -EPROTO;
	for (int i = 0; i < BF_URB_ERR_STOP; i++)
		babyface_complete_out(&r.urb);
	chk("stopping OUT error run: URB leaves the queue", r.chip.out_inflight, 1);

	rig_init(&r, SNDRV_PCM_STREAM_CAPTURE, 14);
	usb_submit_ret = -EPERM;
	babyface_complete_in(&r.urb);
	usb_submit_ret = 0;
	chk("failed IN resubmit: fatal at once", atomic_read(&r.chip.urb_err),
	    BF_URB_ERR_STOP);
}

static void short_capture(void)
{
	struct rig r;

	rig_init(&r, SNDRV_PCM_STREAM_CAPTURE, 14);
	/* A short IN URB moves the pointer by the frames it carries. */
	r.urb.actual_length = 10 * r.chip.frame_bytes + 3;
	babyface_complete_in(&r.urb);
	chk("short IN URB: whole frames only", r.chip.hw_ptr[1], 10);
	r.urb.actual_length = 0;
	babyface_complete_in(&r.urb);
	chk("empty IN URB: pointer stays", r.chip.hw_ptr[1], 10);
	chk("empty IN URB: still resubmitted", r.urb.submitted, 2);
	chk("IN completion stamps in_done", r.chip.in_done != 0, 1);
}

int main(void)
{
	unsigned int i;

	for (i = 0; i < 3; i++) {
		capture_wraps(frame_words[i]);
		playback_wraps(frame_words[i]);
	}
	playback_idle_sends_silence();
	error_runs();
	killed_urbs();
	resubmit_failure();
	short_capture();
	if (fails) {
		printf("%d FAILED\n", fails);
		return 1;
	}
	printf("ok all\n");
	return 0;
}
