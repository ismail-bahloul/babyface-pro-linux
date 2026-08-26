#!/usr/bin/env python3
"""mktone.py <out.wav> <freq> <dbFS> <sec> — stereo 48k tone.

Writes a STANDARD S16 wav (2 bytes/sample — the most portable format
for sndfile/PipeWire/aplay; corrected 2026-08-26: the old 24-bit-in-4-
bytes container was non-standard and sndfile read it as 32-bit, so the
whole playback chain ran ~48 dB low, which corrupted every loopback
level test that used these tones)."""
import sys, math, struct, wave

def main():
    out, freq, db, sec = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4])
    rate = 48000
    amp = 32768 * 10 ** (db / 20.0)
    n = int(rate * sec)
    w = wave.open(out, "wb")
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(rate)
    frames = bytearray()
    for i in range(n):
        v = int(amp * math.sin(2 * math.pi * freq * i / rate))
        if v > 32767:
            v = 32767
        if v < -32768:
            v = -32768
        frames += struct.pack("<h", v)
        frames += struct.pack("<h", v)
    w.writeframes(bytes(frames))
    w.close()
    print("wrote %s: %.1f Hz at %.1f dBFS, %d s (S16)" % (out, freq, db, sec))

if __name__ == "__main__":
    main()
