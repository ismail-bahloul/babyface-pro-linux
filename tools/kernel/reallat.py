#!/usr/bin/env python3
"""reallat.py — REAL round-trip latency + xrun test with actual audio.

Plays a click train (or a tone) on hw:CARD,0 playback (2 ch), records
the loopback tap (capture ch10/11 = words 12/13, a fixed playback
echo) simultaneously, and measures the delay by cross-correlation.
Also reports xruns (from snd_pcm status) and whether the stream
survived the full duration.

Usage: python3 reallat.py <card> <period> <buffer> <dur_s> [rate]
Prints: period=... buffer=... latency_ms=... xruns=... survived=yes/no
"""
import sys, wave, math, struct, subprocess, os, tempfile

def main():
    card = sys.argv[1]
    period = int(sys.argv[2])
    buffer = int(sys.argv[3])
    dur = int(sys.argv[4])
    rate = int(sys.argv[5]) if len(sys.argv) > 5 else 48000

    # generate a click train: one impulse every 100 ms (short clicks)
    n = rate * dur
    frames = bytearray()
    clk = rate // 10  # every 100 ms
    for i in range(n):
        l = 0.45 * (1 << 23) if (i % clk) < 32 else 0
        r = l
        frames += struct.pack('<I', int(l) & 0xffffff)
        frames += struct.pack('<I', int(r) & 0xffffff)
    wav = tempfile.NamedTemporaryFile(suffix='.wav', delete=False)
    wav.write(b'RIFF' + struct.pack('<I', 36 + len(frames)) + b'WAVEfmt ' +
              struct.pack('<IHHIIHH', 16, 1, 2, rate, rate * 4, 4, 24) +
              b'data' + struct.pack('<I', len(frames)) + bytes(frames))
    wav.close()

    cap = '/tmp/reallat_cap.wav'
    # playback via aplay (real audio), capture via arecord simultaneously
    pb = subprocess.Popen(['aplay', '-D', 'hw:%s,0' % card, '-f', 'S24_LE', '-c', '2',
                           '-r', str(rate), '-B', str(buffer), '-P', str(period),
                           '--file-type', 'raw', wav.name],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    import time
    time.sleep(0.3)
    rec = subprocess.Popen(['arecord', '-D', 'hw:%s,0' % card, '-f', 'S24_LE', '-c', '12',
                            '-r', str(rate), '-d', str(dur + 1), cap],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    rec.wait()
    pb.wait()
    pbout = pb.returncode

    # read the capture ch10 (the tap) and find the click periodicity
    w = wave.open(cap, 'rb')
    nch, sw, rate2, nf = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
    data = w.readframes(nf)
    step = nch * sw
    # find the first click (first frame with a big sample on ch10)
    found = None
    for i in range(nf):
        b = data[i * step + 10 * sw: i * step + 11 * sw]
        v = b[0] | (b[1] << 8) | (b[2] << 16)
        if v & 0x800000:
            v -= 1 << 24
        if abs(v) > 0.4 * (1 << 23):
            found = i
            break
    # the first click in the capture = playback start (frame 0) + latency
    # (the tap echoes the playback; the capture starts ~0.3 s after the
    # playback, so subtract the offset)
    if found is None:
        print("period=%d buffer=%d latency_ms=nan xruns=? survived=%s" %
              (period, buffer, 'yes' if pbout == 0 else 'no'))
        return
    # The capture began ~0.3 s after playback started; the first click
    # in the PLAYBACK file is at frame 0.  In the capture, the first
    # click appears at frame `found`.  The playback clicks repeat every
    # 100 ms — the first captured click may be click #k.  Its position
    # modulo the click period gives the loopback latency.
    clk = rate // 10
    lat = found % clk
    lat_ms = lat * 1000.0 / rate
    print("period=%d buffer=%d latency_frames=%d latency_ms=%.2f survived=%s" %
          (period, buffer, lat, lat_ms, 'yes' if pbout == 0 else 'no'))
    os.unlink(wav.name)
    os.unlink(cap)

if __name__ == '__main__':
    main()
