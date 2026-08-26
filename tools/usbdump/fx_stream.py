#!/usr/bin/env python3
"""fx_stream.py — decode the audio streams (ep 0x01 OUT / 0x82 IN) from a
pcap and show per-channel levels (14ch x 32-bit frames, 24-bit samples).

The 2 invisible ASIO FX channels (manual 21.6) should show:
  - IN stream ch12/13 (FX Send, device->host): the DRY pre-reverb signal
  - OUT stream ch12/13 (FX Out, host->device): the WET reverb return
"""
import struct, sys
sys.path.insert(0, '.')
from eq_extract import read_records

CH = 14
def audio_urb(path, ep):
    """Yield (t, payload) for large audio URBs on endpoint `ep`."""
    out = []
    for ts_sec, ts_usec, fr in read_records(path):
        if len(fr) < 27:
            continue
        hlen = struct.unpack_from("<H", fr, 0)[0]
        endpoint = fr[0x15]
        plen = struct.unpack_from("<I", fr, 0x17)[0]
        if endpoint == ep and plen >= 1000 and hlen + plen <= len(fr):
            t = ts_sec + ts_usec / 1e6
            out.append((t, fr[hlen:hlen + plen]))
    return out

def channel_rms(payload, ch):
    """RMS of one channel across a URB's frames (24-bit in bytes 1-3)."""
    n = len(payload) // 56
    s = 0.0
    cnt = 0
    for f in range(n):
        base = f * 56 + ch * 4
        word = payload[base] | (payload[base+1] << 8) | (payload[base+2] << 16) | (payload[base+3] << 24)
        v = (word >> 8) / 8388608.0  # 24-bit signed-ish
        s += v * v
        cnt += 1
    return (s / cnt) ** 0.5 if cnt else 0.0

path = sys.argv[1] if len(sys.argv) > 1 else "cap_fx_live.pcap"
for name, ep in (("OUT host->dev", 0x01), ("IN  dev->host", 0x82)):
    urbs = audio_urb(path, ep)
    print(f"== {name} ep=0x{ep:02X}: {len(urbs)} audio URBs")
    if not urbs:
        continue
    # per-channel RMS across the first 20 URBs
    acc = [0.0] * CH
    nacc = 0
    for t, pl in urbs[:20]:
        for c in range(CH):
            acc[c] += channel_rms(pl, c) ** 2
        nacc += 1
    levels = [(c, (acc[c] / nacc) ** 0.5) for c in range(CH)]
    print("  ch | level (0..1 FS)")
    for c, v in levels:
        bar = "#" * int(min(v * 60, 60))
        tag = "  <<< FX?" if c >= 12 else ""
        print(f"  {c:2d} | {v:6.4f} {bar}{tag}")
