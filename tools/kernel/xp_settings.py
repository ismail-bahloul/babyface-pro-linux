#!/usr/bin/env python3
"""xp_settings.py — brute-force the 0x05CF keepalive settings-word bits (bit 0 =
clock Internal preserved) and watch the loopback tap level.  If some bit flips
the tap to unity (1:1), that's the Windows session-state difference."""
import subprocess, wave, math, os, time, sys

TONE = "/tmp/tone440b.wav"
SINK = "alsa_output.usb-RME_Babyface_Pro__SERIAL-05.stereo-fallback"

def level440(path, ch):
    w = wave.open(path, "rb")
    nch, sw, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
    data = w.readframes(n)
    step = nch * sw
    win = min(rate, n)
    s0 = s1 = 0.0
    cw = 2 * math.cos(2 * math.pi * 440 / rate)
    for i in range(win):
        b = data[i * step + ch * sw: i * step + (ch + 1) * sw]
        v = b[0] | (b[1] << 8) | (b[2] << 16)
        if v & 0x800000:
            v -= 1 << 24
        t = v + cw * s0 - s1
        s1, s0 = s0, t
    p = s1 * s1 + s0 * s0 - cw * s1 * s0
    amp = math.sqrt(p * 2) / win
    return 20 * math.log10(amp / (1 << 23)) if amp > 0 else -200

def rec(path):
    subprocess.run(["arecord", "-D", "hw:3,0", "-f", "S24_LE", "-c", "12",
                    "-r", "48000", "-d", "1", path],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def amixer(args):
    subprocess.run(["amixer", "-c", "3", "cset"] + args,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def vwrite(req, val, idx):
    subprocess.run(["./tools/kernel/usbwrite", str(req), str(val), str(idx)],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

amixer(["numid=106", "1"])   # AN1/2 loopback ON
amixer(["numid=1", "8192"])  # AN1/2 master 0 dB
pw = subprocess.Popen(["pw-cat", "--playback", "--target", SINK, TONE],
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(2)
try:
    print("%8s %8s %8s" % ("settings", "ch0", "ch10"))
    base = [0x0001, 0x0003, 0x0005, 0x0009, 0x0011, 0x0021, 0x0041, 0x0081,
            0x0101, 0x0201, 0x0401, 0x0801, 0x1001, 0x2001, 0x4001, 0x8001,
            0x0000, 0x01ff, 0xffff]
    for v in base:
        vwrite(0x10, v, 0x05cf)
        time.sleep(0.2)
        path = "/tmp/st_%04x.wav" % v
        rec(path)
        print("%#8x %8.1f %8.1f" % (v, level440(path, 0), level440(path, 10)))
finally:
    vwrite(0x10, 0x0001, 0x05cf)   # restore clock Internal
    pw.terminate()
    pw.wait()
    amixer(["numid=106", "0"])
