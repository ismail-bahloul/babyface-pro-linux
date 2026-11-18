/* pcm_shim.h - the kernel types and helpers the PCM completion path uses,
 * reduced to what pcm_selftest.c needs to run that path in userspace.
 *
 * selftests.sh includes it before the header extract_laws.py builds, so
 * the driver's own completion handlers compile against it unchanged.
 * struct snd_usb_babyface holds only the fields the extracted functions
 * touch, under the driver's names: a field renamed in the driver breaks
 * the build here, which is the point.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t u8;

#define min(a, b)		((a) < (b) ? (a) : (b))
#define READ_ONCE(x)		(x)
#define WRITE_ONCE(x, v)	((x) = (v))
#define GFP_ATOMIC		0
#define __rcu
#define rcu_dereference(p)	(p)

/* Locks: the test is single-threaded, so a guard only checks that it is
 * given something to lock.
 */
typedef int spinlock_t;
#define guard(type)		guard_##type
#define guard_spinlock(l)	((void)(l))
#define guard_spinlock_irqsave(l)	((void)(l))
#define guard_pcm_stream_lock_irqsave(s)	((void)(s))
#define scoped_guard(...)	for (int _sg_once = 1; _sg_once; _sg_once = 0)

typedef int atomic_t;
static inline int atomic_inc_return(atomic_t *v) { return ++*v; }
static inline int atomic_read(atomic_t *v) { return *v; }
static inline void atomic_set(atomic_t *v, int i) { *v = i; }
static inline bool atomic_try_cmpxchg(atomic_t *v, int *old, int new)
{
	if (*v == *old) {
		*v = new;
		return true;
	}
	*old = *v;
	return false;
}

struct work_struct { int scheduled; };
static inline bool schedule_work(struct work_struct *w)
{
	w->scheduled++;
	return true;
}

typedef long long ktime_t;
static inline ktime_t ktime_get(void) { return 1; }

struct device { int unused; };
struct usb_device { struct device dev; };
#define dev_dbg_ratelimited(d, ...)	((void)(d))
#define dev_err_ratelimited(d, ...)	((void)(d))

#define SNDRV_PCM_STREAM_PLAYBACK	0
#define SNDRV_PCM_STREAM_CAPTURE	1

struct snd_pcm_runtime {
	u8 *dma_area;
	unsigned long buffer_size;
	unsigned long period_size;
	unsigned int channels;
};
/* S32_LE: four bytes per sample. */
#define frames_to_bytes(rt, f)	((f) * (rt)->channels * 4)

struct snd_pcm_substream {
	struct snd_pcm_runtime *runtime;
	bool running;
	int periods_elapsed;
};
#define snd_pcm_running(s)	((s)->running)
static inline void snd_pcm_period_elapsed(struct snd_pcm_substream *s)
{
	s->periods_elapsed++;
}

struct urb {
	void *context;
	int status;
	unsigned int actual_length;
	unsigned int transfer_buffer_length;
	u8 *transfer_buffer;
	int submitted;
};
/* What the next usb_submit_urb() returns: 0, or an error to inject. */
static int usb_submit_ret;
static inline int usb_submit_urb(struct urb *urb, int mem_flags)
{
	(void)mem_flags;
	if (usb_submit_ret)
		return usb_submit_ret;
	urb->submitted++;
	return 0;
}

struct snd_usb_babyface {
	struct usb_device *dev;
	spinlock_t lock;
	unsigned int frame_bytes;
	bool streaming;
	atomic_t urb_err;
	struct work_struct stream_work;
	struct snd_pcm_substream __rcu *subs[2];
	unsigned long hw_ptr[2];
	unsigned long prev_period[2];
	ktime_t in_done;
	unsigned int out_inflight;
};
