# Audit — the stream work queue (`babyface_stream_work`)

Response to Takashi's point:

> The use of interrupt EPs should be fine, as long as it actually works.
> The implementation with work might need more consideration, though.

Audit performed on `tools/kernel/babyfacepro.c`. Two parts: what is
already sound, and what deserves discussion.

---

## 1. What is already handled correctly (verified)

### Process context + bounded time
- `babyface_stream_work()` runs in **process context** (workqueue) —
  required because it makes `usb_control_msg_send()` calls (which sleep).
- Every write is **bounded**: `bf_vendor_write()` passes a `1000` ms
  timeout to `usb_control_msg_send()` (L68-74). So no unbounded stall
  even if the device wedges.

### Lifecycle / synchronization
- `INIT_WORK(&chip->stream_work, …)` at `probe()` (L1162).
- `chip->shutdown` guard at the top of the work (L756) → no-op after teardown.
- `flush_work(&chip->stream_work)` in `babyface_pcm_close()` before
  freeing the substream (L909) → no callback touches `subs` after free.
- `cancel_work_sync(&chip->stream_work)` in `babyface_disconnect()`
  (L1343) and `babyface_suspend()` (L1382) → no UAF on teardown.
- `babyface_panel_stop()` also cancels the (delayed) panel work.

### Robust session accounting
- Start/stop goes through `stream_users` (spin-lock guarded), with
  `schedule_work` on the `0→1` and `1→0` transitions (L1015/1021).
- `bf_recount_users()` re-counts from the substream RUNNING states on the
  error paths, so a fresh increment from a recovering app is not wiped
  (L728). Avoids a "RUNNING substream with no URBs" hang.

### Clean error plan
- `goto err` → `babyface_stream_kill()` (unlinks already-submitted URBs)
  + `babyface_pcm_stop_both(…, SNDRV_PCM_STATE_XRUN)` to wake apps that
  already got a successful trigger (L852-859).

---

## 2. Points worth discussing (the real "more consideration")

### 2.1 Asynchronous start (trigger ≠ stream actually running)
The `trigger(START)` returns **immediately**; the real start
(cold-init + submit URBs) happens later in the work queue. This is the
usual choice for this driver (control transfers must sleep), but it
differs from a standard synchronous trigger. Implications to document:
- an `hw_params`/re-trigger while the work runs is handled via
  `stream_users` + `bf_recount_users`;
- on a failed start we force an XRUN so the app is not left blocked —
  worth explaining in the series comment.

### 2.2 Cold-init + mixer restore on EVERY start (the heavy point)
On every `users>0 && !streaming`, `babyface_stream_work()` replays:
`bf_cold_init()` + trigger + `babyface_restore_state()` +
`bf_state_apply_flags()`. That is on the order of **~100+ serialized
vendor writes**, each potentially up to 1 s.

- **Why**: protocol-mandated — the firmware only validates a session
  preceded by the full cold-init, and the 0x16 clear wipes the whole
  mixer. So this is not an implementation defect.
- **What we can do**:
  a. measure the real start→RUNNING latency (USB sniffer or `pcmxrun`)
     and document it;
  b. check whether any write is redundant when the state did not change
     (the `get`/cache already avoids no-ops in the `_put()` handlers,
     but the restore rewrites everything);
  c. optionally spread the restore over several work iterations so it
     does not come up in one block — but that lengthens startup, a
     trade-off to weigh.

### 2.3 Work scheduled from an URB callback (IRQ context)
`schedule_work()` is called from `babyface_complete_in/out()` when
`urb_err >= BF_URB_ERR_STOP`. This is legal (`schedule_work` is designed
to be called in interrupt context), and the heavy work runs in process
context inside the work. Only need to confirm that no IRQ path does a
`GFP_KERNEL` allocation / sleep — which is the case here (the callbacks
only count and call `schedule_work`).

---

## 3. Verdict

The work queue is **sound on lifecycle** (no UAF, no unbounded stall,
robust accounting). The two real discussion points for Takashi are the
**asynchronous start** (to justify) and the **per-start restore cost**
(protocol-mandated, to measure and document). Nothing blocking found.

## 4. Possible follow-ups
- Measure the start latency (start → RUNNING) and add it to
  `LINUX-VALIDATION.md`.
- Add these justifications (async trigger + protocol-mandated restore)
  to the `babyface_stream_work()` comment to answer the reviewer
  proactively.
