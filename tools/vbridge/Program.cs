// wheelemu-bridge: Phase 5, step 0 (see docs/phase5-windows-virtual-device.md).
//
// Creates a Windows-level virtual "Logitech G920" with HIDMaestro, feeds it the wheel state that
// wheelemu-service publishes (UDP 127.0.0.1:41329, the shim's input), and sends the game's HID PID force
// feedback back to the service as FfbPackets (UDP 127.0.0.1:41330, the shim's output). The service does
// not change. Use it INSTEAD of dinput8.dll in the game folder, never together (both want port 41329).
//
// While it runs it also hides the real wheel from games (HidHide, when installed) and names the G920's axes
// for DirectInput; both are undone on exit.
//
// Run from an Administrator terminal (HIDMaestro installs its user-mode driver on first run).

using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Net;
using System.Net.Sockets;
using System.Security.Principal;
using HIDMaestro;
using Microsoft.Win32;

namespace Wheelemu.Bridge;

internal static class Program
{
    // The G920's USB ids; --test-id swaps in pid.codes' test ids (1209:0001) to see whether Windows treats
    // Logitech's ids specially.
    static ushort G920Vid = 0x046D;
    static ushort G920Pid = 0xC262;
    const string G920Name = "Logitech G920 Driving Force Racing Wheel USB";   // what the shim reports
    const int StaleMs = 1500;                                                  // same as the shim
    static string OemKey => $@"SYSTEM\CurrentControlSet\Control\MediaProperties\PrivateProperties\Joystick\OEM\VID_{G920Vid:X4}&PID_{G920Pid:X4}";

    // DirectInput axis instance -> name (0 = X, 1 = Y, 5 = Rz, 6 = Slider): the layout NFS reads.
    static readonly (int Instance, string Name)[] AxisNames =
        { (0, "Wheel"), (1, "Brake"), (5, "Clutch"), (6, "Accelerator") };

    sealed class Options
    {
        public int StatePort = 41329, FfbPort = 41330;
        public bool DrivingType = true, Hide = true, UnhideOnly;
        public bool AxisNames;                              // experimental: made the device vanish from DirectInput
        public string WheelVendor = "346E";                 // Moza
        public string? ServiceExe, HidHideCli;
    }

    static readonly object LogLock = new();
    static readonly ManualResetEventSlim CleanedUp = new(false);
    static volatile bool _stop;

    static void OnWindowClosing(PosixSignalContext context)
    {
        context.Cancel = true;
        _stop = true;
        CleanedUp.Wait(TimeSpan.FromSeconds(4));
    }

    static int Main(string[] args)
    {
        var o = new Options();
        for (int i = 0; i < args.Length; ++i)
        {
            switch (args[i])
            {
                case "--state-port" when i + 1 < args.Length: o.StatePort = int.Parse(args[++i]); break;
                case "--ffb-port" when i + 1 < args.Length: o.FfbPort = int.Parse(args[++i]); break;
                case "--no-driving-type": o.DrivingType = false; break;
                case "--no-hide": o.Hide = false; break;
                case "--axis-names": o.AxisNames = true; break;
                case "--wheel-vid" when i + 1 < args.Length: o.WheelVendor = args[++i].ToUpperInvariant(); break;
                case "--service-exe" when i + 1 < args.Length: o.ServiceExe = args[++i]; break;
                case "--hidhide-cli" when i + 1 < args.Length: o.HidHideCli = args[++i]; break;
                case "--unhide": o.UnhideOnly = true; break;
                case "--test-id": G920Vid = 0x1209; G920Pid = 0x0001; break;
                default:
                    Console.WriteLine(
                        "usage: wheelemu-bridge [--state-port 41329] [--ffb-port 41330] [--no-driving-type]\n"
                        + "                       [--no-hide] [--wheel-vid 346E] [--service-exe PATH] [--hidhide-cli PATH]\n"
                        + "                       [--axis-names]   (experimental: Wheel/Brake/Clutch/Accelerator names)\n"
                        + "                       [--test-id]      (diagnostic: USB ids 1209:0001 instead of the G920's)\n"
                        + "       wheelemu-bridge --unhide   (after a crash: make the real wheel visible again)");
                    return 2;
            }
        }

        if (!new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator))
        {
            Log("run this from an Administrator terminal: HIDMaestro installs its driver and writes the "
                + "DirectInput name to the registry");
            return 1;
        }
        Console.CancelKeyPress += (_, e) => { e.Cancel = true; _stop = true; };
        // Closing the window (SIGHUP on Windows = CTRL_CLOSE_EVENT) or logging off: stop the same way and give
        // the cleanup (un-hiding the real wheel) the few seconds Windows allows.
        using var onClose = PosixSignalRegistration.Create(PosixSignal.SIGHUP, OnWindowClosing);
        using var onTerm = PosixSignalRegistration.Create(PosixSignal.SIGTERM, OnWindowClosing);
        Log($"wheelemu-bridge starting: state from 127.0.0.1:{o.StatePort}, force feedback to 127.0.0.1:{o.FfbPort}");

        try
        {
            if (o.UnhideOnly)
            {
                var hidHide = HidHide.Find(o.HidHideCli, Log);
                if (hidHide == null)
                {
                    Log("HidHide is not installed; nothing to do");
                    return 1;
                }
                hidHide.UnhideAll(o.WheelVendor);
                hidHide.LogState();
                SetDirectInputEntries(false, false, true);
                return 0;
            }
            try
            {
                return Run(o);
            }
            finally
            {
                CleanedUp.Set();
            }
        }
        catch (Exception e)
        {
            Log($"error: {e}");
            return 1;
        }
    }

    // A G920 as DirectInput sees the shim's: wheel X, brake Y, clutch Rz, throttle Slider (16-bit, pedals
    // released at the top), 11 buttons, a hat, plus HID PID force feedback (needs a Joystick collection).
    static HMProfile BuildProfile()
    {
        var descriptor = new HidDescriptorBuilder()
            .Joystick()
            .AddAxis(HMAxis.X, 16, -32768, 32767)
            .AddAxis(HMAxis.Y, 16, -32768, 32767)
            .AddAxis(HMAxis.Rz, 16, -32768, 32767)
            .AddAxis(HMAxis.Slider, 16, -32768, 32767)
            .AddButtons(11)                          // declared as 16 so the report stays byte-aligned
            .AddHat()
            .AddPidFfbBlock();
        return new HMProfileBuilder()
            .Id("wheelemu-g920")
            .Name("Logitech G920 (wheelemu)")
            .Vendor("Logitech")
            .Vid(G920Vid).Pid(G920Pid)
            .ProductString("G920 Driving Force Racing Wheel USB")
            .ManufacturerString("Logitech")
            .Type("wheel")
            .Connection("usb")
            .FromDescriptorBuilder(descriptor)
            .Build();
    }

    // Input report data (report ID added by HIDMaestro): X, Y, Rz, Slider as int16, 16 button bits, hat.
    static void Pack(in TargetReport r, byte[] report)
    {
        static void Axis(byte[] b, int at, int value)
        {
            short v = (short)Math.Clamp(value - 32768, short.MinValue, short.MaxValue);
            b[at] = (byte)v;
            b[at + 1] = (byte)(v >> 8);
        }
        Axis(report, 0, r.Steering);
        Axis(report, 2, r.Brake);
        Axis(report, 4, r.Clutch);
        Axis(report, 6, r.Throttle);
        report[8] = (byte)(r.Buttons & 0xFF);
        report[9] = (byte)((r.Buttons >> 8) & 0x07);   // 11 real buttons
        report[10] = r.Hat is >= 0 and <= 7 ? (byte)r.Hat : (byte)8;   // 8 = outside 0..7 = centred
    }

    static readonly TargetReport Neutral = new(32768, 65535, 65535, 65535, 0, -1);

    static int Run(Options o)
    {
        HMOemNameOverride.RecoverOrphans();          // labels left behind by a crashed earlier run
        using var ctx = new HMContext();
        Log("installing / checking the HIDMaestro driver...");
        ctx.InstallDriver();
        var profile = BuildProfile();
        HMOemNameOverride.Set(G920Vid, G920Pid, G920Name);
        using var ctrl = ctx.CreateController(profile);
        Log($"virtual '{G920Name}' created (VID {G920Vid:X4} PID {G920Pid:X4}); check it in joy.cpl");
        SetDirectInputEntries(false, false, true);   // leftovers of an earlier run first
        SetDirectInputEntries(true, o.DrivingType, o.AxisNames);
        RemoveForeignForceFeedbackDriver();
        HidHide? hidHide = o.Hide ? StartHiding(o) : null;
        using var ffbSocket = new UdpClient();
        ffbSocket.Connect(new IPEndPoint(IPAddress.Loopback, o.FfbPort));
        var clock = Stopwatch.StartNew();
        var forwarder = new PidForwarder(bytes => ffbSocket.Send(bytes, bytes.Length), Log);
        try
        {
            RunUntilStopped(o, ctrl, clock, forwarder);
        }
        finally
        {
            // Always, also after an error or when the window is closed: the real wheel must not stay hidden.
            Log($"stopping: {forwarder.ReportsHandled} force feedback reports from the game, "
                + $"{forwarder.PacketsSent} packets to the service");
            try { forwarder.StopAll(clock.ElapsedMilliseconds); } catch (Exception e) { Log($"stop all: {e.Message}"); }
            hidHide?.Restore();
            hidHide?.LogState();
            SetDirectInputEntries(false, o.DrivingType, o.AxisNames);
            HMOemNameOverride.Clear(G920Vid, G920Pid);
        }
        return 0;
    }

    static void RunUntilStopped(Options o, HMController ctrl, Stopwatch clock, PidForwarder forwarder)
    {
        int statePort = o.StatePort;

        // Force feedback: the game's PID reports, on HIDMaestro's thread.
        ctrl.OutputReceived += (_, packet) =>
        {
            if (packet.Source is HMOutputSource.HidOutput or HMOutputSource.HidFeature)
            {
                forwarder.Handle(packet.ReportId, packet.Data.Span, packet.Source == HMOutputSource.HidFeature,
                                 clock.ElapsedMilliseconds);
            }
        };
        ctrl.PublishPidPool(ramPoolSize: 0xFFFF, simultaneousEffectsMax: PidForwarder.MaxSlots,
                            deviceManagedPool: true, sharedParameterBlocks: false);
        ctrl.PublishPidState(0, PidStateFlags.ActuatorsEnabled | PidStateFlags.ActuatorPower);

        // Input: the service's state packets, on their own thread.
        var input = new Thread(() => InputLoop(statePort, ctrl)) { IsBackground = true, Name = "state input" };
        input.Start();

        Log("running; Ctrl+C to stop");
        long lastStatus = 0, lastDriverCheck = 0;
        (int, long) lastShown = (-1, -1);
        while (!_stop)
        {
            long now = clock.ElapsedMilliseconds;
            forwarder.Tick(now);
            if (now - lastDriverCheck >= 2000)
            {
                lastDriverCheck = now;
                RemoveForeignForceFeedbackDriver();   // Logitech G HUB writes its entry back when it sees a "G920"
            }
            if (now - lastStatus >= 10_000)
            {
                lastStatus = now;
                var shown = (forwarder.ActiveSlots, forwarder.ReportsHandled > 0 ? 1L : 0L);
                if (shown != lastShown)   // only when it changes, like the service's log
                {
                    lastShown = shown;
                    Log($"force feedback: {forwarder.ActiveSlots} effects playing, {forwarder.ReportsHandled} "
                        + $"reports from the game, {forwarder.PacketsSent} packets to the service");
                }
            }
            Thread.Sleep(5);
        }
    }

    static void InputLoop(int statePort, HMController ctrl)
    {
        using var socket = new UdpClient(new IPEndPoint(IPAddress.Loopback, statePort));
        socket.Client.ReceiveTimeout = 100;
        var report = new byte[11];
        var remote = new IPEndPoint(IPAddress.Any, 0);
        var sinceLast = Stopwatch.StartNew();
        bool receiving = false;
        bool neutralSent = false;
        Pack(Neutral, report);
        ctrl.SubmitRawReport(report);
        while (!_stop)
        {
            byte[]? datagram = null;
            try
            {
                datagram = socket.Receive(ref remote);
            }
            catch (SocketException e) when (e.SocketErrorCode == SocketError.TimedOut)
            {
            }
            if (datagram != null && StatePacket.TryDecode(datagram, out uint sequence, out var state))
            {
                if (!receiving)
                {
                    receiving = true;
                    Log($"receiving wheel state from wheelemu-service (packet #{sequence})");
                }
                sinceLast.Restart();
                neutralSent = false;
                Pack(state, report);
                ctrl.SubmitRawReport(report);
            }
            else if (sinceLast.ElapsedMilliseconds > StaleMs && !neutralSent)
            {
                // Same rule as the shim: hold the last input for 1.5 s, then centred wheel, released pedals.
                if (receiving)
                {
                    Log($"no wheel state for {StaleMs} ms: centred wheel and released pedals until it comes back");
                }
                receiving = false;
                neutralSent = true;
                Pack(Neutral, report);
                ctrl.SubmitRawReport(report);
            }
        }
    }

    // Hides the real wheel from games, keeping the running service allowed to see it. Null when HidHide is not
    // installed or the service is not running (then nothing is hidden: the service would lose the wheel).
    static HidHide? StartHiding(Options o)
    {
        var hidHide = HidHide.Find(o.HidHideCli, Log);
        if (hidHide == null)
        {
            Log("HidHide is not installed: games see the real wheel too (install it, or ignore with --no-hide)");
            return null;
        }
        string? service = o.ServiceExe;
        // The service may be started just after the bridge: wait for it rather than giving up at once.
        for (int waited = 0; service == null; ++waited)
        {
            var running = Process.GetProcessesByName("wheelemu-service").FirstOrDefault();
            try { service = running?.MainModule?.FileName; } catch (Exception) { service = null; }
            if (service != null) break;
            if (waited == 0) Log("waiting for wheelemu-service to start (up to 60 s) before hiding the real wheel");
            if (waited >= 60) break;
            Thread.Sleep(1000);
        }
        if (service == null || !File.Exists(service))
        {
            Log("wheelemu-service is not running: start it first, or pass --service-exe; the real wheel stays visible");
            return null;
        }
        return hidHide.HideWheel(o.WheelVendor, service) ? hidHide : null;
    }

    // Windows' own HID PID force feedback driver (pid.dll): the one that talks to our PID reports.
    const string PidDriverClsid = "{EEC6993A-B3FD-11D2-A916-00C04FB98638}";

    // DirectInput takes the force feedback driver from OEM\VID_046D&PID_C262\OEMForceFeedback (HKCU first).
    // Logitech G HUB puts its own driver there for every G920, and that driver only talks to a real one
    // (HID++): every effect fails with E_NOTIMPL. Removing the entry lets DirectInput recreate it with
    // pid.dll, as it does for any HID PID device. Checked again while running: G HUB writes it back.
    static void RemoveForeignForceFeedbackDriver()
    {
        foreach (var root in new[] { Registry.CurrentUser, Registry.LocalMachine })
        {
            try
            {
                using var ffb = root.OpenSubKey($@"{OemKey}\OEMForceFeedback");
                string? clsid = ffb?.GetValue("CLSID") as string;
                if (ffb == null || string.Equals(clsid, PidDriverClsid, StringComparison.OrdinalIgnoreCase)) continue;
                ffb.Dispose();
                using var oem = root.OpenSubKey(OemKey, writable: true);
                oem?.DeleteSubKeyTree("OEMForceFeedback", false);
                Log($"DirectInput: removed another force feedback driver ({clsid ?? "no CLSID"}) from {root.Name}; "
                    + "Windows' HID PID driver takes over");
            }
            catch (Exception e)
            {
                Log($"cannot check the force feedback driver entry under {root.Name}: {e.Message}");
            }
        }
    }

    // DirectInput reads a joystick's name, type and axis names from its OEM entry (HKCU first, then HKLM):
    //  - OEMData JOY_HWS_ISCARCTRL (0x40) | HASPOV (0x02), 11 buttons: a driving device, as the shim reports it;
    //  - Axes\<instance> default value: the axis names games and joy.cpl show. Only with --axis-names: written
    //    without an "Attributes" value, they made DirectInput drop the whole device (joy.cpl listed nothing).
    // HIDMaestro writes OEMName (and clears the rest) when it creates the device, so this runs after it and is
    // removed again on exit.
    static void SetDirectInputEntries(bool on, bool drivingType, bool axisNames)
    {
        foreach (var root in new[] { Registry.LocalMachine, Registry.CurrentUser })
        {
            try
            {
                using var key = root.CreateSubKey(OemKey);
                if (key == null) continue;
                if (on)
                {
                    if (drivingType)
                    {
                        key.SetValue("OEMData", new byte[] { 0x42, 0, 0, 0, 11, 0, 0, 0 }, RegistryValueKind.Binary);
                    }
                    foreach (var (instance, name) in axisNames ? AxisNames : Array.Empty<(int, string)>())
                    {
                        using var axis = key.CreateSubKey($@"Axes\{instance}");
                        axis?.SetValue("", name, RegistryValueKind.String);
                    }
                }
                else
                {
                    key.DeleteValue("OEMData", false);
                    key.DeleteSubKeyTree("Axes", false);   // always: also clears names an earlier run left
                }
            }
            catch (Exception e)
            {
                Log($"cannot {(on ? "write" : "remove")} the DirectInput entry under {root.Name}: {e.Message}");
            }
        }
        if (on)
        {
            Log("DirectInput: " + (drivingType ? "driving device" : "standard device")
                + (axisNames ? ", axes named " + string.Join(", ", AxisNames.Select(a => a.Name)) : "")
                + " (reopen joy.cpl to see it)");
        }
    }

    // Console and %LOCALAPPDATA%\wheelemu\logs\wheelemu-bridge-YYYY-MM-DD.log (cleaned by the service's
    // [retention] like its other files).
    static void Log(string message)
    {
        string line = $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff}] {message}";
        lock (LogLock)
        {
            Console.WriteLine(line);
            try
            {
                string folder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                                             "wheelemu", "logs");
                Directory.CreateDirectory(folder);
                File.AppendAllText(Path.Combine(folder, $"wheelemu-bridge-{DateTime.Now:yyyy-MM-dd}.log"),
                                   line + Environment.NewLine);
            }
            catch
            {
                // the console still has it
            }
        }
    }
}
