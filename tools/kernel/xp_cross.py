#!/usr/bin/env python3
"""Crosspoint-vs-loopback test: with the AN1/2 loopback ON and a 440 Hz tone
playing, sweep the PB1->AN1/2 crosspoint control and measure the record ch0."""
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

# Phones loopback ON (numid 107), Phones master 0 dB (numid 3)
amixer(["numid=107", "1"])
amixer(["numid=3", "8192"])

pw = subprocess.Popen(["pw-cat", "--playback", "--target", SINK, TONE],
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(2)
try:
    print("%10s %7s %9s %9s" % ("ctlval", "reg", "rec_ch2", "tap_ch10"))
    for v in (0x1000, 0x16a0, 0x2000, 0x2d41):
        amixer(["numid=22", str(v)])   # PB1 -> out1 (Phones) crosspoint
        path = "/tmp/xp_%04x.wav" % v
        rec(path)
        print("%10d %#7x %9.1f %9.1f" % (v, v, level440(path, 2), level440(path, 10)))
finally:
    pw.terminate()
    pw.wait()
    amixer(["numid=22", "5792"])   # restore default crosspoint
    amixer(["numid=107", "0"])     # loopback OFF
