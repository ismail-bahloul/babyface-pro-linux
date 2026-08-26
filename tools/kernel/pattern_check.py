#!/usr/bin/env python3
"""pattern_check.py — record 3 s (loopback ON or OFF) and report whether ch0
shows the ~54/55-sample amplitude alternation."""
import wave, sys

path = sys.argv[1]
w = wave.open(path, 'rb')
nch, sw, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
data = w.readframes(n)

def ch(ch):
    out = []
    for i in range(n):
        b = data[i * nch * sw + ch * sw: i * nch * sw + (ch + 1) * sw]
        v = b[0] | (b[1] << 8) | (b[2] << 16)
        if v & 0x800000:
            v -= 1 << 24
        out.append(v)
    return out

for chn, name in [(0, 'ch0'), (2, 'ch2'), (10, 'ch10')]:
    c = ch(chn)
    # classify big vs small: |v| > 1e6 for the loopback (×32), else small
    if max(abs(v) for v in c[:20000]) < 1e5:
        print('%s: no big/small split (all < 1e5)' % name)
        continue
    big = [abs(v) > 1e6 for v in c[:20000]]
    runs = []
    cur = big[0]; cnt = 1
    for b in big[1:]:
        if b == cur:
            cnt += 1
        else:
            runs.append((cur, cnt))
            cur = b; cnt = 1
    runs.append((cur, cnt))
    from collections import Counter
    print('%s: run histogram %s' % (name, Counter(runs)))
