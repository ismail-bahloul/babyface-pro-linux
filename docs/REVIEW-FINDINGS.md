# Review findings (external AI review) — resolutions

An independent AI review (Claude) was run against the driver via
`docs/REVIEW-BRIEF.md`. Each finding was verified against the real code
before acting; the conclusions and fixes are recorded below. All fixes
are folded into the source (`tools/kernel/babyfacepro{,-ctl}.c`) and
regenerated into the `patches/000[1-4]-*.patch` series.

## Verdicts

| # | Finding | Verdict | Fix |
|---|---|---|---|
| 1 | `probe()` leaks the autosuspend disable on any post-probe failure (`usb_disable_autosuspend` never balanced because `disconnect()` isn't called for a failed probe) | **Real** (medium) | `usb_enable_autosuspend(chip->dev)` added to the `error:` label |
| 2 | `bf_eq_put()` doesn't validate values against the declared bounds → out-of-range value reaches the Q27 math (`bf_exp2` shift by ≥ width = UB) | **Real** (high, latent) — the ALSA core only bounds-checks with `CONFIG_SND_CTL_INPUT_VALIDATION` | Validate every param against its declared range in `bf_eq_put()`, return `-EINVAL` on overflow |
| 3 | `bf_eq_get`/`bf_eq_put` use `.value.integer.value` for the ENUMERATED controls (band type 1-3, slope 14) instead of `.value.enumerated.item` — silent breakage on big-endian | **Real** (endianness) | Use `.value.enumerated.item[0]` for the enum params in both get and put |
| 4 | `bf_panel_balance_wheel()` reads `chip->master[out]` before taking the mutex (inconsistent L/R possible vs the master/mute/dim writers) | **Real** (minor race) | Move the two reads after `mutex_lock()` |
| 5 | `bf_master_put()` has no bounds guard on `l`/`r` (declared max 0x4000); the 16-bit register and cache could hold out-of-spec values | **Real** (robustness) | Reject `l`/`r` > 0x4000 with `-EINVAL`, matching `bf_xpoint_put` |
| 6 | `frames_per_urb`/`nurbs`/`panel_poll_ms` clamped in place in the module globals (benign race if two units probe together; hides the user's value in sysfs) | Real, benign | Clamp into the `chip` fields instead of mutating the globals |
| 7 | `param == 0` casts `bool*` to `s32*` (strict-aliasing, works by padding) | **Real** (cosmetic) | Handle the EQ enable as a plain `bool` (`e->on = !!nv`), no cast |

## Also confirmed / not touched
- 64-bit division → `div_u64`/`div_s64`/`div64_s64` everywhere (the ARM
  build fix) — already done, verified.
- `bf_eq_reupload()` no double-lock (`lockdep_assert_held` in place).
- Lifecycle: `flush_work`/`cancel_work_sync` correctly placed; no UAF found.
- Array indices come from `kctl->private_value` (set at creation), never
  from user input — no OOB-by-index risk.
