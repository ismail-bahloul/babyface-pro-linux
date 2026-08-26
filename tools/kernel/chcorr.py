#!/usr/bin/env python3
"""chcorr.py <file.wav> — cross-correlate the 12 capture channels to find
duplicates / delayed copies.  Prints, for each channel pair (a,b), the
lag in samples with the best correlation and the correlation coefficient
(|r| >= 0.5 only)."""
import sys, wave, math

def load(path, ch, n):
    w = wave.open(path, "rb")
    nch = w.getnchannels()
    sw = w.getsampwidth()
    data = w.readframes(n)
    step = nch * sw
    out = []
    for i in range(n):
        b = data[i * step + ch * sw : i * step + (ch + 1) * sw]
        v = b[0] | (b[1] << 8) | (b[2] << 16)
        if v & 0x800000:
            v -= 1 << 24
        out.append(v)
    return out

def corr(a, b, maxlag):
    n = len(a)
    am = sum(a) / n
    bm = sum(b) / n
    a = [x - am for x in a]
    b = [x - bm for x in b]
    aa = math.sqrt(sum(x * x for x in a))
    bb = math.sqrt(sum(x * x for x in b))
    best = []
    for lag in range(-maxlag, maxlag + 1):
        k = abs(lag)
        num = sum(a[i] * b[i + lag] for i in range(0, n - k))
        den = aa * bb
        r = num / den if den else 0
        best.append((abs(r), lag, r))
    best.sort(reverse=True)
    return best[0]

def main():
    path = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 48000  # default 1 s
    w = wave.open(path, "rb")
    nch = w.getnchannels()
    print("channels=%d rate=%d sampwidth=%d frames=%d" %
          (nch, w.getframerate(), w.getsampwidth(), w.getnframes()))
    chans = [load(path, c, n) for c in range(nch)]
    for a in range(nch):
        # baseline energy
        e = math.sqrt(sum(x * x for x in chans[a]) / n)
        db = 20 * math.log10(e / (1 << 23)) if e > 0 else -200
        print("ch%-2d rms=%7.2f dBFS" % (a, db))
    print("--- correlated pairs (|r|>=0.5) ---")
    for a in range(nch):
        for b in range(a + 1, nch):
            r, lag, rv = corr(chans[a], chans[b], 8)
            if r >= 0.5:
                print("ch%d <-> ch%d: r=%.3f lag=%+d" % (a, b, rv, lag))

if __name__ == "__main__":
    main()
