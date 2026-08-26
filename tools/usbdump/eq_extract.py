#!/usr/bin/env python3
"""eq_extract.py — extract the SETTLED EQ coefficient states from the
USBPcap captures (cap_eq3-7.pcap).

The EQ coefficients are 64-byte bulk OUT blocks on ep 0x0A. TotalMix
ramps the coefficients (DSP interpolation) after each change, so each
user action produces a BURST of blocks that settle during the pause —
the LAST block of each >1s-gap group is the settled state.

Usage: python3 eq_extract.py [cap_eqN.pcap ...] [--raw]
Prints each settled state as the 16-byte header + the 5 signed Q1.31
coefficients (b0 b1 b2 a1 a2) per active slot, with the capture time.
"""
import struct
import sys

USBPcap_LT = 249  # LINKTYPE_USBPCAP (also accept 152 = old tag)


def read_records(path):
    """Yield (ts_sec, ts_usec, frame_bytes) for each pcap record."""
    with open(path, "rb") as f:
        gh = f.read(24)
        if len(gh) < 24:
            return
        magic, ver_maj, ver_min, _, _, _, lt = struct.unpack("<IHHiIII", gh)
        if lt not in (249, 152):
            print(f"  ! {path}: linktype {lt} != USBPcap({USBPcap_LT})", file=sys.stderr)
            return
        while True:
            rh = f.read(16)
            if len(rh) < 16:
                return
            ts_sec, ts_usec, incl, orig = struct.unpack("<IIII", rh)
            data = f.read(incl)
            if len(data) < incl:
                return
            yield ts_sec, ts_usec, data


def eq_blocks(path):
    """Yield (t, payload) for every 64-byte ep 0x0A OUT block."""
    out = []
    for ts_sec, ts_usec, fr in read_records(path):
        if len(fr) < 27:
            continue
        hlen = struct.unpack_from("<H", fr, 0)[0]  # USBPcap pseudoheader length
        if hlen + 64 > len(fr):
            continue
        # Empirical offsets (cross-checked against known frames):
        #   endpoint @ 0x15, transfer type @ 0x16, length (u32 LE) @ 0x17
        endpoint = fr[0x15]
        xfer = fr[0x16]
        plen = struct.unpack_from("<I", fr, 0x17)[0]
        if endpoint == 0x0A and plen == 64:
            payload = fr[hlen:hlen + 64]
            t = ts_sec + ts_usec / 1e6
            out.append((t, payload))
    return out


def signed32(w):
    return w - (1 << 32) if w & (1 << 31) else w


def q31(v):
    return v / (1 << 31)


def dump_state(t, payload):
    hdr = payload[0:16]
    print(f"  t={t:10.3f}  hdr={hdr.hex(' ')}")
    # 3 band slots: 4 coeffs each @ 0x04/0x14/0x24 + a 5th @ 0x34
    for slot, off in enumerate((0x04, 0x14, 0x24)):
        words = [struct.unpack_from("<i", payload, off + 4 * k)[0] for k in range(4)]
        if all(w == 0 for w in words):
            continue
        a5 = struct.unpack_from("<i", payload, 0x34)[0]
        qs = [q31(w) for w in words]
        print(
            f"      slot{slot + 1} @0x{off:02X}: "
            + " ".join(f"{w:08X}({q:+.6f})" for w, q in zip(words, qs))
            + f"    +0x34={a5:08X}({q31(a5):+.6f})"
        )


def main():
    raw = "--raw" in sys.argv
    paths = [a for a in sys.argv[1:] if a != "--raw"]
    for path in paths:
        print(f"== {path} ==")
        blocks = eq_blocks(path)
        print(f"  ep 0x0A 64-B blocks: {len(blocks)}")
        if not blocks:
            continue
        # group by >1 s gaps
        groups = []
        cur = [blocks[0]]
        for b in blocks[1:]:
            if b[0] - cur[-1][0] > 1.0:
                groups.append(cur)
                cur = []
            cur.append(b)
        groups.append(cur)
        print(f"  settled groups: {len(groups)}")
        for g, grp in enumerate(groups, 1):
            t, payload = grp[-1]
            print(f"  --- group {g}: {len(grp)} blocks, last t={t:.3f} ---")
            dump_state(t, payload)
            if raw:
                print(f"      raw: {payload.hex()}")


if __name__ == "__main__":
    main()
