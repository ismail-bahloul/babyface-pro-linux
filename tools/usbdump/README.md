# usbdump — USB RE toolkit (Windows)

Dumps the device descriptor and the open pipes (endpoints) of any USB
device straight from the host controller — no vendor driver involved.

Written for reverse-engineering the **RME Babyface Pro FS** (VID `2A39`,
PID `3FC0`), but works for any VID/PID.

## Tools in this folder

| Tool | Purpose |
|---|---|
| `usbdump.exe` | Dump device descriptor + endpoints of a USB device |
| `ParsePcap.exe` | Parse USBPcap `.pcap` files, dump the URB traffic |
| `capture.ps1` | Capture the Babyface root hub with USBPcap |
| `build.ps1` | Compile the two C# tools with the .NET Framework compiler |

## Build & run (Windows)

```powershell
.\build.ps1                          # compiles usbdump.exe + ParsePcap.exe
.\usbdump.exe 2a39 3fc0             # dump the Babyface
```

> `csc.exe` is the .NET Framework compiler: `C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe`

## What it does

1. Enumerates USB hubs (SetupAPI device interfaces).
2. For each port, reads the node connection info → finds the target
   device by VID/PID.
3. Prints the device descriptor (bcdUSB, class, VID/PID, number of
   configurations).
4. Prints every open pipe (endpoint) of the current configuration.

## Notes on the Windows 11 24H2 USB stack

This stack (driver `USBHUB3`) does **not** answer to the classic
`usbioctl.h` IOCTL numbers. The working values (found by live IOCTL
scanning) are:

| Function | Value      | Purpose                                    |
|----------|------------|--------------------------------------------|
| 258      | `0x220408` | GET_NODE_INFORMATION (hub descriptor)      |
| 274      | `0x220448` | GET_NODE_CONNECTION_INFORMATION (EX2)      |
| —        | `0x220458` | returns the port's device name (UTF-16)    |

The replies of `GET_NODE_INFORMATION` are prefixed with a 4-byte zero
header (hub descriptor starts at offset 4).

**Limitation**: this stack does not expose
`IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION` in a usable form, so the
full configuration descriptor (USB Audio Control topology, interface
layout) cannot be read from Windows 11 24H2 this way. Use Linux for
that — `lsusb -v` shows the complete configuration descriptor.

---

# RE notes: RME Babyface Pro FS (PID 3FC0)

## Device identity (dumped 2026-08-21)

```
bcdUSB         : 02.00
device class   : 0xFF (Vendor Specific)   ← proprietary mode
sub/proto      : 0x00 / 0x00
vid:pid        : 2A39:3FC0                (RME)
bcdDevice      : 00.01
configurations : 1
```

Key finding: with the RME Windows driver (`firefaceu`, loaded as
`Service: firefaceu`) the device enumerates as a **vendor-specific
device with a single configuration** — it is *not* class-compliant in
this state. This is the "proprietary mode" TotalMix talks to.

## The two personalities + the mode toggle (hardware-verified 2026-08-25)

| mode | VID:PID | driver on Linux |
|---|---|---|
| proprietary (TotalMix/firefaceu, our snd-usb-babyface-pro) | `2a39:3fc0` | snd-usb-babyface-pro (vendor-specific iface 5, interrupt endpoints) |
| class-compliant (CC) | `2a39:3fb0` | snd-usb-audio (UAC2: S24_3LE, standard mixer) |

The toggle is **physical, on the unit**: unplug USB, hold **SELECT +
DIM**, plug the USB while holding, release after ~2 s.  Verified live
both directions on Linux: CC → snd-usb-audio plays clean (48 kHz
S24_3LE, RUNNING); back to proprietary → snd-usb-audio correctly
ignores the vendor-specific device and our module rebinds (card
"Babyface Pro FS"), full-duplex 0 xrun after.  The device's mixer
registers survive the mode round-trip (48V/gain/master reappeared
via the usual alsactl/asound.state restore on card-add).

## Endpoints of the proprietary mode

```
pipe 01: ep 0x03 OUT Isochronous(async)  ← audio OUT
pipe 02: ep 0x04 IN  Isochronous(async)  ← audio IN
pipe 03: ep 0x05 IN  Bulk (512)          ← data
pipe 04: ep 0x0A OUT Bulk (512)          ← data
pipe 05: ep 0x07 OUT Bulk (512)          ← data
pipe 06: ep 0x06 IN  Bulk (512)          ← data
pipe 07: ep 0x08 IN  Bulk (512)          ← data
pipe 08: ep 0x09 OUT Bulk (512)          ← data
pipe 09: ep 0x0B IN  Bulk (512)          ← data
pipe 10: ep 0x01 OUT Interrupt (448)     ← CONTROL (TotalMix commands)
pipe 11: ep 0x02 IN  Interrupt (448)     ← CONTROL (status/state)
```

The interrupt endpoints are where the proprietary control protocol
lives. Capturing them (USBPcap + Wireshark) while TotalMix moves a
fader reveals the command format.

> Note: the capture-based protocol work (`PROTOCOL.md`) revealed that
> during actual streaming the audio runs on **isochronous** endpoints
> (ep 0x01 OUT / ep 0x82 IN on **interface 5**, alt-setting 1, 448-B
> packets) — the endpoint numbers above are the ones the Windows driver
> enumerates via the IOCTL pipe list, which mislabels the types and
> numbers (it even lists the interface-0 interrupt pair as ISO). The
> device's own configuration descriptor, captured byte-for-byte in
> `cap_coldplug.pcap`, is authoritative: interface 5 `0x01`/`0x82` =
> isochronous (bmAttributes 0x03), interface 0 `0x03`/`0x84` = interrupt
> (bmAttributes 0x01). Trust the capture, not the pipe list or `lsusb`
> alone.

## Driver state in the registry

With `firefaceu` loaded, the driver caches device state in:

```
HKLM\SYSTEM\CurrentControlSet\Enum\USB\VID_2A39&PID_3FC0\<serial>\Device Parameters
```

Observed values (good topology hints):

| Value          | Meaning                          | Observed |
|----------------|----------------------------------|----------|
| Frequency      | current sample rate              | 48000    |
| DDS frequency 0-3 | supported rates               | 32000/44100/48000/50000 |
| Mic 0-3 Gain   | preamp gains (dB-ish, 32 = ~?)   | 32/0/0/0 |
| Mic 0-3 Power  | 48V phantom per preamp           | 1/0/0/0  |
| Mic 0-3 Pad    | pad per preamp                   | 0/0/0/0  |
| Instr 0-1      | instrument inputs                | 0/0      |
| Pad 0-1        | pads on instrument/line inputs   | 1/1      |
| Instr Gain 0-1 | instrument gains                 | 0/0      |
| Sync Ref       | clock reference                  | 8        |
| ASIO Latency   | ASIO buffer setting              | 3        |
| ADC Gain / DAC Gain | trim                     | 2/65535  |
| Phones Gain    | headphone amp gain               | 1        |

This confirms the channel topology (4 mic preamps, 2 instrument inputs)
and gives the parameter names the RME driver uses.

## What this means for TuxMix

The Babyface Pro FS is *dual-mode*:

- **Proprietary mode** (what Windows + `firefaceu`/TotalMix use): the
  state dumped above. Reversing the control protocol requires USB
  traffic capture or driver disassembly — doable, but a large project
  of its own.
- **Class-compliant mode** (what Linux uses via `snd-usb-audio`): the
  DSP mixer is exposed as standard USB Audio Class 2.0 mixer/feature
  units, which ALSA surfaces as plain mixer controls. This is exactly
  what `tuxmix-core` already drives (`babyface.rs` + `DeviceProfile`).

So for TuxMix the proprietary protocol is *not required* — the
class-compliant control surface (ALSA) covers the same hardware mixer.
The proprietary protocol matters only if you want to bit-replicate
TotalMix's extra features that are *not* exposed over the class-compliant
interface (e.g. TotalMix-only settings).

## Next steps (if you still want to RE the proprietary protocol)

1. Install [USBPcap](https://desowin.org/usbpcap/) and capture the
   traffic on the Babyface's bus while TotalMix moves faders / toggles
   48V / changes routing.
2. Focus on the interrupt OUT endpoint (ep 0x01) — that is the command
   channel; the interrupt IN (ep 0x02) carries state responses.
3. Correlate the observed registers with the registry `Device
   Parameters` names above.

See `PROTOCOL.md` for the protocol findings so far (volume/mute/pan/48V
commands, ISO audio stream format, VU-meter conclusion).

---

## Capturing the proprietary protocol (USBPcap)

USBPcap is already installed on this machine (service `USBPcap`, set to
`auto`) and the NonStandardHWIDs key is initialized, so USB 3.0 capture
works. **A reboot is required once** so the filter driver attaches to the
xHCI controller.

After the reboot:

```powershell
# 1) reload the tools
cd D:\TuxMix\tools\usbdump
.\build.ps1

# 2) see which capture interfaces exist
.\capture.ps1 -List

# 3) capture the Babyface root hub for N seconds (TotalMix must be
#    running; move faders while it captures)
.\capture.ps1 -Device <N> -Seconds 60 -OutFile cap1.pcap

# 4) analyze: dump everything on the control endpoints
.\ParsePcap.exe cap1.pcap --dumpall
.\ParsePcap.exe cap1.pcap --ep 0x01      # commands TotalMix sent
.\ParsePcap.exe cap1.pcap --ep 0x02      # status / VU meter stream
```

The control protocol lives on the interrupt endpoints: `ep 0x01`
(OUT, command channel) and `ep 0x02` (IN, state/VU stream). Both carry
448-byte packets.

## ParsePcap options

```
ParsePcap.exe file.pcap [--stats] [--ep 0x01] [--dev N] [--ascii]
                        [--max N] [--isodump] [--isoframes N]
                        [--skipiso N] [--bulk85] [--series N] [--skip N]
```

| Option | Purpose |
|---|---|
| `--stats` | per-endpoint/type/function record counts |
| `--ep 0xNN` | only records matching this endpoint (e.g. `--ep 2` matches 0x02 and 0x82) |
| `--dev N` | only records for device address N (1 = Babyface, 2 = mouse) |
| `--ascii` | append ASCII column to hex dumps |
| `--max N` | stop after N printed records |
| `--isodump` | dump isochronous payloads (hex of first 1024 bytes) |
| `--isoframes N` | compact per-frame decode (14ch × 32-bit, 56-byte frames) |
| `--skipiso N` | skip the first N ISO data records (dive into the capture) |
| `--bulk85` | dump 480-byte status blocks |
| `--series N` | with `--bulk85`: compact time-series line per block |
| `--skip N` | with `--bulk85`: skip the first N blocks |

Note: `--dev` and `--ep` are AND-ed together. IRP-info records
(`type=IRPINFO`, USBPcap URB-lifecycle markers) are never printed.
