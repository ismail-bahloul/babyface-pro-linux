#!/usr/bin/env python3
"""Split snd-usb-babyface-pro.c into protocol.c / pcm.c / mixer.c /
state.c / main.c + snd-usb-babyface-pro.h.  Line ranges are 1-based
inclusive, taken from the pre-split file.  Deterministic: rerunning
reproduces the same output."""
import re

SRC = "snd-usb-babyface-pro.c"
lines = open(SRC).read().split("\n")  # lines[i] = line i+1


def sl(a, b):
    return "\n".join(lines[a - 1:b])


HEADER_EXTRAS = '''\
/* ── shared driver state ─────────────────────────────────────── */
extern const u16 bf_flag_cycle[4];
extern const struct bf_source bf_sources[14];
extern const u8 bf_xpoint_block[6];
extern const struct snd_pcm_hw_constraint_list bf_rates_constraint;

/* ── protocol.c ──────────────────────────────────────────────── */
int bf_vendor_write(struct snd_usb_babyface *chip, u8 req, u16 val, u16 idx);
int bf_vendor_read(struct snd_usb_babyface *chip, u8 req, u16 idx, u8 *buf);
int bf_cold_init(struct snd_usb_babyface *chip);
int bf_crosspoint_clear_cross(struct snd_usb_babyface *chip,
			      unsigned int blk);
const struct bf_rate *bf_rate_lookup(unsigned int rate);

/* ── pcm.c ───────────────────────────────────────────────────── */
void babyface_stream_kill(struct snd_usb_babyface *chip);
void babyface_pcm_stop_both(struct snd_usb_babyface *chip, int state);
void babyface_stream_work(struct work_struct *work);
extern const struct snd_pcm_ops babyface_pcm_ops;

/* ── mixer.c ─────────────────────────────────────────────────── */
int babyface_write_default_mixer(struct snd_usb_babyface *chip);
int bf_apply_masters(struct snd_usb_babyface *chip);
int bf_loopback_write_map(struct snd_usb_babyface *chip, int out, bool on);
int bf_preamp_state_write(struct snd_usb_babyface *chip);
int babyface_create_controls(struct snd_usb_babyface *chip);
int babyface_create_xpoints(struct snd_usb_babyface *chip);
int babyface_create_flags(struct snd_usb_babyface *chip);

/* ── state.c ─────────────────────────────────────────────────── */
void bf_state_save(struct snd_usb_babyface *chip);
int bf_state_restore(struct snd_usb_babyface *chip);
void bf_state_purge(void);
int babyface_restore_state(struct snd_usb_babyface *chip);
int bf_state_apply_flags(struct snd_usb_babyface *chip);
'''

# bf_rate struct must be in the header: extract it from the rate section
BF_RATE_STRUCT = sl(308, 313)

header = (sl(1, 47) +
          "\n\n" +
          sl(49, 137) + "\n\n" +
          sl(139, 144) + "\n\n" +
          "/* Crosspoint-source order + register block maps (mixer.c). */\n" +
          "extern const struct bf_source bf_sources[14];\n" +
          "extern const u8 bf_xpoint_block[6];\n\n" +
          sl(203, 204) + "\n\n" +
          sl(209, 254) + "\n\n" +
          sl(266, 283) + "\n\n" +
          BF_RATE_STRUCT + "\n\n" +
          "/* Sample-rate / alt classes (protocol.c). */\n" +
          "const struct bf_rate *bf_rate_lookup(unsigned int rate);\n\n" +
          HEADER_EXTRAS)

header = header.replace("// SPDX-License-Identifier: GPL-2.0-only",
                        "/* SPDX-License-Identifier: GPL-2.0-only */", 1)
open("snd-usb-babyface-pro.h", "w").write(header)


def file_head(name, desc):
    return (f"// SPDX-License-Identifier: GPL-2.0-only\n"
            f"/*\n"
            f" * RME Babyface Pro FS — proprietary-mode USB audio driver\n"
            f" * {desc}  See snd-usb-babyface-pro.h for the shared state.\n"
            f" */\n"
            f"#include <linux/log2.h>\n"
            f"#include <linux/module.h>\n"
            f"#include <linux/mutex.h>\n"
            f"#include <linux/unaligned.h>\n"
            f"#include <linux/usb.h>\n"
            f"#include <linux/workqueue.h>\n"
            f"#include <sound/control.h>\n"
            f"#include <sound/tlv.h>\n"
            f"#include <sound/core.h>\n"
            f"#include <sound/initval.h>\n"
            f"#include <sound/pcm.h>\n\n"
            f'#include "snd-usb-babyface-pro.h"\n\n')


def unstatic(text, names):
    for n in names:
        text = re.sub(rf"^static (const )?(?:[\w\s\*]+ )?{n}\(", lambda m: m.group(0).replace("static ", "", 1), text, count=1, flags=re.M)
    return text


# ---- protocol.c -----------------------------------------------------------
proto = (file_head("protocol.c", "USB vendor requests, cold init, rate table.")
         + unstatic(sl(206, 207) + "\n\n" + sl(306, 307) + "\n\n" + sl(315, 346) + "\n\n" + sl(348, 455),
                    ["bf_vendor_write", "bf_vendor_read", "bf_cold_init",
                     "bf_crosspoint_clear_cross", "bf_rate_lookup",
                     "bf_rates", "bf_rate_list", "bf_rates_constraint"])
         + "\n")
# bf_flag_cycle is `static const u16` — handled by the generic unstatic? no,
# its line is "static const u16 bf_flag_cycle[4]" — the regex needs the [4].
proto = proto.replace("static const u16 bf_flag_cycle[4]",
                      "const u16 bf_flag_cycle[4]")
# bf_rates[] / bf_rate_list[] / bf_rates_constraint are static const arrays
proto = proto.replace("static const struct bf_rate bf_rates[]",
                      "const struct bf_rate bf_rates[]")
proto = proto.replace("static const unsigned int bf_rate_list[ARRAY_SIZE(bf_rates)]",
                      "const unsigned int bf_rate_list[ARRAY_SIZE(bf_rates)]")
proto = proto.replace("static const struct snd_pcm_hw_constraint_list bf_rates_constraint",
                      "const struct snd_pcm_hw_constraint_list bf_rates_constraint")
# bf_rate_lookup: non-static (exported to pcm.c/main.c)
proto = proto.replace("static const struct bf_rate *bf_rate_lookup(unsigned int rate)",
                      "const struct bf_rate *bf_rate_lookup(unsigned int rate)")
open("protocol.c", "w").write(proto)

# ---- mixer.c --------------------------------------------------------------
mixer = (file_head("mixer.c", "Mixer controls: masters, preamp, crosspoints, flags, gains.")
         + unstatic(sl(146, 161) + "\n\n" + sl(163, 171) + "\n\n" + sl(173, 201) + "\n\n" +
                    sl(457, 535) + "\n\n" + sl(767, 804) + "\n\n" + sl(1115, 2026),
                    ["babyface_write_default_mixer", "bf_apply_masters",
                     "bf_loopback_write_map", "bf_preamp_state_write",
                     "babyface_create_xpoints",
                     "babyface_create_flags", "babyface_create_controls"])
         + "\n")
mixer = mixer.replace("static const struct bf_source bf_sources[14]",
                      "const struct bf_source bf_sources[14]")
mixer = mixer.replace("static const u8 bf_xpoint_block[6]",
                      "const u8 bf_xpoint_block[6]")
open("mixer.c", "w").write(mixer)

# ---- pcm.c ----------------------------------------------------------------
pcm = (file_head("pcm.c", "PCM: interrupt-URB stream, copy, trigger, pointer.")
       + unstatic(sl(537, 754) + "\n\n" + sl(756, 765) + "\n\n" + sl(806, 808) +
                  "\n\n" + sl(811, 825) + "\n\n" + sl(827, 934) + "\n\n" +
                  sl(936, 1113),
                  ["babyface_stream_kill", "babyface_pcm_stop_both",
                   "babyface_stream_work", "babyface_pcm_ops"])
       + "\n")
open("pcm.c", "w").write(pcm)

# ---- state.c --------------------------------------------------------------
state = (file_head("state.c", "Mixer-state persistence across re-probes + resume re-apply.")
         + sl(256, 265) + "\n\n" + sl(285, 286) + "\n\n"
         + unstatic(sl(2296, 2545),
                    ["babyface_restore_state", "bf_state_apply_flags",
                     "bf_state_save", "bf_state_restore", "bf_state_purge"])
         + "\n")
open("state.c", "w").write(state)

# ---- main.c ---------------------------------------------------------------
mainc = (file_head("main.c", "Card lifecycle: probe/disconnect, PM, module entry.")
         + sl(292, 304) + "\n\n" + sl(2028, 2294) + "\n\n" + sl(2547, 2604) + "\n")
open("main.c", "w").write(mainc)

print("split done:")
for f in ("snd-usb-babyface-pro.h", "protocol.c", "mixer.c", "pcm.c", "state.c", "main.c"):
    print(f"  {f}: {len(open(f).read().splitlines())} lines")
