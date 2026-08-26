#!/usr/bin/env python3
"""rms.py <file.wav> [channel] — RMS dBFS of one channel of a 24-bit wav."""
import sys, wave, math

def main():
    path = sys.argv[1]
    ch = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    w = wave.open(path, "rb")
    nch = w.getnchannels()
    sw = w.getsampwidth()
    n = w.getnframes()
    data = w.readframes(n)
    step = nch * sw
    if sw in (3, 4):
        # ALSA S24_LE: 24-bit sample in the low 3 bytes (of a 3- or 4-byte word)
        def val(i):
            b = data[i * step + ch * sw : i * step + (ch + 1) * sw]
            v = b[0] | (b[1] << 8) | (b[2] << 16)
            if v & 0x800000:
                v -= 1 << 24
            return v
    elif sw == 2:
        def val(i):
            b = data[i * step + ch * sw : i * step + (ch + 1) * sw]
            v = b[0] | (b[1] << 8)
            if v & 0x8000:
                v -= 1 << 16
            return v
    else:
        sys.exit("unsupported sample width %d" % sw)
    peak = 0.0
    acc = 0.0
    cnt = 0
    for i in range(n):
        v = val(i)
        acc += v * v
        cnt += 1
        av = abs(v)
        if av > peak:
            peak = av
    if cnt == 0:
        sys.exit("empty file")
    rms = math.sqrt(acc / cnt)
    full = 1 << 23
    rms_db = 20 * math.log10(rms / full) if rms > 0 else -200.0
    peak_db = 20 * math.log10(peak / full) if peak > 0 else -200.0
    print("ch%d rms=%.2f dBFS peak=%.2f dBFS samples=%d" % (ch, rms_db, peak_db, cnt))

if __name__ == "__main__":
    main()
