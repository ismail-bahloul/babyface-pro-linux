#!/usr/bin/env python3
"""mix_test.py — validate the kernel driver's MIX (fader) mode
(tools/kernel/panel.c, TODO 0d): press MIX on the card, turn the
wheel → the AN1/AN2 Playback Volume faders of the OUT-selected output
must move (the driver writes the standard-map crosspoints); MIX again
→ back to gain mode; IN during MIX → gain control.

Usage: mix_test.py [card] [seconds]      (default: card 3, 20 s)

Prints the Front Panel state + the input faders (AN1..AN4, AS1/2) of
the OUT-selected output, and any change.  Expected with the driver
working:
  MIX ON  -> "Front Panel Mix" flips to 1
  wheel   -> the IN-selected pair's fader values change (0.5 dB steps)
  MIX     -> "Front Panel Mix" flips back to 0 (gain mode)
"""
import ctypes, sys, time

CARD = sys.argv[1] if len(sys.argv) > 1 else "3"
DUR = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0

lib = ctypes.CDLL("libasound.so.2")
ctl_t = ctypes.c_void_p
id_t = ctypes.c_void_p
val_t = ctypes.c_void_p

lib.snd_ctl_open.argtypes = [ctypes.POINTER(ctl_t), ctypes.c_char_p, ctypes.c_int]
lib.snd_ctl_open.restype = ctypes.c_int
lib.snd_ctl_close.argtypes = [ctl_t]
lib.snd_ctl_elem_id_malloc.argtypes = [ctypes.POINTER(id_t)]
lib.snd_ctl_elem_id_malloc.restype = ctypes.c_int
lib.snd_ctl_elem_id_free.argtypes = [id_t]
lib.snd_ctl_elem_id_set_interface.argtypes = [id_t, ctypes.c_int]
lib.snd_ctl_elem_id_set_name.argtypes = [id_t, ctypes.c_char_p]
lib.snd_ctl_elem_id_set_index.argtypes = [id_t, ctypes.c_uint]
lib.snd_ctl_elem_value_malloc.argtypes = [ctypes.POINTER(val_t)]
lib.snd_ctl_elem_value_malloc.restype = ctypes.c_int
lib.snd_ctl_elem_value_free.argtypes = [val_t]
lib.snd_ctl_elem_value_set_id.argtypes = [val_t, id_t]
lib.snd_ctl_elem_read.argtypes = [ctl_t, val_t]
lib.snd_ctl_elem_read.restype = ctypes.c_int
lib.snd_ctl_elem_value_get_integer.argtypes = [val_t, ctypes.c_uint]
lib.snd_ctl_elem_value_get_enumerated.argtypes = [val_t, ctypes.c_uint]

IN_TXT = ["Unknown", "Ch 1/2", "Ch 3/4", "Opt"]
OUT_TXT = ["Unknown", "Ch 1/2", "Phones", "Opt"]

ctl = ctl_t()
if lib.snd_ctl_open(ctypes.byref(ctl), ("hw:%s" % CARD).encode(), 0) < 0:
    sys.exit("cannot open card hw:%s" % CARD)

def elem(name, index=0):
    eid = id_t()
    lib.snd_ctl_elem_id_malloc(ctypes.byref(eid))
    lib.snd_ctl_elem_id_set_interface(eid, 2)  # SNDRV_CTL_ELEM_IFACE_MIXER
    lib.snd_ctl_elem_id_set_name(eid, name.encode())
    lib.snd_ctl_elem_id_set_index(eid, index)
    v = val_t()
    lib.snd_ctl_elem_value_malloc(ctypes.byref(v))
    lib.snd_ctl_elem_value_set_id(v, eid)
    return v

def rd(v):
    return lib.snd_ctl_elem_read(ctl, v) == 0

def get_int(v):
    return lib.snd_ctl_elem_value_get_integer(v, 0)

mix = elem("Front Panel Mix")
out = elem("Front Panel Out")
inp = elem("Front Panel In")
wheel = elem("Front Panel Wheel")
btn = elem("Front Panel Button")
sel = elem("Front Panel Select")
BTN_TXT = {0: "none", 1: "IN", 2: "SET", 3: "MIX", 4: "OUT", 5: "SELECT", 6: "DIM"}
SEL_TXT = ["Left", "Right", "Both", "None"]
OUT_NAMES = ["AN1/2", "PH3/4", "AS1/2", "ADAT3/4", "ADAT5/6", "ADAT7/8"]
gains = [elem("Mic 1 Capture Volume", i) for i in range(4)]
GAN = {0: "AN1 gain", 1: "AN2 gain", 2: "AN3 gain", 3: "AN4 gain"}

# The faders the MIX wheel can drive: the input sources (AN1..AN4,
# AS1/2 = src 0..4) into the OUT-selected output.  Canonical out:
# Ch1/2=0, Phones=1, Opt=5.  Watching the whole src set so the wheel
# is visible whatever the IN pair selection is.
out2canon = {1: 0, 2: 1, 3: 5}
SRC_NAMES = ["AN1", "AN2", "AN3", "AN4", "AS1/2"]


def fader_elems(canon_out):
    base = canon_out * 14
    return [(n, elem(n + " Playback Volume", base + s))
            for s, n in enumerate(SRC_NAMES)]

last = {"mix": None, "out": None, "in": None, "wheel": 0, "btn": 0,
        "sel": None, "fvals": {}, "gvals": {}}

print("mix_test: card %s — press MIX on the card, turn the wheel, MIX again"
      % CARD, flush=True)
t0 = time.monotonic()
while time.monotonic() - t0 < DUR:
    if rd(mix):
        m = get_int(mix)
        if m != last["mix"]:
            last["mix"] = m
            print("[%6.2f] MIX %s" % (time.monotonic() - t0, "ON (fader mode)" if m else "off (gain mode)"), flush=True)
    if rd(out):
        o = get_int(out)
        if o != last["out"]:
            last["out"] = o
            o_txt = OUT_TXT[o] if 0 <= o < 4 else "?%d" % o
            print("[%6.2f] OUT = %s" % (time.monotonic() - t0, o_txt), flush=True)
            # re-bind the fader controls to the newly selected output
            if o in out2canon:
                last["f1"] = last["f2"] = None
                last["_faders"] = fader_elems(out2canon[o])
    if rd(inp):
        i = get_int(inp)
        if i != last["in"]:
            last["in"] = i
            print("[%6.2f] IN  = %s" % (time.monotonic() - t0, IN_TXT[i] if 0 <= i < 4 else "?%d" % i), flush=True)
    if rd(btn):
        b = get_int(btn)
        if b and b != last["btn"]:
            last["btn"] = b
            print("[%6.2f] BUTTON %s"
                  % (time.monotonic() - t0, BTN_TXT.get(b, "?%d" % b)), flush=True)
    if rd(sel):
        s = get_int(sel)
        if s != last["sel"]:
            last["sel"] = s
            print("[%6.2f] SELECT = %s"
                  % (time.monotonic() - t0,
                     SEL_TXT[s] if 0 <= s < 4 else "?%d" % s), flush=True)
    if rd(wheel):
        w = get_int(wheel)
        d = w - last["wheel"]
        if d > 32767:
            d -= 65536
        elif d < -32768:
            d += 65536
        if d:
            print("[%6.2f] WHEEL %+d" % (time.monotonic() - t0, d), flush=True)
        last["wheel"] = w
    faders = last.get("_faders")
    if faders is None and last["out"] in out2canon:
        faders = last["_faders"] = fader_elems(out2canon[last["out"]])
    if faders:
        for name, f in faders:
            if rd(f):
                v = get_int(f)
                if v != last["fvals"].get(name):
                    last["fvals"][name] = v
                    print("[%6.2f] %-6s fader = 0x%04x"
                          % (time.monotonic() - t0, name, v), flush=True)
    # the OUT-selected output's master + the mic gains (OUT/gain wheel)
    if last["out"] in out2canon:
        co = out2canon[last["out"]]
        if last.get("_mout") != co:
            last["_master"] = elem(OUT_NAMES[co] + " Playback Volume", co)
            last["_mout"] = co
        if rd(last["_master"]):
            v = get_int(last["_master"])
            if v != last["fvals"].get("master"):
                last["fvals"]["master"] = v
                print("[%6.2f] %-6s master = 0x%04x"
                      % (time.monotonic() - t0, OUT_NAMES[co], v), flush=True)
    for i, g in enumerate(gains):
        if rd(g):
            v = get_int(g)
            if v != last["gvals"].get(i):
                last["gvals"][i] = v
                print("[%6.2f] %-8s = %d dB"
                      % (time.monotonic() - t0, GAN[i], v), flush=True)
    time.sleep(0.05)
