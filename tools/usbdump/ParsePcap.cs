// ParsePcap.cs — parse USBPcap .pcap files and dump the USB traffic.
//
// USBPcap wraps every URB in a record whose link type is 249
// (DLT_USBPCAP). The record payload starts with the USBPcap pseudo
// header (see Wireshark's packet-usb.c, dissect_usbpcap_buffer_packet_header):
//
//   UINT16 header_len;      @0
//   UINT64 irp_id;          @2
//   UINT32 usbd_status;     @10
//   UINT16 function;        @14  (URB function, e.g. 0x0009 BULK_OR_INTERRUPT,
//                                 0x0017 VENDOR_DEVICE)
//   UINT8  info;            @16  (bit0: 0=FDO->PDO (host->dev), 1=PDO->FDO)
//   UINT16 bus_id;          @17
//   UINT16 device_address;  @19
//   UINT8  endpoint;        @21  (bit 0x80 = IN)
//   UINT8  transfer_type;   @22  (0=CTRL 1=ISO 2=INTR 3=BULK 0xFE=IRP info)
//   UINT32 data_len;        @23
//   then, for control transfers: UINT8 control_stage @27 (0=SETUP 1=DATA
//     2=STATUS 3=COMPLETE) and the payload (setup packet first for SETUP)
//   otherwise the payload starts at @27.
//
// Build:
//   csc.exe /nologo /out:ParsePcap.exe ParsePcap.cs
// Usage:
//   ParsePcap.exe capture.pcap [--stats] [--ep 0x01] [--dev 1] [--ascii]
//                             [--max N] [--isodump]
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

class ParsePcap
{
    static int Main(string[] args)
    {
        if (args.Length == 0)
        {
            Console.WriteLine("Usage: ParsePcap.exe file.pcap [--stats] [--ep 0x01] [--dev N] [--ascii] [--max N] [--isodump]");
            return 1;
        }
        string file = args[0];
        int epFilter = -1;
        int devFilter = -1;
        bool ascii = false;
        bool stats = false;
        bool bulk85 = false;
        bool ctlout = false;
        bool isodump = false;
        bool isoscan = false;
        int isoFrames = 0;
        int seriesN = 0;
        int bulkSkip = 0;
        int isoSkip = 0;
        int maxPrint = 0;
        for (int i = 1; i < args.Length; i++)
        {
            if (args[i] == "--ep" && i + 1 < args.Length) epFilter = Convert.ToInt32(args[++i], 16);
            else if (args[i] == "--dev" && i + 1 < args.Length) devFilter = Convert.ToInt32(args[++i], 10);
            else if (args[i] == "--ascii") ascii = true;
            else if (args[i] == "--stats") stats = true;
            else if (args[i] == "--bulk85") bulk85 = true;
            else if (args[i] == "--ctlout") ctlout = true;
            else if (args[i] == "--isoscan") isoscan = true;
            else if (args[i] == "--series" && i + 1 < args.Length) seriesN = Convert.ToInt32(args[++i], 10);
            else if (args[i] == "--isodump") isodump = true;
            else if (args[i] == "--isoframes" && i + 1 < args.Length) isoFrames = Convert.ToInt32(args[++i], 10);
            else if (args[i] == "--skipiso" && i + 1 < args.Length) isoSkip = Convert.ToInt32(args[++i], 10);
            else if (args[i] == "--max" && i + 1 < args.Length) maxPrint = Convert.ToInt32(args[++i], 10);
            else if (args[i] == "--skip" && i + 1 < args.Length) bulkSkip = Convert.ToInt32(args[++i], 10);
        }

        byte[] b = File.ReadAllBytes(file);
        int pos = 0;
        if (b.Length < 24) { Console.WriteLine("Too small for a pcap"); return 1; }
        uint magic = ReadU32(b, 0, true);
        bool little = magic == 0xA1B2C3D4;
        if (!little && magic != 0xD4C3B2A1) { Console.WriteLine("Not a pcap (magic 0x" + magic.ToString("X8") + ")"); return 1; }
        uint linktype = ReadU32(b, 20, little);
        Console.WriteLine("pcap linktype=" + linktype + (linktype == 249 ? " (USBPcap)" : ""));
        pos = 24;

        int count = 0;
        int bulkCount = 0;
        int printed = 0;
        Dictionary<string, int> statTable = new Dictionary<string, int>();
        while (pos + 16 <= b.Length)
        {
            uint tsSec = ReadU32(b, pos, little);
            uint tsUsec = ReadU32(b, pos + 4, little);
            uint inclLen = ReadU32(b, pos + 8, little);
            pos += 16;
            if (pos + inclLen > b.Length) { Console.WriteLine("Truncated record at offset " + (pos - 16)); break; }
            byte[] data = new byte[inclLen];
            Array.Copy(b, pos, data, 0, inclLen);
            pos += (int)inclLen;
            count++;
            if (linktype != 249 || data.Length < 27) continue;

            int headerLen = data[0] | (data[1] << 8);
            ulong irpId = ReadU64(data, 2, true);
            uint usbdStatus = ReadU32(data, 10, true);
            ushort function = (ushort)(data[14] | (data[15] << 8));
            byte info = data[16];
            ushort busId = (ushort)(data[17] | (data[18] << 8));
            ushort devAddr = (ushort)(data[19] | (data[20] << 8));
            byte endpoint = data[21];
            byte transferType = data[22];
            uint dataLen = ReadU32(data, 23, true);

            bool isRequest = (info & 0x01) == 0; // FDO->PDO = host request
            bool isIn = (endpoint & 0x80) != 0;
            byte epNum = (byte)(endpoint & 0x0F);

            // The payload starts right after the header (whose length the
            // driver announces in header_len). For control transfers the
            // header includes one extra "control stage" byte at offset 27.
            int payloadOff = headerLen > 27 ? headerLen : 27;
            string stage = "";
            byte controlStage = 0xFF;
            if (headerLen >= 28 && payloadOff - 1 < data.Length)
            {
                controlStage = data[27];
                stage = " stage=" + StageName(controlStage);
            }

            string key = "dev=" + devAddr + " " + FuncName(function) + " ep=" + epNum + (isIn ? " IN" : " OUT") + " type=" + TransferTypeName(transferType);
            if (!statTable.ContainsKey(key)) statTable[key] = 0;
            statTable[key]++;

            // --ctlout: only vendor control writes (host->device SETUP stages).
            if (ctlout)
            {
                if (function == 0x0017 && controlStage == 0x00 && payloadOff + 8 <= data.Length)
                {
                    byte bm = data[payloadOff];
                    // Only host->device (OUT) vendor writes — a read's SETUP
                    // stage is also sent host->device, so filter on the setup
                    // packet's direction bit, not on the USBPcap info bit.
                    if ((bm & 0x80) == 0)
                    {
                        byte req = data[payloadOff + 1];
                        ushort wVal = (ushort)(data[payloadOff + 3] << 8 | data[payloadOff + 2]);
                        ushort wIdx = (ushort)(data[payloadOff + 5] << 8 | data[payloadOff + 4]);
                        ushort wLen = (ushort)(data[payloadOff + 7] << 8 | data[payloadOff + 6]);
                        Console.WriteLine("#" + count + " t=" + tsSec + "." + tsUsec.ToString("D6")
                            + " bReq=0x" + req.ToString("X2")
                            + " wVal=0x" + wVal.ToString("X4")
                            + " wIdx=0x" + wIdx.ToString("X4")
                            + " wLen=" + wLen);
                    }
                }
                continue;
            }

            bool interesting = (epFilter < 0 || epNum == epFilter || endpoint == epFilter)
                && (devFilter < 0 || devAddr == devFilter);
            if (bulk85)
            {
                if (transferType == 3 && epNum == 5 && dataLen == 480)
                {
                    if (bulkCount++ < bulkSkip) continue;
                    if (seriesN > 0 && bulkCount - bulkSkip > seriesN) break;
                    if (seriesN > 0)
                    {
                        // Compact time-series line for the 480-byte status block.
                        int p = payloadOff;
                        Console.WriteLine(tsSec + "." + tsUsec.ToString("D6")
                            + " m=" + ReadU32(data, p + 0x00, true).ToString("X8")
                            + " c=" + ReadU32(data, p + 0x04, true)
                            + " v8=" + ReadU32(data, p + 0x08, true).ToString("X8")
                            + " v10=" + ReadU32(data, p + 0x10, true).ToString("X8")
                            + " v18=" + ReadU32(data, p + 0x18, true).ToString("X8")
                            + " q60=" + ReadU64(data, p + 0x60, true).ToString("X16") + "," + ReadU64(data, p + 0x68, true).ToString("X16")
                            + " qE0=" + ReadU64(data, p + 0xE0, true).ToString("X16") + "," + ReadU64(data, p + 0xE8, true).ToString("X16")
                            + " p140=" + ReadU32(data, p + 0x140, true) + "," + ReadU32(data, p + 0x144, true) + "," + ReadU32(data, p + 0x148, true) + "," + ReadU32(data, p + 0x14C, true)
                            + " p170=" + ReadU32(data, p + 0x170, true) + "," + ReadU32(data, p + 0x174, true)
                            + " p1B0=" + ReadU32(data, p + 0x1B0, true) + "," + ReadU32(data, p + 0x1B4, true));
                        continue;
                    }
                    if (bulkCount - bulkSkip > 6) continue;
                    Console.WriteLine("#" + count + " t=" + tsSec + "." + tsUsec.ToString("D6"));
                    DumpFull480(data, (int)Math.Min(480, data.Length - payloadOff), payloadOff);
                }
                continue;
            }
            if (!interesting) continue;

            // USBPcap IRP-info records (URB submitted markers) carry no payload.
            if (transferType == 0xFE) continue;

            // USBPcap emits 2 records per ISO URB (payload + status); skip
            // the empty status record so filters see only the data-bearing one.
            if (transferType == 1 && dataLen == 0 && data.Length - payloadOff == 0) continue;

            if (transferType == 1 && isoSkip > 0) { isoSkip--; continue; }

            if (maxPrint > 0 && printed >= maxPrint) break;
            printed++;

            string dir = isIn ? "IN " : "OUT";
            Console.WriteLine("#" + count + " t=" + tsSec + "." + tsUsec.ToString("D6")
                + " " + dir + " dev=" + devAddr + " bus=" + busId
                + " ep=" + (isIn ? "0x8" : "0x") + epNum.ToString("X1")
                + " fn=0x" + function.ToString("X4") + "(" + FuncName(function) + ")"
                + " type=" + TransferTypeName(transferType) + stage
                + " data=" + dataLen + " irp=0x" + irpId.ToString("X8") + (usbdStatus != 0 ? " status=0x" + usbdStatus.ToString("X8") : ""));

            int avail = data.Length - payloadOff;
            if (transferType == 1)
            {
                // USBPcap stores the ISO payload after the header; the header's
                // data_len is often 0 for ISO, so dump what was actually captured.
                Console.WriteLine("    hdrlen=" + headerLen + " dataLen=" + dataLen + " avail=" + avail);
                if (isoscan && avail >= 56)
                {
                    // Per-channel peak level scan (14ch x 32-bit, 56-byte frames):
                    // find which input channel carries signal at which time.
                    int nf = avail / 56;
                    long[] peak = new long[14];
                    for (int f = 0; f < nf; f++)
                    {
                        int fp = payloadOff + f * 56;
                        for (int c = 0; c < 14; c++)
                        {
                            long v = Math.Abs((long)(int)ReadU32(data, fp + c * 4, true));
                            if (v > peak[c]) peak[c] = v;
                        }
                    }
                    bool active = false;
                    for (int c = 0; c < 6; c++) if (peak[c] >= 2000) active = true;
                    if (active)
                        Console.WriteLine(tsSec + "." + tsUsec.ToString("D6")
                            + " ch0=" + peak[0] + " ch1=" + peak[1] + " ch2=" + peak[2]
                            + " ch3=" + peak[3] + " ch4=" + peak[4] + " ch5=" + peak[5]);
                    continue;
                }
                if (isodump && avail > 0)
                {
                    if (isoFrames > 0)
                    {
                        // Compact per-frame summary: decode 56-byte frames
                        // (14 channels x 32-bit) as signed 32-bit values.
                        int nf = Math.Min(isoFrames, avail / 56);
                        for (int f = 0; f < nf; f++)
                        {
                            int fp = payloadOff + f * 56;
                            Console.WriteLine("    f=" + f.ToString("D3")
                                + " ch0=" + (int)ReadU32(data, fp, true)
                                + " ch1=" + (int)ReadU32(data, fp + 4, true)
                                + " ch2=" + (int)ReadU32(data, fp + 8, true)
                                + " ch3=" + (int)ReadU32(data, fp + 12, true)
                                + " ch4=" + (int)ReadU32(data, fp + 16, true)
                                + " ch5=" + (int)ReadU32(data, fp + 20, true));
                        }
                        continue;
                    }
                    int show = Math.Min(avail, 1024);
                    int p = payloadOff;
                    if (avail >= 16)
                        Console.WriteLine("    dwords: " + ReadU32(data, p, true).ToString("X8") + " " + ReadU32(data, p + 4, true).ToString("X8")
                            + " " + ReadU32(data, p + 8, true).ToString("X8") + " " + ReadU32(data, p + 12, true).ToString("X8"));
                    DumpHex(data, (uint)p, (uint)show, ascii);
                }
                continue;
            }

            int len = (int)Math.Min(dataLen, avail);
            if (len > 0)
            {
                // SETUP stage: the 8 bytes are the USB setup packet.
                if (controlStage == 0x00 && payloadOff + 8 <= data.Length)
                {
                    byte bm = data[payloadOff];
                    byte req = data[payloadOff + 1];
                    ushort wVal = (ushort)(data[payloadOff + 3] << 8 | data[payloadOff + 2]);
                    ushort wIdx = (ushort)(data[payloadOff + 5] << 8 | data[payloadOff + 4]);
                    ushort wLen = (ushort)(data[payloadOff + 7] << 8 | data[payloadOff + 6]);
                    Console.WriteLine("    setup: dir=" + ((bm & 0x80) != 0 ? "IN" : "OUT")
                        + " type=" + ((bm >> 5) & 3) + " recip=" + (bm & 0x1F)
                        + " bReq=0x" + req.ToString("X2")
                        + " wVal=0x" + wVal.ToString("X4")
                        + " wIdx=0x" + wIdx.ToString("X4")
                        + " wLen=" + wLen);
                    DumpHex(data, (uint)(payloadOff + 8), (uint)Math.Max(0, len - 8), ascii);
                }
                else
                {
                    DumpHex(data, (uint)payloadOff, (uint)len, ascii);
                }
                Console.WriteLine();
            }
        }
        Console.WriteLine("Total records: " + count);

        if (stats)
        {
            Console.WriteLine();
            Console.WriteLine("── Per-endpoint / function statistics ──────────");
            foreach (KeyValuePair<string, int> kv in statTable)
                Console.WriteLine("  " + kv.Key.PadRight(48) + " " + kv.Value);
        }
        return 0;
    }

    static uint ReadU32(byte[] b, int off, bool little)
    {
        if (little) return (uint)(b[off] | (b[off + 1] << 8) | (b[off + 2] << 16) | (b[off + 3] << 24));
        return (uint)((b[off] << 24) | (b[off + 1] << 16) | (b[off + 2] << 8) | b[off + 3]);
    }

    static ulong ReadU64(byte[] b, int off, bool little)
    {
        if (little) return (ulong)ReadU32(b, off, true) | ((ulong)ReadU32(b, off + 4, true) << 32);
        return ((ulong)ReadU32(b, off, false) << 32) | ReadU32(b, off + 4, false);
    }

    static string StageName(byte s)
    {
        switch (s)
        {
            case 0: return "SETUP";
            case 1: return "DATA";
            case 2: return "STATUS";
            case 3: return "COMPLETE";
            default: return "0x" + s.ToString("X2");
        }
    }

    static string TransferTypeName(byte t)
    {
        switch (t)
        {
            case 0: return "CTRL";
            case 1: return "ISO";
            case 2: return "INTR";
            case 3: return "BULK";
            case 0xFE: return "IRPINFO";
            default: return "0x" + t.ToString("X2");
        }
    }

    static string FuncName(ushort f)
    {
        switch (f)
        {
            case 0x0000: return "SELECT_CONFIG";
            case 0x0001: return "SELECT_IFACE";
            case 0x0008: return "CONTROL_TRANSFER";
            case 0x0009: return "BULK_OR_INTR";
            case 0x000A: return "ISOCH_TRANSFER";
            case 0x000B: return "GET_DESC_DEV";
            case 0x000D: return "SET_FEATURE_DEV";
            case 0x0010: return "CLEAR_FEATURE_DEV";
            case 0x0013: return "GET_STATUS_DEV";
            case 0x0017: return "VENDOR_DEVICE";
            case 0x0018: return "VENDOR_IFACE";
            case 0x001A: return "CLASS_DEVICE";
            case 0x001B: return "CLASS_IFACE";
            case 0x0020: return "VENDOR_OTHER";
            case 0x0026: return "GET_CONFIG";
            case 0x0027: return "GET_IFACE";
            default: return "0x" + f.ToString("X4");
        }
    }

    static void DumpHex(byte[] data, uint off, uint len, bool ascii)
    {
        if (len == 0) return;
        const int perLine = 16;
        for (uint i = 0; i < len; i += perLine)
        {
            StringBuilder hex = new StringBuilder();
            StringBuilder asc = new StringBuilder();
            for (uint j = 0; j < perLine && i + j < len; j++)
            {
                byte v = data[off + i + j];
                hex.Append(v.ToString("X2") + " ");
                asc.Append(v >= 0x20 && v < 0x7F ? (char)v : '.');
            }
            while (hex.Length < perLine * 3) hex.Append(' ');
            Console.WriteLine("    " + ((off + i).ToString("X6")) + "  " + hex + (ascii ? " | " + asc.ToString() : ""));
        }
    }

    // Dump a 480-byte status block, showing only the non-zero regions
    // (the blocks are mostly zero when nothing is active).
    static void DumpFull480(byte[] data, int len, int off)
    {
        // collect non-zero runs
        int start = -1;
        for (int i = 0; i < len; i++)
        {
            bool nz = data[off + i] != 0;
            if (nz && start < 0) start = i;
            if (!nz && start >= 0)
            {
                PrintRun(data, off + start, i - start);
                start = -1;
            }
        }
        if (start >= 0) PrintRun(data, off + start, len - start);
    }

    static void PrintRun(byte[] data, int off, int len)
    {
        StringBuilder sb = new StringBuilder();
        sb.Append("  @0x" + off.ToString("X3") + ": ");
        for (int i = 0; i < len; i++) sb.Append(data[off + i].ToString("X2") + " ");
        Console.WriteLine(sb.ToString().TrimEnd());
    }
}
