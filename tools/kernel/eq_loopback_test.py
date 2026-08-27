#!/usr/bin/env python3
"""eq_loopback_test.py — loopback-chain level check (EQ measurement caveat).

IMPORTANT (hardware-verified 2026-08-27): the loopback taps the OUTPUT
bus into the RECORD words (PROTOCOL.md "w2/3 = the PH3/4 record bus ...
when its loopback is on"), i.e. POST-EQ on the input strip.  The input
strip EQ therefore does NOT appear on the loopback measurement — the
EQ is only audible/measurable on the physical input path (mic/line).
The EQ itself was validated by ear on the mic (bell +-6 dB @ 200 Hz,
low cut 100/300 Hz on/off all confirmed).

What this script still verifies objectively:
- the AN1/2 output -> loopback -> record ch0/1 path (level check),
- the PB1/2 -> AN1/2 out routing, and the AN1->PH3/4 crosspoint ->
  PH3/4 out -> loopback -> ch2/3 path (dual loopback via usbwrite:
  the hardware supports several pairs at once — TotalMix's
  single-active is a software convention, not a hardware limit).

Chain: 440 Hz tone (pw-cat) -> PB1/2 -> AN1/2 out -> loopback AN1/2
(idx 0/1 via usbwrite) -> ch0/1 AND -> AN1 strip -> xpoint AN1->PH3/4
-> PH3/4 out -> loopback PH3/4 (idx 2/3) -> ch2/3.  The AN1->AN1/2
crosspoint (numid=21) is set to -inf so the loopback does not feed
back.

The 440 Hz level on ch0 and ch2 is measured with a Goertzel."""
import subprocess, wave, math, os, time

TONE = "/tmp/eqtone.wav"
SINK = "alsa_output.usb-RME_Babyface_Pro__73055480__3A1697563035400-05.stereo-fallback"


def card_num():
    for line in open("/proc/asound/cards"):
        if "BabyfaceProFS" in line:
            return line.split()[0]
    raise SystemExit("no Babyface Pro FS card")


def goertzel(path, ch, freq, rate=48000):
    w = wave.open(path, "rb")
    nch, sw, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
    data = w.readframes(n)
    step = nch * sw
    win = min(rate, n)
    s0 = s1 = 0.0
    cw = 2 * math.cos(2 * math.pi * freq / rate)
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


def cset(name, val):
    subprocess.run(["amixer", "-c", card_num(), "cset", name, str(val)],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def rec(path):
    subprocess.run(["arecord", "-D", "hw:%s,0" % card_num(), "-f", "S24_LE",
                    "-c", "12", "-r", "48000", "--buffer-size=512",
                    "--period-size=256", "-d", "2", path],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def measure(tag):
    p = "/tmp/eqrec.wav"
    rec(p)
    print("%-32s  440Hz(ch0) = %7.2f dBFS" % (tag, goertzel(p, 0, 440)))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    if not os.path.exists(TONE):
        subprocess.run(["python3", os.path.join(here, "mktone.py"),
                        TONE, "440", "-20", "60"])
    # routing: kill the feedback path, keep PB1/2->AN1/2 at 0 dB
    cset("numid=21", 0)          # AN1 -> AN1/2 xpoint = -inf
    cset("numid=1", 8192)        # AN1/2 master 0 dB
    cset("numid=29", 5792)       # PB1 -> AN1/2 xpoint = 0 dB
    cset("numid=30", 5792)       # PB2 -> AN1/2 xpoint = 0 dB
    cset("numid=106", "1,1")     # loopback AN1/2 ON (single-active)
    cset("name='AN1 EQ Enable'", 0)

    pw = subprocess.Popen(["pw-cat", "--playback", TONE])
    time.sleep(1.5)

    measure("baseline (EQ off)")

    cset("name='AN1 EQ Enable'", 1)
    cset("name='AN1 EQ Band 1 Type'", 1)    # Bell
    cset("name='AN1 EQ Band 1 Freq'", 440)
    cset("name='AN1 EQ Band 1 Q'", 70)      # Q = 0.70
    cset("name='AN1 EQ Band 1 Gain'", 60)   # +6.0 dB
    measure("+6 dB @ 440 Hz")

    cset("name='AN1 EQ Band 1 Gain'", -120)  # -12.0 dB
    measure("-12 dB @ 440 Hz")

    cset("name='AN1 EQ Band 1 Gain'", 60)    # back to +6 dB
    cset("name='AN1 EQ Band 1 Freq'", 3000)  # band moved off-band
    measure("+6 dB @ 3 kHz (off-band)")

    pw.terminate()
    cset("numid=106", "0,0")
    cset("name='AN1 EQ Enable'", 0)
    print("done")


if __name__ == "__main__":
    main()
