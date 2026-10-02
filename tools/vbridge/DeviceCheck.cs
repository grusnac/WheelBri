// Windows-only checks around the virtual G920's life: is its game-controller part really there after the
// bridge created it, and did a program block its removal when the bridge stopped. See DeviceHealth.cs.

using System.Diagnostics;
using System.Reflection;
using System.Text;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace Wheelemu.Bridge;

static class DeviceCheck
{
    [DllImport("hid.dll")]
    static extern void HidD_GetHidGuid(out Guid guid);

    [StructLayout(LayoutKind.Sequential)]
    struct HiddAttributes
    {
        public int Size;
        public ushort VendorId, ProductId, Version;
    }

    [DllImport("hid.dll", SetLastError = true)]
    static extern bool HidD_GetAttributes(SafeFileHandle device, ref HiddAttributes attributes);

    [DllImport("CfgMgr32.dll", CharSet = CharSet.Unicode)]
    static extern int CM_Get_Device_Interface_List_SizeW(out int length, ref Guid interfaceClass, string? deviceId, int flags);

    [DllImport("CfgMgr32.dll", CharSet = CharSet.Unicode)]
    static extern int CM_Get_Device_Interface_ListW(ref Guid interfaceClass, string? deviceId, char[] buffer, int length, int flags);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr security, uint creation,
                                             uint flags, IntPtr template);

    // True if a working HID interface with these ids exists (the game-controller part has its driver).
    public static bool HidDevicePresent(ushort vid, ushort pid)
    {
        HidD_GetHidGuid(out Guid hid);
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            if (CM_Get_Device_Interface_List_SizeW(out int length, ref hid, null, 0) != 0 || length <= 1) return false;
            var buffer = new char[length];
            if (CM_Get_Device_Interface_ListW(ref hid, null, buffer, length, 0) != 0) continue;   // list grew meanwhile
            foreach (string path in new string(buffer).Split('\0', StringSplitOptions.RemoveEmptyEntries))
            {
                using var handle = CreateFileW(path, 0, 3 /* read | write sharing */, IntPtr.Zero, 3 /* open existing */, 0, IntPtr.Zero);
                if (handle.IsInvalid) continue;
                var attributes = new HiddAttributes { Size = Marshal.SizeOf<HiddAttributes>() };
                if (HidD_GetAttributes(handle, ref attributes) && attributes.VendorId == vid && attributes.ProductId == pid)
                {
                    return true;
                }
            }
            return false;
        }
        return false;
    }

    // Programs that blocked the removal of a HIDMaestro device in the last `window` (Kernel-PnP event 225).
    public static List<string> RecentRemovalBlockers(TimeSpan window)
    {
        try
        {
            long ms = (long)window.TotalMilliseconds;
            var info = new ProcessStartInfo("wevtutil.exe")
            {
                RedirectStandardOutput = true,
                UseShellExecute = false,
                CreateNoWindow = true,
            };
            foreach (var a in new[] { "qe", "System", "/rd:true", "/c:20", "/f:text",
                         $"/q:*[System[Provider[@Name='Microsoft-Windows-Kernel-PnP'] and (EventID=225) and TimeCreated[timediff(@SystemTime) <= {ms}]]]" })
            {
                info.ArgumentList.Add(a);
            }
            using var process = Process.Start(info);
            if (process == null) return new List<string>();
            string text = process.StandardOutput.ReadToEnd();
            process.WaitForExit(5000);
            return DeviceHealth.RemovalBlockers(text);
        }
        catch (Exception)
        {
            return new List<string>();
        }
    }

    // After creating the device: its game-controller part should appear within a few seconds.
    public static bool WaitForGameController(ushort vid, ushort pid, TimeSpan timeout, out long readyMs)
    {
        var clock = Stopwatch.StartNew();
        while (clock.Elapsed < timeout)
        {
            if (HidDevicePresent(vid, pid))
            {
                readyMs = clock.ElapsedMilliseconds;
                return true;
            }
            Thread.Sleep(250);
        }
        readyMs = clock.ElapsedMilliseconds;
        return false;
    }

    // HIDMaestro keeps the device instance id (ROOT\HIDCLASS\HM_...) internal.
    public static string? InstanceIdOf(object controller) =>
        controller.GetType().GetProperty("InstanceId", BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public)
            ?.GetValue(controller) as string;

    [DllImport("CfgMgr32.dll", CharSet = CharSet.Unicode)]
    static extern int CM_Locate_DevNodeW(out uint devInst, string deviceId, int flags);

    [DllImport("CfgMgr32.dll", CharSet = CharSet.Unicode)]
    static extern int CM_Query_And_Remove_SubTreeW(uint devInst, out int vetoType, StringBuilder vetoName, int nameLength, int flags);

    const int CrSuccess = 0, CrNoSuchDevnode = 0x0D, CrRemoveVetoed = 0x17;
    const int RemoveUiNotOk = 0x1, RemoveNoRestart = 0x2;

    public enum RemoveResult { Removed, Vetoed, Gone, Failed }

    static readonly string[] VetoTypes =
    {
        "unknown", "a legacy device", "a pending close", "an application", "a service", "an open handle",
        "a device", "a driver", "an illegal request", "insufficient power", "a non-disableable device",
        "a legacy driver", "insufficient rights",
    };

    // Asks Windows to remove the device and its children, which first asks every program holding it; a
    // refusal changes nothing (the device keeps working). Reason = who refused, as well as can be told.
    public static (RemoveResult Result, string Reason) TryRemove(string instanceId)
    {
        int cr = CM_Locate_DevNodeW(out uint devInst, instanceId, 0);
        if (cr == CrNoSuchDevnode) return (RemoveResult.Gone, "");
        if (cr != CrSuccess) return (RemoveResult.Failed, $"CM_Locate_DevNode 0x{cr:X}");
        var name = new StringBuilder(512);
        cr = CM_Query_And_Remove_SubTreeW(devInst, out int vetoType, name, name.Capacity, RemoveUiNotOk | RemoveNoRestart);
        if (cr == CrSuccess) return (RemoveResult.Removed, "");
        if (cr != CrRemoveVetoed) return (RemoveResult.Failed, $"CM_Query_And_Remove_SubTree 0x{cr:X}");
        // The program's name is in the Kernel-PnP event Windows writes for the refusal.
        Thread.Sleep(200);
        var blockers = RecentRemovalBlockers(TimeSpan.FromSeconds(15));
        if (blockers.Count > 0) return (RemoveResult.Vetoed, string.Join(", ", blockers));
        string type = vetoType >= 0 && vetoType < VetoTypes.Length ? VetoTypes[vetoType] : $"veto {vetoType}";
        return (RemoveResult.Vetoed, name.Length > 0 ? $"{type} ({name})" : type);
    }
}
