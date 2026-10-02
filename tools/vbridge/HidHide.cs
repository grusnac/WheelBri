// Hides the physical wheel from games while the bridge runs, through HidHide's command-line tool
// (https://github.com/nefarius/HidHide), and puts everything back on exit. wheelemu-service is added to
// HidHide's application list so it keeps reading the wheel.
//
// Only the wheel's game-controller interfaces are hidden ("gamingDevice": true), so a vendor tool such as
// Moza Pit House, which talks to other interfaces, keeps working.
//
// The parsing functions are portable (SelfTest checks them); running the tool is Windows-only.

using System.Diagnostics;
using System.Text.Json;

namespace Wheelemu.Bridge;

public sealed class HidHide
{
    readonly string _cli;
    readonly Action<string> _log;
    readonly List<string> _hiddenByUs = new();
    bool _cloakWasOff;

    HidHide(string cli, Action<string> log)
    {
        _cli = cli;
        _log = log;
    }

    // HidHideCLI.exe in its install folder (newer releases put it at the root, older ones under x64\), or null.
    public static HidHide? Find(string? cliOverride, Action<string> log)
    {
        var candidates = new List<string>();
        if (!string.IsNullOrEmpty(cliOverride)) candidates.Add(cliOverride);
        foreach (var root in new[] { Environment.GetEnvironmentVariable("ProgramW6432"),
                                     Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles) })
        {
            if (string.IsNullOrEmpty(root)) continue;
            string folder = Path.Combine(root, "Nefarius Software Solutions", "HidHide");
            candidates.Add(Path.Combine(folder, "HidHideCLI.exe"));
            candidates.Add(Path.Combine(folder, "x64", "HidHideCLI.exe"));
        }
        foreach (var path in candidates)
        {
            if (File.Exists(path)) return new HidHide(path, log);
        }
        return null;
    }

    // Device instance paths of the game-controller interfaces of devices with `vendorId` (e.g. "346E"), from
    // the JSON of `HidHideCLI --dev-gaming`: [ { "friendlyName": ..., "devices": [ { "gamingDevice": true,
    // "deviceInstancePath": "HID\\VID_346E&PID_0005&MI_02\\...", ... } ] } ].
    public static List<string> GamingDevicesOf(string json, string vendorId)
    {
        var result = new List<string>();
        using var doc = JsonDocument.Parse(json);
        foreach (var container in doc.RootElement.EnumerateArray())
        {
            if (!container.TryGetProperty("devices", out var devices)) continue;
            foreach (var device in devices.EnumerateArray())
            {
                bool gaming = device.TryGetProperty("gamingDevice", out var g) && g.ValueKind == JsonValueKind.True;
                string path = device.TryGetProperty("deviceInstancePath", out var p) ? p.GetString() ?? "" : "";
                if (gaming && path.Contains($"VID_{vendorId}", StringComparison.OrdinalIgnoreCase) &&
                    !result.Contains(path, StringComparer.OrdinalIgnoreCase))
                {
                    result.Add(path);
                }
            }
        }
        return result;
    }

    // Every device of `vendorId` in the `--dev-gaming` JSON, described for the log (why nothing was chosen).
    public static List<string> DescribeDevicesOf(string json, string vendorId)
    {
        var result = new List<string>();
        using var doc = JsonDocument.Parse(json);
        foreach (var container in doc.RootElement.EnumerateArray())
        {
            if (!container.TryGetProperty("devices", out var devices)) continue;
            foreach (var device in devices.EnumerateArray())
            {
                string path = device.TryGetProperty("deviceInstancePath", out var p) ? p.GetString() ?? "" : "";
                if (!path.Contains($"VID_{vendorId}", StringComparison.OrdinalIgnoreCase)) continue;
                string Flag(string name) => device.TryGetProperty(name, out var v) ? v.ToString() : "?";
                result.Add($"{path} (gamingDevice {Flag("gamingDevice")}, present {Flag("present")})");
            }
        }
        return result;
    }

    // Lines of `--dev-list` / `--app-list`: --dev-hide "PATH" / --app-reg "PATH".
    public static List<string> QuotedValues(string output)
    {
        var result = new List<string>();
        foreach (var line in output.Split('\n'))
        {
            int first = line.IndexOf('"'), last = line.LastIndexOf('"');
            if (first >= 0 && last > first) result.Add(line.Substring(first + 1, last - first - 1));
        }
        return result;
    }

    (int Exit, string Output) Run(params string[] args)
    {
        var info = new ProcessStartInfo(_cli)
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true,
        };
        foreach (var a in args) info.ArgumentList.Add(a);
        using var process = Process.Start(info) ?? throw new InvalidOperationException($"cannot start {_cli}");
        string output = process.StandardOutput.ReadToEnd();
        string error = process.StandardError.ReadToEnd();
        process.WaitForExit();
        if (process.ExitCode != 0)
        {
            _log($"hidhide: {string.Join(' ', args)} -> exit code {process.ExitCode} {error.Trim()}");
        }
        return (process.ExitCode, output);
    }

    // Allows `serviceExe`, hides the wheel's game-controller interfaces, turns hiding on. False if nothing
    // could be hidden.
    public bool HideWheel(string vendorId, string serviceExe)
    {
        var apps = QuotedValues(Run("--app-list").Output);
        if (!apps.Contains(serviceExe, StringComparer.OrdinalIgnoreCase))
        {
            Run("--app-reg", serviceExe);
            _log($"hidhide: {serviceExe} may still see hidden devices");
        }

        var (exit, json) = Run("--dev-gaming");
        List<string> wheel;
        try
        {
            wheel = exit == 0 ? GamingDevicesOf(json, vendorId) : new List<string>();
        }
        catch (JsonException e)
        {
            _log($"hidhide: cannot read the device list: {e.Message}");
            return false;
        }
        if (wheel.Count == 0)
        {
            _log($"hidhide: no game controller with VID {vendorId} found; the real wheel stays visible to games");
            try
            {
                var seen = exit == 0 ? DescribeDevicesOf(json, vendorId) : new List<string>();
                _log($"hidhide: --dev-gaming exit {exit}, {json.Length} bytes; VID {vendorId} devices: "
                     + (seen.Count == 0 ? "none" : string.Join("; ", seen)));
            }
            catch (JsonException) { }
            return false;
        }

        var alreadyHidden = QuotedValues(Run("--dev-list").Output);
        foreach (var path in wheel)
        {
            if (alreadyHidden.Contains(path, StringComparer.OrdinalIgnoreCase)) continue;
            if (Run("--dev-hide", path).Exit == 0) _hiddenByUs.Add(path);
        }
        _cloakWasOff = Run("--cloak-state").Output.Contains("--cloak-off");
        Run("--cloak-on");
        _log($"hidhide: real wheel hidden from games ({string.Join(", ", wheel)})");
        LogState();
        return true;
    }

    // Entries of HidHide's hidden list matching `wanted` (case-insensitive), spelled exactly as HidHide stores
    // them: --dev-unhide only removes an exact match.
    public static List<string> StoredSpellings(IEnumerable<string> hiddenList, IEnumerable<string> wanted) =>
        hiddenList.Where(h => wanted.Contains(h, StringComparer.OrdinalIgnoreCase)).ToList();

    // Unhides `paths`, then reads the list back; returns the entries still hidden.
    List<string> Unhide(IReadOnlyCollection<string> paths)
    {
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            var still = StoredSpellings(QuotedValues(Run("--dev-list").Output), paths);
            if (still.Count == 0) return still;
            foreach (var path in still) Run("--dev-unhide", path);
        }
        return StoredSpellings(QuotedValues(Run("--dev-list").Output), paths);
    }

    // Undoes HideWheel: devices we hid become visible again, hiding goes back off if it was off.
    public void Restore()
    {
        if (_hiddenByUs.Count > 0)
        {
            var still = Unhide(_hiddenByUs);
            _log(still.Count == 0
                ? "hidhide: real wheel visible again"
                : $"hidhide: FAILED to unhide {string.Join(", ", still)}; run: wheelemu-bridge --unhide");
        }
        if (_cloakWasOff)
        {
            Run("--cloak-off");
            _log("hidhide: hiding switched off again (it was off before)");
        }
        _hiddenByUs.Clear();
        _cloakWasOff = false;
    }

    // After a crash: makes every hidden device of `vendorId` visible again.
    public void UnhideAll(string vendorId)
    {
        var ours = QuotedValues(Run("--dev-list").Output)
            .Where(p => p.Contains($"VID_{vendorId}", StringComparison.OrdinalIgnoreCase)).ToList();
        var still = Unhide(ours);
        _log(still.Count == 0
            ? $"hidhide: {ours.Count} device(s) of VID {vendorId} visible again"
            : $"hidhide: FAILED to unhide {string.Join(", ", still)}");
    }

    // Diagnostics for the log: cloak state and everything HidHide hides.
    public void LogState()
    {
        string cloak = Run("--cloak-state").Output.Trim();
        var hidden = QuotedValues(Run("--dev-list").Output);
        _log($"hidhide: {cloak}; hidden: {(hidden.Count == 0 ? "nothing" : string.Join(", ", hidden))}");
    }
}
