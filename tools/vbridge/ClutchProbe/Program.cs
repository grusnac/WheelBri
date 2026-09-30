// A test clutch while no clutch pedal is connected. Sends the clutch position to wheelemu-service
// (UDP 127.0.0.1:41331, see ClutchPacket), which uses it instead of the mapped clutch, and reads the G920's
// clutch axis (Rz) back through DirectInput, as a game would. The service must be running; the check needs a
// G920 in DirectInput (the bridge); the game may be running too (reading is shared).
//
//   steps               0, 25, 50, 75, 100 %, 1 s each, each checked against what DirectInput reads (default)
//   sweep [times]       released -> pressed -> released over 5 s, `times` times (default 2); watch joy.cpl
//   hold <percent> [s]  hold one position (default 30 s), e.g. to try the clutch in a game
//
// When it ends (or Ctrl+C) the clutch goes back to released, and 1.5 s later to the service's clutch.
// Output goes to the console and to %LOCALAPPDATA%\wheelemu\logs\clutchprobe-<time>.txt.

using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using SharpDX.DirectInput;
using Wheelemu.Bridge;

static class ClutchProbe
{
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern IntPtr CreateWindowExW(int exStyle, string cls, string name, int style, int x, int y, int w, int h,
                                         IntPtr parent, IntPtr menu, IntPtr instance, IntPtr param);

    const int SendEveryMs = 10;
    const float Tolerance = 0.02f;   // 2 % of the travel

    static StreamWriter? _file;
    static UdpClient? _socket;
    static Joystick? _joy;
    static volatile bool _cancel;

    static void Say(string text)
    {
        Console.WriteLine(text);
        _file?.WriteLine(text);
        _file?.Flush();
    }

    static int Main(string[] args)
    {
        string dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "wheelemu", "logs");
        Directory.CreateDirectory(dir);
        string path = Path.Combine(dir, $"clutchprobe-{DateTime.Now:yyyyMMdd-HHmmss}.txt");
        _file = new StreamWriter(path);
        Console.CancelKeyPress += (_, e) => { e.Cancel = true; _cancel = true; };
        try
        {
            _socket = new UdpClient();
            _socket.Connect(new IPEndPoint(IPAddress.Loopback, ClutchPacket.DefaultPort));
            Say($"wheelemu-clutchprobe {DateTime.Now:yyyy-MM-dd HH:mm:ss}: test clutch to 127.0.0.1:{ClutchPacket.DefaultPort}");
            using var di = new DirectInput();
            OpenG920(di);
            string mode = args.Length > 0 ? args[0].ToLowerInvariant() : "steps";
            return mode switch
            {
                "steps" => Steps(),
                "sweep" => Sweep(args.Length > 1 ? int.Parse(args[1]) : 2),
                "hold" when args.Length > 1 => Hold(float.Parse(args[1]) / 100f, args.Length > 2 ? int.Parse(args[2]) : 30),
                _ => Usage(),
            };
        }
        catch (Exception e)
        {
            Say($"FAILED: {e.GetType().Name}: {e.Message}");
            return 1;
        }
        finally
        {
            Release();
            _joy?.Dispose();
            _socket?.Dispose();
            Say($"written: {path}");
            _file.Dispose();
        }
    }

    static int Usage()
    {
        Say("usage: wheelemu-clutchprobe [steps | sweep [times] | hold <percent> [seconds]]");
        return 2;
    }

    // Non-exclusive, background: works next to a game that holds the G920 exclusively.
    static void OpenG920(DirectInput di)
    {
        var all = di.GetDevices(DeviceClass.GameControl, DeviceEnumerationFlags.AttachedOnly);
        var target = all.FirstOrDefault(d => d.InstanceName.Contains("G920", StringComparison.OrdinalIgnoreCase));
        if (target == null)
        {
            Say("no G920 in DirectInput: the clutch is still sent, but cannot be checked. Controllers DirectInput sees: "
                + (all.Count == 0 ? "none" : string.Join(", ", all.Select(d => d.InstanceName))));
            Say("(is the service running, and does joy.cpl list the G920 (bridge)? If Steam held the old device, quit Steam and restart the bridge)");
            return;
        }
        _joy = new Joystick(di, target.InstanceGuid);
        var hwnd = CreateWindowExW(0, "Static", "wheelemu-clutchprobe", 0, 0, 0, 0, 0, new IntPtr(-3), IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
        _joy.SetCooperativeLevel(hwnd, CooperativeLevel.NonExclusive | CooperativeLevel.Background);
        _joy.Properties.Range = new InputRange(0, 65535);
        _joy.Acquire();
        Say($"reading '{target.InstanceName}' clutch (Rz) through DirectInput");
    }

    // Pressed share (0..1) DirectInput reports, or null when it cannot be read.
    static float? Seen()
    {
        if (_joy == null) return null;
        try
        {
            _joy.Poll();
            return 1f - _joy.GetCurrentState().RotationZ / 65535f;
        }
        catch (SharpDX.SharpDXException)
        {
            return null;
        }
    }

    static void Send(float pressed) => _socket!.Send(ClutchPacket.Encode(pressed));

    static void Release()
    {
        if (_socket == null) return;
        for (int i = 0; i < 20; ++i) { Send(0); Thread.Sleep(SendEveryMs); }
        Say("clutch released; the service's clutch takes over again in 1.5 s");
    }

    // Keeps sending `position(t)` (t = seconds since start) for `seconds`; calls `report` every `reportMs`.
    static void Drive(Func<double, float> position, double seconds, int reportMs, Action<double, float>? report = null)
    {
        var clock = Stopwatch.StartNew();
        long nextReport = 0;
        while (!_cancel && clock.Elapsed.TotalSeconds < seconds)
        {
            double t = clock.Elapsed.TotalSeconds;
            float p = position(t);
            Send(p);
            if (report != null && clock.ElapsedMilliseconds >= nextReport)
            {
                nextReport += reportMs;
                report(t, p);
            }
            Thread.Sleep(SendEveryMs);
        }
    }

    static string Show(float? seen) => seen is float s ? $"{s * 100,5:0.0} %" : "  n/a";

    static int Steps()
    {
        Say("steps: each position is held 1 s and checked after 0.5 s");
        int failed = 0, checkedCount = 0;
        foreach (float target in new[] { 0f, 0.25f, 0.5f, 0.75f, 1f, 0f })
        {
            float? seen = null;
            Drive(_ => target, 1.0, 500, (t, _) => { if (t >= 0.45) seen = Seen(); });
            if (_cancel) break;
            bool ok = seen is float s && Math.Abs(s - target) <= Tolerance;
            if (seen != null) ++checkedCount;
            if (!ok && seen != null) ++failed;
            Say($"  sent {target * 100,5:0.0} %   DirectInput reads {Show(seen)}   {(seen == null ? "not checked" : ok ? "OK" : "WRONG")}");
        }
        Say(checkedCount == 0 ? "result: nothing checked (DirectInput could not read the G920)"
            : failed == 0 ? "result: the clutch axis follows the test clutch" : $"result: {failed} position(s) wrong");
        return checkedCount > 0 && failed == 0 ? 0 : 1;
    }

    static int Sweep(int times)
    {
        Say($"sweep: released -> pressed -> released over 5 s, {times} times (open joy.cpl to watch the clutch bar)");
        for (int i = 0; i < times && !_cancel; ++i)
        {
            Drive(t => t < 2 ? (float)(t / 2) : t < 3 ? 1f : (float)Math.Max(0, 1 - (t - 3) / 2), 5.0, 500,
                  (t, p) => Say($"  {t,4:0.0} s  sent {p * 100,5:0.0} %   DirectInput reads {Show(Seen())}"));
        }
        return 0;
    }

    static int Hold(float pressed, int seconds)
    {
        pressed = Math.Clamp(pressed, 0f, 1f);
        Say($"hold: clutch at {pressed * 100:0} % for {seconds} s (Ctrl+C ends it early)");
        Drive(_ => pressed, seconds, 5000, (t, _) => Say($"  {t,4:0} s  DirectInput reads {Show(Seen())}"));
        return 0;
    }
}
