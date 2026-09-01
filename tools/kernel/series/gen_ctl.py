#!/usr/bin/env python3
"""Generate the per-patch babyfacepro-ctl.c variants for the split series.

The full file is cleanly layered (mixer / front-panel / EQ), so each
earlier patch is the full file with the later sections replaced by stubs
that keep the module linkable.

  patch2 = preamble + mixer + panel_stubs + eq_stubs
  patch3 = preamble + mixer + panel    + eq_stubs
  patch4 = preamble + mixer + panel    + eq   (the full source file)

Section boundaries are located by CONTENT (not line numbers), so the
script survives edits that shift lines.  Deterministic: rerunning
reproduces the same output.
"""
import sys

SRC = "babyfacepro-ctl.c"
OUT = "series/ctl-patch%d.c"

lines = open(SRC).read().split("\n")


def find(substr):
    """0-based index of the first line containing substr."""
    for i, l in enumerate(lines):
        if substr in l:
            return i
    sys.exit("marker not found: %r" % substr)


# Content markers that pin the section boundaries.
idx_src = find("const struct bf_source bf_sources[14]")
idx_panel = find("Control indices in chip->panel_kctl")
idx_eq = find("#define BF_EQ_Q27")

# 0-based slices (the blank line right before each marker belongs to the
# preceding slice, giving the separator).
PREAMBLE = "\n".join(lines[0:idx_src])          # SPDX + header + includes + blank
MIXER     = "\n".join(lines[idx_src:idx_panel])  # bf_sources .. just before panel
PANEL     = "\n".join(lines[idx_panel:idx_eq])   # panel section .. just before EQ
EQ        = "\n".join(lines[idx_eq:])            # EQ .. EOF (trailing newline added below)

PANEL_STUBS = """\
/* ── front-panel stubs ────────────────────────────────────────
 * Replaced by the panel patch.  The core driver calls these from
 * probe()/disconnect()/suspend()/resume() and the delayed-work init.
 */
int babyface_create_panel(struct snd_usb_babyface *chip)
{
	return 0;
}

void babyface_panel_start(struct snd_usb_babyface *chip)
{
}

void babyface_panel_stop(struct snd_usb_babyface *chip)
{
}

void babyface_panel_work(struct work_struct *work)
{
}
"""

EQ_STUBS = """\
/* ── DSP-EQ stubs ─────────────────────────────────────────────
 * Replaced by the eq patch.  The core driver calls these from
 * probe() and hw_params() (fs-dependent re-upload).
 */
int babyface_create_eq(struct snd_usb_babyface *chip)
{
	return 0;
}

void bf_eq_reupload(struct snd_usb_babyface *chip)
{
}
"""

patch2 = PREAMBLE + "\n" + MIXER + "\n\n" + PANEL_STUBS + "\n" + EQ_STUBS
patch3 = PREAMBLE + "\n" + MIXER + "\n\n" + PANEL + "\n\n" + EQ_STUBS
# patch 4 = the full source file, verbatim, so the series ends exactly there.
patch4 = open(SRC).read()

for n, text in ((2, patch2), (3, patch3), (4, patch4)):
    with open(OUT % n, "w") as f:
        f.write(text + ("" if text.endswith("\n") else "\n"))
    print("wrote %s (%d lines)" % (OUT % n, text.count("\n") + 1))
