#!/usr/bin/env python3
"""panel_monitor.py — watch the kernel driver's Front Panel ALSA controls
in real time.  Button and Wheel are consumed on read (latched), so every
poll prints the events accumulated since the previous read.

Usage: panel_monitor.py [card] [hz]     (default: card 3, 20 Hz)
"""
import ctypes, sys, time

CARD = sys.argv[1] if len(sys.argv) > 1 else "3"
HZ = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0

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
lib.snd_ctl_elem_value_malloc.argtypes = [ctypes.POINTER(val_t)]
lib.snd_ctl_elem_value_malloc.restype = ctypes.c_int
lib.snd_ctl_elem_value_free.argtypes = [val_t]
lib.snd_ctl_elem_value_set_id.argtypes = [val_t, id_t]
lib.snd_ctl_elem_read.argtypes = [ctl_t, val_t]
lib.snd_ctl_elem_read.restype = ctypes.c_int
lib.snd_ctl_elem_value_get_integer.argtypes = [val_t, ctypes.c_uint]
lib.snd_ctl_elem_value_get_enumerated.argtypes = [val_t, ctypes.c_uint]

BTN = {0: "none", 1: "IN", 2: "SET", 3: "MIX", 4: "OUT", 5: "SELECT", 6: "DIM"}
IN_TXT = ["Unknown", "Ch 1/2", "Ch 3/4", "Opt"]
OUT_TXT = ["Unknown", "Ch 1/2", "Phones", "Opt"]

ctl = ctl_t()
if lib.snd_ctl_open(ctypes.byref(ctl), ("hw:%s" % CARD).encode(), 0) < 0:
    sys.exit("cannot open card hw:%s" % CARD)

def elem(name):
    eid = id_t()
    lib.snd_ctl_elem_id_malloc(ctypes.byref(eid))
    lib.snd_ctl_elem_id_set_interface(eid, 2)  # SNDRV_CTL_ELEM_IFACE_MIXER
    lib.snd_ctl_elem_id_set_name(eid, name.encode())
    v = val_t()
    lib.snd_ctl_elem_value_malloc(ctypes.byref(v))
    lib.snd_ctl_elem_value_set_id(v, eid)
    return v

def rd(v):
    return lib.snd_ctl_elem_read(ctl, v) == 0

names = ["Front Panel Button", "Front Panel Wheel", "Front Panel In",
         "Front Panel Out", "Front Panel Mix", "Front Panel Dim"]
vals = {n: elem(n) for n in names}
last = {"In": None, "Out": None, "Mix": None, "Dim": None,
        "Button": 0, "Wheel": 0}

print("watching Front Panel controls on card %s at %.0f Hz — press buttons / turn the wheel"
      % (CARD, HZ), flush=True)
t0 = time.monotonic()
while True:
    if rd(vals["Front Panel Button"]):
        b = lib.snd_ctl_elem_value_get_integer(vals["Front Panel Button"], 0)
        if b and b != last["Button"]:
            print("[%7.2f] BUTTON %s" % (time.monotonic() - t0, BTN.get(b, "?%d" % b)), flush=True)
        last["Button"] = b
    if rd(vals["Front Panel Wheel"]):
        w = lib.snd_ctl_elem_value_get_integer(vals["Front Panel Wheel"], 0)
        d = w - last["Wheel"]
        if d > 32767: d -= 65536
        elif d < -32768: d += 65536
        if d:
            print("[%7.2f] WHEEL %+d (acc %d)" % (time.monotonic() - t0, d, w), flush=True)
        last["Wheel"] = w
    if rd(vals["Front Panel In"]):
        i = lib.snd_ctl_elem_value_get_enumerated(vals["Front Panel In"], 0)
        if i != last["In"]:
            last["In"] = i
            print("[%7.2f] IN  = %s" % (time.monotonic() - t0, IN_TXT[i] if 0 <= i < 4 else "?%d" % i), flush=True)
    if rd(vals["Front Panel Out"]):
        o = lib.snd_ctl_elem_value_get_enumerated(vals["Front Panel Out"], 0)
        if o != last["Out"]:
            last["Out"] = o
            print("[%7.2f] OUT = %s" % (time.monotonic() - t0, OUT_TXT[o] if 0 <= o < 4 else "?%d" % o), flush=True)
    if rd(vals["Front Panel Mix"]):
        m = lib.snd_ctl_elem_value_get_integer(vals["Front Panel Mix"], 0)
        if m != last["Mix"]:
            last["Mix"] = m
            print("[%7.2f] MIX %s" % (time.monotonic() - t0, "ON" if m else "off"), flush=True)
    if rd(vals["Front Panel Dim"]):
        d = lib.snd_ctl_elem_value_get_integer(vals["Front Panel Dim"], 0)
        if d != last["Dim"]:
            last["Dim"] = d
            print("[%7.2f] DIM %s" % (time.monotonic() - t0, "ON" if d else "off"), flush=True)
    time.sleep(1.0 / HZ)
