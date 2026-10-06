$ErrorActionPreference = 'Stop'

$source = @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

public static class AimMxlProbe
{
    [StructLayout(LayoutKind.Sequential)]
    private struct DeviceInterfaceData
    {
        public uint Size;
        public Guid InterfaceClassGuid;
        public uint Flags;
        public IntPtr Reserved;
    }

    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Ansi)]
    private static extern IntPtr SetupDiGetClassDevs(ref Guid classGuid, IntPtr enumerator,
        IntPtr parent, uint flags);

    [DllImport("setupapi.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiEnumDeviceInterfaces(IntPtr set, IntPtr device,
        ref Guid classGuid, uint index, ref DeviceInterfaceData data);

    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Ansi)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr set,
        ref DeviceInterfaceData data, IntPtr detail, uint detailLength,
        out uint required, IntPtr device);

    [DllImport("setupapi.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Ansi)]
    private static extern IntPtr CreateFile(string name, uint access, uint share,
        IntPtr security, uint disposition, uint flags, IntPtr template);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DeviceIoControl(IntPtr file, uint code, IntPtr input,
        uint inputLength, byte[] output, uint outputLength, out uint returned,
        IntPtr overlapped);

    [DllImport("kernel32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(IntPtr handle);

    private static void CheckPipe(string path, string name)
    {
        IntPtr pipe = CreateFile(path + name, 0xC0000000, 3, IntPtr.Zero, 3, 0,
            IntPtr.Zero);
        if (pipe == new IntPtr(-1))
            throw new Win32Exception(Marshal.GetLastWin32Error(), name);
        CloseHandle(pipe);
    }

    public static byte[] ReadConfiguration()
    {
        Guid classGuid = new Guid("af171a6e-5c83-47fd-baec-3b0498f2998f");
        IntPtr set = SetupDiGetClassDevs(ref classGuid, IntPtr.Zero, IntPtr.Zero, 0x12);
        if (set == new IntPtr(-1)) throw new Win32Exception(Marshal.GetLastWin32Error());
        try
        {
            DeviceInterfaceData data = new DeviceInterfaceData();
            data.Size = (uint)Marshal.SizeOf(typeof(DeviceInterfaceData));
            if (!SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref classGuid, 0, ref data))
                throw new Win32Exception(Marshal.GetLastWin32Error());

            uint required;
            SetupDiGetDeviceInterfaceDetail(set, ref data, IntPtr.Zero, 0, out required,
                IntPtr.Zero);
            if (required < 8) throw new Win32Exception(Marshal.GetLastWin32Error());
            IntPtr detail = Marshal.AllocHGlobal((int)required);
            try
            {
                Marshal.WriteInt32(detail, IntPtr.Size == 8 ? 8 : 5);
                if (!SetupDiGetDeviceInterfaceDetail(set, ref data, detail, required,
                    out required, IntPtr.Zero))
                    throw new Win32Exception(Marshal.GetLastWin32Error());

                string path = Marshal.PtrToStringAnsi(IntPtr.Add(detail, 4));
                IntPtr file = CreateFile(path, 0xC0000000, 0, IntPtr.Zero, 3, 0,
                    IntPtr.Zero);
                if (file == new IntPtr(-1))
                    throw new Win32Exception(Marshal.GetLastWin32Error());
                try
                {
                    byte[] output = new byte[4096];
                    uint returned;
                    if (!DeviceIoControl(file, 0x220000, IntPtr.Zero, 0, output,
                        (uint)output.Length, out returned, IntPtr.Zero))
                        throw new Win32Exception(Marshal.GetLastWin32Error());
                    CheckPipe(path, "\\Bulk_Read_Pipe");
                    CheckPipe(path, "\\Bulk_Write_Pipe");
                    Array.Resize(ref output, (int)returned);
                    return output;
                }
                finally { CloseHandle(file); }
            }
            finally { Marshal.FreeHGlobal(detail); }
        }
        finally { SetupDiDestroyDeviceInfoList(set); }
    }
}
'@

Add-Type -TypeDefinition $source
$descriptor = [AimMxlProbe]::ReadConfiguration()
if ($descriptor.Length -lt 9 -or $descriptor[1] -ne 2) {
    throw 'Driver returned an invalid USB configuration descriptor'
}
[PSCustomObject]@{
    DescriptorLength = $descriptor.Length
    TotalLength = [BitConverter]::ToUInt16($descriptor, 2)
    Interfaces = $descriptor[4]
}