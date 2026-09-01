# Reply to Takashi — v2 review (draft to send / adapt)

Send as a reply to Takashi's v2 review email, or fold into the v3 cover
letter.  Draft in English (kernel convention).

---

On Tue, 01 Sep 2026, Takashi Iwai wrote:

> You don't have to put Reported-by / Closes tags here.

Done - removed them from the commits.  They belong on follow-up fixes
to an already-applied patch, not a fresh submission.

> And, above all, please try to give some "big picture" of the design of
> the driver.  It's not clear what the stream_work does and how the
> stream numbers are managed and how they influence on what.

Done.  Added a "Stream model (big picture)" section to the [1/4] commit
message and a longer comment above `babyface_stream_work()`.  In short:
the PCM stream is fully asynchronous.  A `trigger(START/STOP)` only
toggles the shared `stream_users` counter (0..2, one per running
substream, under `chip->lock`) and schedules `stream_work`.  The work
runs in process context (the control transfers and `usb_submit_urb()`
must sleep): the first user cold-inits, submits the interrupt URBs and
arms the session; the last user kills them.  So there is exactly one
live stream regardless of substream count, shared by playback and
capture; `bf_recount_users()` on the error path re-syncs `stream_users`
to the RUNNING substreams so a recovering app re-arms cleanly.

> SPDX tag is missing in Makefile?

Added.

> Avoid non-ASCII letters as much as possible.  LLM tends to put too
> fancy letters and comment styles...

Done - all comments are plain ASCII now (dashes, arrows, box-drawing
separators and the like are gone).

> Avoid magic 1000.  It's a timeout for 1000ms, so define it.

Done - `BF_CTL_TIMEOUT` (1000) used in `bf_vendor_write/read`.

> Those L and R registers should be defined properly, instead of
> hard-coded magic numbers in the loop condition.

Done - `BF_CROSS_L_FIRST/LAST` and `BF_CROSS_R_FIRST/LAST` in the
header, used in `bf_crosspoint_clear_cross()`.

> Maybe it should be better in a helper, e.g. bf_vendor_write_cycle()?

Done - added `bf_vendor_write_cycle()` and used it in
`bf_crosspoint_clear_cross()`.  I kept the explicit form at the master /
mute writes because there a single flag word is shared by the L+R pair
of a write (both 16-bit writes carry the same flag), so a mechanical
replacement would change the validated flag sequence; I'm happy to
extend the helper if you'd prefer it uniform.

> Hmm, this could be done by alsactl restore, in general, too?  Though,
> the state save/restore could be used for the runtime PM, too...

Agreed it overlaps with what `alsactl restore` does.  I kept it in the
driver because the firmware has no mixer readback and the save/restore
also covers the usbfs detach/re-probe path and is the basis for a future
runtime-PM state restore, but I've noted it as a follow-up to revisit.

> Why do you have to convert to S24_LE at all?  You can use S32_LE with
> msbits.  That's a far more standard format... just copy the data
> as-is.

Done - the PCM is now S32_LE with `snd_pcm_hw_constraint_msbits(runtime,
0, 32, 24)`; both directions copy the 32-bit word as-is (the 24 valid
bits are left-justified), no shifting.

> Hmm, this sounds weird [the playback clamp]...

Rewrote the comment and the variable names, and confirmed the logic:
with several URBs in flight the device can transiently be ahead of the
app ring at low periods, so we never copy frames past `appl_ptr` (that
would trip a spurious XRUN); the device repeats the last frames.  The
clamp is on the signed `appl - hw` difference because both counters are
unbounded.

I'd like to keep it: without it the driver reports hw ahead of appl at
low periods (16-128 frames with nurbs=16, the monitoring floor) and the
core flags a spurious XRUN even though the app refills on schedule.  It
is what keeps the low-latency sweep at 0 xruns.  If you still prefer the
core underrun path to handle this, I'm happy to drop it - just say so.

Thanks for the review,
Ismaïl
