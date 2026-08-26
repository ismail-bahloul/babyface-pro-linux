#!/usr/bin/env python3
"""lb_sweep.py — loopback record level vs the output master.

Plays a 440 Hz tone via pw-cat on the PipeWire sink, enables the
loopback of one output (AN1/2 or PH3/4), sweeps that output's master,
and measures the 440 Hz component on the record words (ch 2·out).

2026-08-25 finding this tool documents: the loopback record of the
AN1/2 bus follows the AN1/2 master with the right +6 dB per register
doubling but sits ~30 dB BELOW the 20·log10(reg/0x2000) law (a -20
dBFS tone at master 0x2000 records at ~-49.8 dBFS); the PH3/4 record
~-46.7 dBFS at the same 0 dB master.  Gain-staging to pin with the
Windows perfect-calibration capture (Capture 1).

RESOLVED 2026-08-26: that "law" was an ARTIFACT of a broken test tone
(mktone.py wrote a non-standard 24-bit-in-4-bytes wav -> the chain ran
~48 dB low -> the record quantizer turned the ~-68 dBFS signal into a
coarse square; the Goertzel measured the square's fundamental).  With a
proper S16 tone the loopback records 1:1 clean (-20 dBFS tone at
master 0x2000 -> -20.0 dBFS, -60 -> -60.2).  No staging is needed; the
kernel x32 stage was reverted.  NOTE: the master numids here assume the
post-rename control order (AN1/2 = numid 1, PH3/4 = numid 3).

Expected output now:

== an12 ==  8192 0x2000 -20.0 (a -20 dBFS tone, 1:1)
"""
import subprocess, wave, math, os, time

TONE = "/tmp/tone440b.wav"
SINK = "alsa_output.usb-RME_Babyface_Pro__SERIAL-05.stereo-fallback"

def goertzel_power(s, freq, rate):
    w0 = 2 * math.pi * freq / rate
    cw = 2 * math.cos(w0)
    s0 = s1 = 0.0
    for x in s:
        t = x + cw * s0 - s1
        s1, s0 = s0, t
    return s1 * s1 + s0 * s0 - cw * s1 * s0

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

def amixer(numid, val):
    subprocess.run(["amixer", "-c", "3", "cset", "numid=%d" % numid, str(val)],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def rec(path):
    subprocess.run(["arecord", "-D", "hw:3,0", "-f", "S24_LE", "-c", "12",
                    "-r", "48000", "-d", "2", path],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def sweep(tag, lb_numid, master_numid, ch, recch):
    """lb_numid: loopback control; master_numid: the output's master;
    ch: the record channel the bus lands on; recch: monitor channel
    (the tone should sit there at ~-47 dBFS as a sanity check)."""
    print("== %s (numid=%d): record ch%d = the bus ==" % (tag, lb_numid, ch))
    print("%8s %7s %8s %10s" % ("alsaval", "reg", "440Hz", "mon440"))
    amixer(lb_numid, 1)
    for v in (0, 512, 1024, 2048, 4096, 8192, 12288, 16384):
        amixer(master_numid, v)
        path = "/tmp/lb_%s_%d.wav" % (tag, v)
        rec(path)
        reg = v * 0x4000 // 16384
        print("%8d %#7x %8.1f %10.1f" %
              (v, reg, level440(path, ch), level440(path, recch)))
    amixer(lb_numid, 0)

def main():
    if not os.path.exists(TONE) or os.path.getsize(TONE) < 5e6:
        subprocess.run(["python3", "tools/kernel/mktone.py", TONE, "440",
                        "-20", "60"])
    pw = subprocess.Popen(["pw-cat", "--playback", "--target", SINK, TONE],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)
    try:
        # AN1/2: master numid=1 (out0), loopback numid=106, record ch0.
        # (numids corrected 2026-08-26 after the per-output master rename:
        #  pre-rename 'Master Playback Volume' numid=3 was AN1/2; now
        #  'AN1/2 Playback Volume'=1, 'PH3/4 Playback Volume'=3.)
        sweep("an12", 106, 1, 0, 10)
        # PH3/4: master numid=3 (out1), loopback numid=107, record ch2.
        sweep("ph34", 107, 3, 2, 10)
    finally:
        pw.terminate()
        pw.wait()
        amixer(1, 8192)
        amixer(3, 8192)

if __name__ == "__main__":
    main()
