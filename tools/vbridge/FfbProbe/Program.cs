// Plays force feedback on the virtual G920 through DirectInput, as a game would, and reports every step.
// Tells apart "Windows' FFB path to the bridge does not work" from "the game does not send FFB to this device".
// Run with the service and the bridge running and the game closed (the probe takes the device exclusively).
// Output goes to the console and to %LOCALAPPDATA%\wheelemu\logs\ffbprobe-<time>.txt.

using System.Runtime.InteropServices;
using SharpDX.DirectInput;

static class Probe
{
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern IntPtr CreateWindowExW(int exStyle, string cls, string name, int style, int x, int y, int w, int h,
                                         IntPtr parent, IntPtr menu, IntPtr instance, IntPtr param);

    static StreamWriter? _file;

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
        string path = Path.Combine(dir, $"ffbprobe-{DateTime.Now:yyyyMMdd-HHmmss}.txt");
        _file = new StreamWriter(path);
        try { return Run(args.Length > 0 ? args[0] : "G920"); }
        catch (Exception e) { Say($"FAILED: {e.GetType().Name}: {e.Message}"); return 1; }
        finally { Say($"written: {path}"); _file.Dispose(); }
    }

    static Effect? TryCreate(Joystick joy, Guid type, string name, EffectParameters p)
    {
        try
        {
            var e = new Effect(joy, type, p);
            Say($"  create {name}: OK");
            return e;
        }
        catch (Exception ex)
        {
            Say($"  create {name}: FAILED {ex.Message}");
            return null;
        }
    }

    static void Play(Effect? e, string name, int ms)
    {
        if (e == null) return;
        try
        {
            e.Start(1);
            Say($"  play {name} for {ms} ms: started");
            Thread.Sleep(ms);
            e.Stop();
        }
        catch (Exception ex) { Say($"  play {name}: FAILED {ex.Message}"); }
    }

    static int Run(string match)
    {
        Say($"wheelemu-ffbprobe {DateTime.Now:yyyy-MM-dd HH:mm:ss}");
        using var di = new DirectInput();
        var all = di.GetDevices(DeviceClass.GameControl, DeviceEnumerationFlags.AttachedOnly);
        var ffb = di.GetDevices(DeviceClass.GameControl, DeviceEnumerationFlags.ForceFeedback | DeviceEnumerationFlags.AttachedOnly);
        Say("game controllers (DirectInput):");
        foreach (var d in all)
        {
            bool hasFfb = ffb.Any(f => f.InstanceGuid == d.InstanceGuid);
            Say($"  {d.InstanceName} | product {d.ProductGuid} | type {d.Type}/{d.Subtype}{(hasFfb ? " | FFB" : " | no FFB")}");
        }

        var target = all.FirstOrDefault(d => d.InstanceName.Contains(match, StringComparison.OrdinalIgnoreCase));
        if (target == null) { Say($"no controller named *{match}*"); return 2; }
        if (!ffb.Any(f => f.InstanceGuid == target.InstanceGuid))
        {
            Say("DirectInput does not offer force feedback for it: games cannot send any (the PID part is not seen)");
            return 3;
        }

        using var joy = new Joystick(di, target.InstanceGuid);
        var hwnd = CreateWindowExW(0, "Static", "wheelemu-ffbprobe", 0, 0, 0, 0, 0, new IntPtr(-3), IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
        joy.SetCooperativeLevel(hwnd, CooperativeLevel.Exclusive | CooperativeLevel.Background);
        try { joy.Properties.AutoCenter = false; Say("auto-centre off: OK"); }   // before Acquire, as games do
        catch (Exception ex) { Say($"auto-centre off: FAILED {ex.Message}"); }
        joy.Acquire();
        Say($"acquired '{target.InstanceName}' exclusively; flags {joy.Capabilities.Flags}, {joy.Capabilities.AxeCount} axes, {joy.Capabilities.ButtonCount} buttons");

        Say("effects offered: " + string.Join(", ", joy.GetEffects().Select(e => e.Name)));
        foreach (var o in joy.GetObjects(DeviceObjectTypeFlags.Axis))
            Say($"  axis {o.Name} offset {o.Offset} type {o.ObjectId.Flags} usage {o.UsagePage:X}/{o.Usage:X}");
        var actuators = joy.GetObjects(DeviceObjectTypeFlags.ForceFeedbackActuator);
        Say("actuators: " + (actuators.Count == 0 ? "none" : string.Join(", ", actuators.Select(a => $"{a.Name} (offset {a.Offset})"))));
        int axis = actuators.Count > 0 ? actuators[0].Offset : 0;   // offset 0 = X, the wheel

        try { joy.SendForceFeedbackCommand(ForceFeedbackCommand.Reset); Say("reset: OK"); }
        catch (Exception ex) { Say($"reset: FAILED {ex.Message}"); }
        try { joy.SendForceFeedbackCommand(ForceFeedbackCommand.SetActuatorsOn); Say("actuators on: OK"); }
        catch (Exception ex) { Say($"actuators on: FAILED {ex.Message}"); }

        EffectParameters Params(TypeSpecificParameters specific, int durationUs) => new()
        {
            Flags = EffectFlags.Cartesian | EffectFlags.ObjectOffsets,
            Duration = durationUs,
            Gain = 10000,
            SamplePeriod = 0,
            StartDelay = 0,
            TriggerButton = -1,
            TriggerRepeatInterval = 0,
            Axes = new[] { axis },
            Directions = new[] { 1 },
            Parameters = specific,
        };

        Say("the wheel should: pull right, pull left, vibrate, then resist turning (spring)");
        using (var right = TryCreate(joy, EffectGuid.ConstantForce, "constant +40%", Params(new ConstantForce { Magnitude = 4000 }, 1_500_000)))
            Play(right, "constant +40%", 1500);
        using (var left = TryCreate(joy, EffectGuid.ConstantForce, "constant -40%", Params(new ConstantForce { Magnitude = -4000 }, 1_500_000)))
            Play(left, "constant -40%", 1500);
        using (var sine = TryCreate(joy, EffectGuid.Sine, "sine 30% 100 ms",
                   Params(new PeriodicForce { Magnitude = 3000, Offset = 0, Phase = 0, Period = 100_000 }, 1_500_000)))
            Play(sine, "sine", 1500);
        var spring = new ConditionSet
        {
            Conditions = new[] { new Condition { Offset = 0, PositiveCoefficient = 6000, NegativeCoefficient = 6000,
                                                 PositiveSaturation = 10000, NegativeSaturation = 10000, DeadBand = 0 } },
        };
        using (var s = TryCreate(joy, EffectGuid.Spring, "spring 60%", Params(spring, 3_000_000)))
            Play(s, "spring (turn the wheel)", 3000);

        try { joy.SendForceFeedbackCommand(ForceFeedbackCommand.StopAll); } catch (Exception) { }
        joy.Unacquire();
        Say("done: the bridge log should now show PID reports, the service log force feedback from the game");
        return 0;
    }
}
