#!/usr/bin/env python3
"""xp_width.py — does the Width control move the Phones loopback / words 12/13
tap?  (The tap should be a fixed playback tap; the Phones loopback has a
session-dependent +/-3 dB factor vs the AN1/2 law.)"""
import subprocess, wave, math, os, time

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
                    "-r", "48000", "-d", "2", path],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def amixer(args):
    subprocess.run(["amixer", "-c", "3", "cset"] + args,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

amixer(["numid=107", "1"])   # Phones loopback ON
amixer(["numid=3", "8192"])  # Phones master 0 dB
pw = subprocess.Popen(["pw-cat", "--playback", "--target", SINK, TONE],
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(2)
try:
    print("%8s %9s %9s" % ("width", "ch2", "ch10"))
    for v in (-100, -50, 0, 50, 100):
        amixer(["numid=116", str(v)])
        path = "/tmp/wd_%d.wav" % (v + 100)
        rec(path)
        print("%8d %9.1f %9.1f" % (v, level440(path, 2), level440(path, 10)))
finally:
    pw.terminate()
    pw.wait()
    amixer(["numid=116", "0"])   # restore width neutral
    amixer(["numid=107", "0"])   # loopback OFF
