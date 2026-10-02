// Why the virtual G920 may be missing from Windows, in words for the log. Portable (SelfTest checks it);
// DeviceCheck.cs does the Windows-only looking.
//
// The trap: removing the G920 while a program (the game, Steam) still has it open makes Windows refuse
// (Kernel-PnP event 225, "stopped the removal"). HIDMaestro's plain removal then leaves the device
// half-removed until Windows restarts, and a new G920 at the same identity gets no driver. The bridge avoids
// it (it asks first and waits, see Program.ReleaseDevice) and gets out of it (spare identities).

using System.Text.RegularExpressions;

namespace Wheelemu.Bridge;

public static class DeviceHealth
{
    // The program names in the text of Kernel-PnP event 225 ("The application \Device\...\NFS16.exe with
    // process id 6048 stopped the removal or ejection for the device ROOT\HIDCLASS\HM_0000 ..."), for our
    // device only, newest first as given, without repeats.
    public static List<string> RemovalBlockers(string eventsText)
    {
        var result = new List<string>();
        foreach (Match m in Regex.Matches(eventsText,
                     @"application\s+(?<app>.+?)\s+with process id\s+\d+\s+stopped the removal[^\r\n]*?device\s+(?<dev>\S+)",
                     RegexOptions.IgnoreCase))
        {
            string device = m.Groups["dev"].Value;
            if (!device.Contains("HM_", StringComparison.OrdinalIgnoreCase) &&
                !device.Contains("HIDMAESTRO", StringComparison.OrdinalIgnoreCase)) continue;
            string app = Path.GetFileName(m.Groups["app"].Value.Trim().Replace('/', '\\').Split('\\').Last());
            if (!result.Contains(app, StringComparer.OrdinalIgnoreCase)) result.Add(app);
        }
        return result;
    }

    public static string StuckAdvice(IReadOnlyCollection<string> blockers) =>
        "the virtual G920 has no game-controller driver, so games and joy.cpl cannot see it"
        + (blockers.Count > 0 ? $"; Windows could not remove the previous one because {string.Join(", ", blockers)} had it open" : "")
        + ". Every spare identity is stuck too: close the game and Steam, stop the bridge, restart Windows, then "
        + "start the service and the bridge again.";
}
