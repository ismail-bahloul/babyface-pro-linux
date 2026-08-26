// usbdump.cs — USB descriptor dumper for RE purposes.
//
// Reads the device descriptor and the open pipes (endpoints) of a USB
// device directly from the host controller, without any vendor driver.
// Works against the Windows 11 24H2 USB stack, which uses a different
// IOCTL numbering than classic usbioctl.h (values were discovered by
// live IOCTL scanning — see README.md in this folder).
//
// Build (Windows, any .NET Framework csc):
//   csc.exe /nologo /out:usbdump.exe usbdump.cs
// Usage:
//   usbdump [vid] [pid]     (hex, optional, default 2a39 3fc0 = RME Babyface Pro FS)
//
// Output: out/device.txt
//
// Note: this stack does NOT expose IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_
// CONNECTION, so the full configuration descriptor (with the USB Audio
// Control topology) cannot be dumped from Windows 11 24H2. Use Linux
// (`lsusb -v`) for that. See README.md.
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

class UsbDump
{
    // ── Win32 ─────────────────────────────────────────────────
    const uint GENERIC_READ = 0x80000000;
    const uint GENERIC_WRITE = 0x40000000;
    const uint FILE_SHARE_READ = 0x00000001;
    const uint FILE_SHARE_WRITE = 0x00000002;
    const uint OPEN_EXISTING = 3;
    const uint FILE_ATTRIBUTE_NORMAL = 0x80;

    const uint FILE_DEVICE_USB = 0x22;
    // Verified by live IOCTL scan against the Windows 11 24H2 hub stack.
    // The classic usbioctl.h values (0x220030, 0x2200A8, 0x22001C) return
    // ERROR_NOT_SUPPORTED here — the modern driver uses these instead:
    static readonly uint IOCTL_USB_GET_NODE_INFORMATION = 0x220408;
    static readonly uint IOCTL_USB_GET_NODE_CONNECTION_INFORMATION = 0x220448; // EX2 variant

    static readonly Guid GUID_DEVINTERFACE_USB_DEVICE = new Guid("A5DCBF10-6530-11D2-901F-00C04FB951ED");
    static readonly Guid GUID_DEVINTERFACE_USB_HUB = new Guid("F18A0E88-C30C-11D0-8815-00A0C906BED8");

    const uint DIGCF_PRESENT = 0x00000002;
    const uint DIGCF_DEVICEINTERFACE = 0x00000010;

    [DllImport("setupapi.dll", SetLastError = true)]
    static extern IntPtr SetupDiGetClassDevs(ref Guid ClassGuid, IntPtr Enumerator, IntPtr hwndParent, uint Flags);

    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiEnumDeviceInterfaces(IntPtr DeviceInfoSet, IntPtr DeviceInfoData, ref Guid InterfaceClassGuid, uint MemberIndex, ref SP_DEVICE_INTERFACE_DATA DeviceInterfaceData);

    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr DeviceInfoSet, ref SP_DEVICE_INTERFACE_DATA DeviceInterfaceData, IntPtr DeviceInterfaceDetailData, uint DeviceInterfaceDetailDataSize, out uint RequiredSize, IntPtr DeviceInfoData);

    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiDestroyDeviceInfoList(IntPtr DeviceInfoSet);

    [DllImport("kernel32.dll", CharSet = CharSet.Auto, SetLastError = true)]
    static extern IntPtr CreateFile(string lpFileName, uint dwDesiredAccess, uint dwShareMode, IntPtr lpSecurityAttributes, uint dwCreationDisposition, uint dwFlagsAndAttributes, IntPtr hTemplateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(IntPtr hDevice, uint dwIoControlCode, IntPtr lpInBuffer, uint nInBufferSize, IntPtr lpOutBuffer, uint nOutBufferSize, out uint lpBytesReturned, IntPtr lpOverlapped);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr hObject);

    [StructLayout(LayoutKind.Sequential)]
    struct SP_DEVICE_INTERFACE_DATA
    {
        public uint cbSize;
        public Guid InterfaceClassGuid;
        public uint Flags;
        public IntPtr Reserved;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    struct USB_DEVICE_DESCRIPTOR
    {
        public byte bLength;
        public byte bDescriptorType;
        public ushort bcdUSB;
        public byte bDeviceClass;
        public byte bDeviceSubClass;
        public byte bDeviceProtocol;
        public byte bMaxPacketSize0;
        public ushort idVendor;
        public ushort idProduct;
        public ushort bcdDevice;
        public byte iManufacturer;
        public byte iProduct;
        public byte iSerialNumber;
        public byte bNumConfigurations;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    struct USB_HUB_DESCRIPTOR
    {
        public byte bDescLength;
        public byte bDescriptorType;
        public byte bNumberOfPorts;
        public ushort wHubCharacteristics;
        public byte bPwrOn2PwrGood;
        public byte bHubContrCurrent;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    struct USB_NODE_CONNECTION_INFORMATION_EX2
    {
        public uint ConnectionIndex;
        public USB_DEVICE_DESCRIPTOR DeviceDescriptor;
        public byte CurrentConfigurationValue;
        public byte Speed;
        public byte DeviceIsHub;
        public ushort DeviceAddress;
        public uint NumberOfOpenPipes;
        public uint NumberOfIsocPipes;
    }

    // ── Main ──────────────────────────────────────────────────
    static int Main(string[] args)
    {
        int vid = 0x2a39, pid = 0x3fc0;
        if (args.Length >= 2)
        {
            vid = Convert.ToInt32(args[0], 16);
            pid = Convert.ToInt32(args[1], 16);
        }
        string outDir = "out";
        Directory.CreateDirectory(outDir);

        StringBuilder report = new StringBuilder();
        report.AppendLine("USB descriptor dump — VID=0x" + vid.ToString("X4") + " PID=0x" + pid.ToString("X4"));
        report.AppendLine("Date: " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss"));
        report.AppendLine();

        Console.WriteLine("Target: VID=0x" + vid.ToString("X4") + " PID=0x" + pid.ToString("X4"));

        string devPath = FindInterfacePath(GUID_DEVINTERFACE_USB_DEVICE, vid, pid);
        if (devPath != null)
        {
            Console.WriteLine("Device path : " + devPath);
            report.AppendLine("Device path : " + devPath);
        }

        // Walk every hub, ask about every port, find ours.
        IntPtr hHub = IntPtr.Zero;
        int port = 0;
        USB_DEVICE_DESCRIPTOR devDesc = new USB_DEVICE_DESCRIPTOR();
        foreach (string hubPath in GetInterfacePaths(GUID_DEVINTERFACE_USB_HUB))
        {
            IntPtr h = CreateFile(hubPath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, IntPtr.Zero, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, IntPtr.Zero);
            if (h == new IntPtr(-1)) continue;
            byte numPorts = GetHubPortCount(h);
            for (uint p = 1; p <= numPorts; p++)
            {
                USB_DEVICE_DESCRIPTOR d;
                byte[] pipes;
                if (GetNodeConnection(h, p, out d, out pipes) && d.idVendor == vid && d.idProduct == pid)
                {
                    port = (int)p;
                    hHub = h;
                    devDesc = d;
                    Console.WriteLine("Attached to hub: " + hubPath);
                    Console.WriteLine("Hub port      : " + port);
                    report.AppendLine("Hub           : " + hubPath);
                    report.AppendLine("Hub port      : " + port);
                    break;
                }
            }
            if (hHub != IntPtr.Zero) break;
            CloseHandle(h);
        }

        if (hHub == IntPtr.Zero)
        {
            Console.WriteLine("ERROR: device not found on any hub (need Administrator for hub access?).");
            return 1;
        }

        Console.WriteLine();
        Console.WriteLine("── Device descriptor ────────────────────────────");
        Console.WriteLine("bcdUSB        : " + Bcd(devDesc.bcdUSB));
        Console.WriteLine("device class  : 0x" + devDesc.bDeviceClass.ToString("X2") + " (" + ClassName(devDesc.bDeviceClass) + ")");
        Console.WriteLine("sub/proto     : 0x" + devDesc.bDeviceSubClass.ToString("X2") + " / 0x" + devDesc.bDeviceProtocol.ToString("X2"));
        Console.WriteLine("vid:pid       : " + devDesc.idVendor.ToString("X4") + ":" + devDesc.idProduct.ToString("X4"));
        Console.WriteLine("bcdDevice     : " + Bcd(devDesc.bcdDevice));
        Console.WriteLine("configurations: " + devDesc.bNumConfigurations);

        report.AppendLine("── Device descriptor ────────────────────────────");
        report.AppendLine("bcdUSB        : " + Bcd(devDesc.bcdUSB));
        report.AppendLine("device class  : 0x" + devDesc.bDeviceClass.ToString("X2") + " (" + ClassName(devDesc.bDeviceClass) + ")");
        report.AppendLine("sub/proto     : 0x" + devDesc.bDeviceSubClass.ToString("X2") + " / 0x" + devDesc.bDeviceProtocol.ToString("X2"));
        report.AppendLine("vid:pid       : " + devDesc.idVendor.ToString("X4") + ":" + devDesc.idProduct.ToString("X4"));
        report.AppendLine("bcdDevice     : " + Bcd(devDesc.bcdDevice));
        report.AppendLine("configurations: " + devDesc.bNumConfigurations);
        report.AppendLine();

        // Open pipes (endpoints) of the current configuration.
        byte[] pipeRaw;
        USB_NODE_CONNECTION_INFORMATION_EX2 info;
        if (GetNodeConnectionFull(hHub, (uint)port, out info, out pipeRaw))
        {
            Console.WriteLine();
            Console.WriteLine("── Current configuration / open pipes ─────────");
            Console.WriteLine("speed         : " + SpeedName(info.Speed));
            Console.WriteLine("device addr   : " + info.DeviceAddress);
            Console.WriteLine("config value  : " + info.CurrentConfigurationValue);
            Console.WriteLine("open pipes    : " + info.NumberOfOpenPipes + " (isoch " + info.NumberOfIsocPipes + ")");

            report.AppendLine("── Current configuration / open pipes ─────────");
            report.AppendLine("speed         : " + SpeedName(info.Speed));
            report.AppendLine("device addr   : " + info.DeviceAddress);
            report.AppendLine("config value  : " + info.CurrentConfigurationValue);
            report.AppendLine("open pipes    : " + info.NumberOfOpenPipes + " (isoch " + info.NumberOfIsocPipes + ")");
            report.AppendLine();

            // USB_PIPE_INFO: { USB_ENDPOINT_DESCRIPTOR(7) + ULONG ScheduleOffset } = 11 bytes
            const int pipeInfoSize = 11;
            int offset = Marshal.SizeOf(typeof(USB_NODE_CONNECTION_INFORMATION_EX2));
            Console.WriteLine();
            Console.WriteLine("── Endpoints (from pipe list) ────────────────");
            report.AppendLine("── Endpoints (from pipe list) ────────────────");
            for (int i = 0; i < info.NumberOfOpenPipes && offset + pipeInfoSize <= pipeRaw.Length; i++, offset += pipeInfoSize)
            {
                byte bLen = pipeRaw[offset];
                byte bType = pipeRaw[offset + 1];
                byte bAddr = pipeRaw[offset + 2];
                byte bAttr = pipeRaw[offset + 3];
                ushort maxPkt = (ushort)(pipeRaw[offset + 4] | (pipeRaw[offset + 5] << 8));
                byte interval = pipeRaw[offset + 6];
                uint sched = BitConverter.ToUInt32(pipeRaw, offset + 7);
                string dir = (bAddr & 0x80) != 0 ? "IN " : "OUT";
                string line = "  pipe " + (i + 1).ToString("D2") + ": ep 0x" + (bAddr & 0x0F).ToString("X2")
                    + " " + dir + " " + EpAttrName(bAttr) + " maxPkt=" + maxPkt + " interval=" + interval + " sched=" + sched;
                Console.WriteLine(line);
                report.AppendLine(line);
            }
            report.AppendLine();
        }

        File.WriteAllText(Path.Combine(outDir, "device.txt"), report.ToString(), Encoding.UTF8);
        Console.WriteLine();
        Console.WriteLine("Report written to " + Path.GetFullPath(Path.Combine(outDir, "device.txt")));
        CloseHandle(hHub);
        return 0;
    }

    // ── SetupAPI helpers ──────────────────────────────────────
    static List<string> GetInterfacePaths(Guid classGuid)
    {
        List<string> paths = new List<string>();
        IntPtr set = SetupDiGetClassDevs(ref classGuid, IntPtr.Zero, IntPtr.Zero, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (set == new IntPtr(-1)) return paths;
        try
        {
            for (uint i = 0; ; i++)
            {
                SP_DEVICE_INTERFACE_DATA did = new SP_DEVICE_INTERFACE_DATA();
                did.cbSize = (uint)Marshal.SizeOf(typeof(SP_DEVICE_INTERFACE_DATA));
                if (!SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref classGuid, i, ref did)) break;
                uint req = 0;
                // First call with a null buffer deliberately "fails" with
                // ERROR_INSUFFICIENT_BUFFER — that is how the size is learned.
                SetupDiGetDeviceInterfaceDetail(set, ref did, IntPtr.Zero, 0, out req, IntPtr.Zero);
                if (req == 0) continue;
                IntPtr detail = Marshal.AllocHGlobal((int)req);
                try
                {
                    // The ANSI detail struct is { DWORD cbSize; CHAR path[] } —
                    // the path starts right after cbSize at offset 4.
                    Marshal.WriteInt32(detail, IntPtr.Size);
                    if (SetupDiGetDeviceInterfaceDetail(set, ref did, detail, req, out req, IntPtr.Zero))
                    {
                        string path = Marshal.PtrToStringAnsi(new IntPtr(detail.ToInt64() + 4));
                        if (!String.IsNullOrEmpty(path)) paths.Add(path);
                    }
                }
                finally { Marshal.FreeHGlobal(detail); }
            }
        }
        finally { SetupDiDestroyDeviceInfoList(set); }
        return paths;
    }

    static string FindInterfacePath(Guid classGuid, int vid, int pid)
    {
        string needle = "VID_" + vid.ToString("X4") + "&PID_" + pid.ToString("X4");
        foreach (string path in GetInterfacePaths(classGuid))
            if (path.IndexOf(needle, StringComparison.OrdinalIgnoreCase) >= 0)
                return path;
        return null;
    }

    // ── Hub IOCTLs ────────────────────────────────────────────
    static byte GetHubPortCount(IntPtr hHub)
    {
        IntPtr ptr = Marshal.AllocHGlobal(4096);
        IntPtr inBuf = Marshal.AllocHGlobal(32);
        try
        {
            uint returned = 0;
            if (!DeviceIoControl(hHub, IOCTL_USB_GET_NODE_INFORMATION, inBuf, 32, ptr, 4096, out returned, IntPtr.Zero)) return 0;
            // This hub driver prefixes the reply with a 4-byte zero header,
            // so the USB 3.0 hub descriptor starts at offset 4.
            byte[] head = new byte[10];
            Marshal.Copy(ptr, head, 0, 10);
            if (head[4] == 0x09 && head[5] == 0x29) return head[6];
            if (head[0] == 0x09 && head[1] == 0x29) return head[2];
            return 0;
        }
        finally { Marshal.FreeHGlobal(ptr); Marshal.FreeHGlobal(inBuf); }
    }

    static bool GetNodeConnection(IntPtr hHub, uint port, out USB_DEVICE_DESCRIPTOR dev, out byte[] pipeRaw)
    {
        USB_NODE_CONNECTION_INFORMATION_EX2 info;
        bool ok = GetNodeConnectionFull(hHub, port, out info, out pipeRaw);
        dev = ok ? info.DeviceDescriptor : new USB_DEVICE_DESCRIPTOR();
        return ok;
    }

    static bool GetNodeConnectionFull(IntPtr hHub, uint port, out USB_NODE_CONNECTION_INFORMATION_EX2 info, out byte[] pipeRaw)
    {
        info = new USB_NODE_CONNECTION_INFORMATION_EX2();
        pipeRaw = null;
        int structSize = Marshal.SizeOf(typeof(USB_NODE_CONNECTION_INFORMATION_EX2));
        IntPtr ptr = Marshal.AllocHGlobal(4096);
        try
        {
            USB_NODE_CONNECTION_INFORMATION_EX2 s = new USB_NODE_CONNECTION_INFORMATION_EX2();
            s.ConnectionIndex = port;
            Marshal.StructureToPtr(s, ptr, false);
            uint returned = 0;
            if (!DeviceIoControl(hHub, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION, ptr, (uint)structSize, ptr, 4096, out returned, IntPtr.Zero)) return false;
            info = (USB_NODE_CONNECTION_INFORMATION_EX2)Marshal.PtrToStructure(ptr, typeof(USB_NODE_CONNECTION_INFORMATION_EX2));
            pipeRaw = new byte[returned];
            Marshal.Copy(ptr, pipeRaw, 0, (int)returned);
            return true;
        }
        finally { Marshal.FreeHGlobal(ptr); }
    }

    // ── Name tables ───────────────────────────────────────────
    static string Bcd(ushort bcd)
    {
        return ((bcd >> 8) & 0xFF).ToString("X2") + "." + (bcd & 0xFF).ToString("X2");
    }

    static string ClassName(byte cls)
    {
        switch (cls)
        {
            case 0x00: return "Reserved";
            case 0x01: return "Audio";
            case 0x02: return "CDC";
            case 0x03: return "HID";
            case 0x06: return "Image";
            case 0x07: return "Printer";
            case 0x08: return "Mass Storage";
            case 0x09: return "Hub";
            case 0x0A: return "CDC-Data";
            case 0x0B: return "Smart Card";
            case 0x0D: return "Content Security";
            case 0x0E: return "Video";
            case 0x0F: return "Personal Healthcare";
            case 0x10: return "Audio/Video";
            case 0xDC: return "Diagnostic";
            case 0xE0: return "Wireless Controller";
            case 0xFE: return "Application Specific";
            case 0xFF: return "Vendor Specific";
            default: return "0x" + cls.ToString("X2");
        }
    }

    static string EpAttrName(byte bmAttr)
    {
        string t;
        switch (bmAttr & 0x03)
        {
            case 0: t = "Control"; break;
            case 1: t = "Isochronous"; break;
            case 2: t = "Bulk"; break;
            default: t = "Interrupt"; break;
        }
        if ((bmAttr & 0x03) == 1)
        {
            switch ((bmAttr >> 2) & 0x03)
            {
                case 0: t += "(async)"; break;
                case 1: t += "(adaptive)"; break;
                default: t += "(sync)"; break;
            }
        }
        return t;
    }

    static string SpeedName(byte speed)
    {
        switch (speed)
        {
            case 0: return "Low";
            case 1: return "Full";
            case 2: return "High";
            case 3: return "Super";
            case 4: return "SuperPlus";
            default: return "0x" + speed.ToString("X2");
        }
    }
}
