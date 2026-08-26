#!/usr/bin/env python3
"""analog_staging.py — DECISIVE test for the analog-input record staging.

Requires a cable from the AN1/2 LINE OUTPUT (back of the unit) to the AN1
INPUT (combo XLR/TRS).  Plays a -20 dBFS 440 Hz tone on PB1 -> AN1/2 at
master 0 dB and records the AN1 input (preamp gain 0, 48V off).

- If AN1 records ~ -20 dBFS : the ANALOG input path is unity-gain, as
  expected (the loopback is already proven 1:1; this closes the analog
  path with a physical signal instead of the noise-floor LSB test).
- Any large offset would be a record-path issue — but the loopback is
  verified 1:1 clean (2026-08-26), so this is a confirmation test.

Run:  python3 analog_staging.py   (then plug/unplug as instructed)
"""
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

def amixer(args):
    subprocess.run(["amixer", "-c", "3", "cset"] + args,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def rec(path, d=2):
    subprocess.run(["arecord", "-D", "hw:3,0", "-f", "S24_LE", "-c", "12",
                    "-r", "48000", "-d", str(d), path],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

if not os.path.exists(TONE) or os.path.getsize(TONE) < 5e6:
    subprocess.run(["python3", "tools/kernel/mktone.py", TONE, "440", "-20", "60"])

# setup: AN1/2 master 0 dB, mic gain 0, 48V OFF, input = line
amixer(["numid=1", "8192"])
amixer(["name='Mic 1 Capture Volume'", "0"])
amixer(["name='Phantom Power Mic 1'", "off"])

pw = subprocess.Popen(["pw-cat", "--playback", "--target", SINK, TONE],
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(2)
try:
    rec("/tmp/stage_base.wav")
    print("AN1 record (cable out->in, gain 0, master 0dB): %.1f dBFS  (tone -20)" %
          level440("/tmp/stage_base.wav", 0))
    print("AN2 (should be silent): %.1f dBFS" % level440("/tmp/stage_base.wav", 2))
finally:
    pw.terminate()
    pw.wait()
