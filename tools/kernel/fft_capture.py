#!/usr/bin/env python3
"""fft_capture.py — FFT of a capture channel to find the real components."""
import wave, math, sys

w = wave.open(sys.argv[1], 'rb')
nch, sw, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
data = w.readframes(n)
ch = int(sys.argv[2]) if len(sys.argv) > 2 else 0
vals = []
for i in range(n):
    b = data[i * nch * sw + ch * sw: i * nch * sw + (ch + 1) * sw]
    v = b[0] | (b[1] << 8) | (b[2] << 16)
    if v & 0x800000:
        v -= 1 << 24
    vals.append(v)

# FFT (simple DFT at candidate bins)
N = 32768
x = vals[:N]
bins = {}
for freq in [440, 565, 880, 1130, 1320, 1695, 2200, 2260, 2640, 3390, 4520, 4400,
             3960, 3080, 1740, 1180, 700, 90, 280, 620, 130]:
    k = freq * N / rate
    if k < 1 or k > N // 2:
        continue
    sr = si = 0.0
    for i, v in enumerate(x):
        a = 2 * math.pi * freq * i / rate
        sr += v * math.cos(a)
        si += v * math.sin(a)
    amp = 2 * math.sqrt(sr * sr + si * si) / N
    if amp > 0:
        bins[freq] = 20 * math.log10(amp / (1 << 23))
print("440-bin amplitude: %.1f dBFS" % bins.get(440, -200))
for f in sorted(bins, key=lambda x: -bins[x])[:8]:
    print("  %5d Hz: %7.1f dBFS" % (f, bins[f]))
