// Self-test of the bridge's portable parts: `dotnet run --project tools/vbridge/SelfTest`.
// Optional argument: a folder to write sample packets into, for the C++ cross-check.

using System.Buffers.Binary;
using Wheelemu.Bridge;

int failures = 0;
void Check(bool ok, string what)
{
    Console.WriteLine($"{(ok ? "ok  " : "FAIL")} {what}");
    if (!ok) ++failures;
}
bool Near(float a, float b) => Math.Abs(a - b) < 1e-3f;

// Decoded view of what the forwarder sent.
var sent = new List<(FfbPacketKind Kind, byte Slot, bool Playing, byte Type, float Level, float PosCoef, float PosSat, float Magnitude, float Gain)>();
void Capture(byte[] b)
{
    float F(int at) => BinaryPrimitives.ReadSingleLittleEndian(b.AsSpan(at));
    if (b.Length != FfbPacket.Size || BinaryPrimitives.ReadUInt32LittleEndian(b) != FfbPacket.Magic)
    {
        Check(false, "packet has size and magic");
    }
    sent.Add(((FfbPacketKind)b[6], b[7], b[12] != 0, b[13], F(20), F(32), F(40), F(48), F(16)));
}

// --- wire formats ---
var report = new TargetReport(40000, 1000, 65535, 65535, 0x405, 3);
Check(StatePacket.TryDecode(StatePacket.Encode(77, report), out uint seq, out var back) && seq == 77 && back == report,
      "state packet round trip");
Check(!StatePacket.TryDecode(new byte[33], out _, out _), "state packet without magic rejected");

// --- PID: NFS's constant force (infinite, X axis, no direction) ---
var logs = new List<string>();
var f = new PidForwarder(Capture, logs.Add);
f.Tick(0);
f.Tick(500);
Check(sent.Count == 0, "no keep-alives before a game has sent any force feedback");
byte[] SetEffect(byte ebi, byte type, ushort duration, byte axes, ushort direction = 0, byte gain = 255)
{
    var d = new byte[21];
    d[0] = ebi; d[1] = type;
    BinaryPrimitives.WriteUInt16LittleEndian(d.AsSpan(2), duration);
    d[10] = gain; d[12] = axes;
    BinaryPrimitives.WriteUInt16LittleEndian(d.AsSpan(13), direction);
    return d;
}
byte[] Constant(byte ebi, short magnitude)
{
    var d = new byte[3];
    d[0] = ebi;
    BinaryPrimitives.WriteInt16LittleEndian(d.AsSpan(1), magnitude);
    return d;
}

f.Handle(0x11, new byte[] { 1, 0 }, feature: true, 0);       // Create New Effect (driver allocates EBI 1)
f.Handle(PidForwarder.SetEffect, SetEffect(1, 1, 0xFFFF, 0x01), false, 0);
f.Handle(PidForwarder.SetConstant, Constant(1, 5000), false, 1);
sent.Clear();
f.Handle(PidForwarder.EffectOperation, new byte[] { 1, 1, 255 }, false, 2);
Check(sent.Count == 1 && sent[0] is { Kind: FfbPacketKind.SlotState, Slot: 0, Playing: true } && sent[0].Type == 0 &&
      Near(sent[0].Level, 0.5f) && Near(sent[0].Gain, 1f), "constant 5000 starts playing at +0.5 in slot 0");
sent.Clear();
f.Handle(PidForwarder.SetConstant, Constant(1, -9000), false, 30);
Check(sent.Count == 1 && Near(sent[0].Level, -0.9f), "a new magnitude is sent at once (-0.9)");
sent.Clear();
f.Handle(PidForwarder.SetConstant, Constant(1, -9000), false, 60);
Check(sent.Count == 0, "the same magnitude again sends nothing");

// --- spring on slot 1 (NFS: coefficient 0.9, saturation 0.41) ---
f.Handle(PidForwarder.SetEffect, SetEffect(2, 8, 0xFFFF, 0x01), false, 70);
var cond = new byte[14];
cond[0] = 2; cond[1] = 0;                                    // X block
BinaryPrimitives.WriteInt16LittleEndian(cond.AsSpan(4), 9000);
BinaryPrimitives.WriteInt16LittleEndian(cond.AsSpan(6), 9000);
BinaryPrimitives.WriteUInt16LittleEndian(cond.AsSpan(8), 4100);
BinaryPrimitives.WriteUInt16LittleEndian(cond.AsSpan(10), 4100);
f.Handle(PidForwarder.SetCondition, cond, false, 71);
cond[1] = 1; BinaryPrimitives.WriteInt16LittleEndian(cond.AsSpan(4), 1234);
f.Handle(PidForwarder.SetCondition, cond, false, 71);         // Y block: ignored
sent.Clear();
f.Handle(PidForwarder.EffectOperation, new byte[] { 2, 1, 255 }, false, 72);
Check(sent.Count == 1 && sent[0].Slot == 1 && sent[0].Type == 1 && Near(sent[0].PosCoef, 0.9f) && Near(sent[0].PosSat, 0.41f),
      "spring plays in slot 1 with coefficient 0.9, saturation 0.41 (Y block ignored)");

// --- square jolt: 150 ms, one loop, direction 270 degrees (-X) ---
f.Handle(PidForwarder.SetEffect, SetEffect(3, 3, 150, 0x05, direction: 24575), false, 100);
var periodic = new byte[11];
periodic[0] = 3;
BinaryPrimitives.WriteUInt16LittleEndian(periodic.AsSpan(1), 8100);
BinaryPrimitives.WriteUInt32LittleEndian(periodic.AsSpan(7), 75);
f.Handle(PidForwarder.SetPeriodic, periodic, false, 101);
sent.Clear();
f.Handle(PidForwarder.EffectOperation, new byte[] { 3, 1, 1 }, false, 102);
Check(sent.Count == 1 && sent[0].Slot == 2 && sent[0].Playing && sent[0].Type == 6 && Near(sent[0].Magnitude, 0.81f),
      "square plays in slot 2, magnitude 0.81 (direction only flips the phase)");
sent.Clear();
f.Tick(200);
Check(!sent.Any(p => p.Slot == 2 && p.Kind == FfbPacketKind.SlotState && !p.Playing), "still playing at 98 ms");
sent.Clear();
f.Tick(260);
Check(sent.Any(p => p.Slot == 2 && p.Kind == FfbPacketKind.SlotState && !p.Playing), "stops by itself after 150 ms");

// --- direction: 90 degrees = +X, 270 degrees = -X for a constant force ---
f.Handle(PidForwarder.SetEffect, SetEffect(4, 1, 0xFFFF, 0x05, direction: 8192), false, 300);
f.Handle(PidForwarder.SetConstant, Constant(4, 10000), false, 300);
sent.Clear();
f.Handle(PidForwarder.EffectOperation, new byte[] { 4, 1, 255 }, false, 301);
Check(sent.Count == 1 && Near(sent[0].Level, 1f), "direction 90 degrees: +1");
sent.Clear();
f.Handle(PidForwarder.SetEffect, SetEffect(4, 1, 0xFFFF, 0x05, direction: 24575), false, 302);
Check(sent.Count == 1 && Near(sent[0].Level, -1f), "direction 270 degrees: -1");

// --- device gain, block free, slot reuse ---
sent.Clear();
f.Handle(PidForwarder.DeviceGain, new byte[] { 128 }, false, 310);
Check(sent.All(p => !p.Playing || Near(p.Gain, 128 / 255f)) && sent.Count >= 3, "device gain applies to every playing effect");
sent.Clear();
f.Handle(PidForwarder.BlockFree, new byte[] { 1 }, false, 320);
Check(sent.Count == 1 && sent[0].Slot == 0 && !sent[0].Playing, "block free stops its slot");
f.Handle(PidForwarder.SetEffect, SetEffect(9, 1, 0xFFFF, 0x01), false, 330);
f.Handle(PidForwarder.SetConstant, Constant(9, 2000), false, 330);
sent.Clear();
f.Handle(PidForwarder.EffectOperation, new byte[] { 9, 1, 255 }, false, 331);
Check(sent.Count == 1 && sent[0].Slot == 0, "a new effect reuses the freed slot 0");

// --- keep-alive, refresh, reset ---
sent.Clear();
f.Tick(800);
Check(sent.Any(p => p.Kind == FfbPacketKind.SlotState && p.Playing), "refresh re-sends playing slots");
sent.Clear();
f.Tick(805);
f.Tick(1100);
Check(sent.Any(p => p.Kind == FfbPacketKind.KeepAlive) || sent.Any(p => p.Kind == FfbPacketKind.SlotState),
      "keep-alive or refresh at least every 250 ms");
sent.Clear();
f.Handle(PidForwarder.DeviceControl, new byte[] { 4 }, false, 1200);
Check(sent.Count == 1 && sent[0].Kind == FfbPacketKind.StopAll, "device reset sends StopAll");
Check(f.ActiveSlots == 0, "nothing plays after a reset");
Check(logs.Count(l => l.StartsWith("pid: first")) >= 8, "the first report of each kind is logged");

// --- HidHide output parsing (formats as HidHideCLI prints them) ---
const string gaming = """
 [ { "friendlyName" : "R3 Racing Wheel and Pedals" , "devices" : [
{ "present" : true ,
"gamingDevice" : true ,
"symbolicLink" : "\\?\\HID#VID_346E&PID_0005&MI_02#8&8052F69&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}" ,
"vendor" : "Gudsen" ,
"product" : "R3 Racing Wheel and Pedals" ,
"serialNumber" : "" ,
"usage" : "Joystick" ,
"description" : "HID-compliant game controller" ,
"deviceInstancePath" : "HID\\VID_346E&PID_0005&MI_02\\8&8052F69&0&0000" ,
"xusbDeviceInstancePath" : "" ,
"baseContainerDeviceInstancePath" : "USB\\VID_346E&PID_0005\\0001" ,
"baseContainerClassGuid" : "{36FC9E60-C465-11CF-8056-444553540000}" ,
"baseContainerDeviceCount" : 3 } ,
{ "present" : true ,
"gamingDevice" : false ,
"symbolicLink" : "x" , "vendor" : "Gudsen" , "product" : "R3" , "serialNumber" : "" , "usage" : "Vendor" ,
"description" : "HID-compliant vendor-defined device" ,
"deviceInstancePath" : "HID\\VID_346E&PID_0005&MI_00\\8&1&0&0000" ,
"xusbDeviceInstancePath" : "" , "baseContainerDeviceInstancePath" : "" , "baseContainerClassGuid" : "{00000000-0000-0000-0000-000000000000}" ,
"baseContainerDeviceCount" : 3 } ] } ,
{ "friendlyName" : "Logitech G920 Driving Force Racing Wheel USB" , "devices" : [
{ "present" : true , "gamingDevice" : true , "symbolicLink" : "y" , "vendor" : "Logitech" , "product" : "G920" ,
"serialNumber" : "" , "usage" : "Joystick" , "description" : "HID-compliant game controller" ,
"deviceInstancePath" : "HID\\VID_046D&PID_C262\\1&2&3" , "xusbDeviceInstancePath" : "" ,
"baseContainerDeviceInstancePath" : "" , "baseContainerClassGuid" : "{00000000-0000-0000-0000-000000000000}" ,
"baseContainerDeviceCount" : 1 } ] } ]
""";
var moza = HidHide.GamingDevicesOf(gaming, "346E");
Check(moza.Count == 1 && moza[0] == @"HID\VID_346E&PID_0005&MI_02\8&8052F69&0&0000",
      "only the R3's game-controller interface is chosen (vendor interface and the virtual G920 are not)");
Check(HidHide.GamingDevicesOf(gaming, "046D").Count == 1, "another vendor's device is found by its own VID");
var described = HidHide.DescribeDevicesOf(gaming, "346E");
Check(described.Count >= 1 && described.All(d => d.Contains("gamingDevice")), "every R3 interface is described for the log");
var listed = HidHide.QuotedValues("--dev-hide \"HID\\VID_346E&PID_0005&MI_02\\8&8052F69&0&0000\"\r\n--dev-hide \"HID\\X\"\r\n");
Check(listed.Count == 2 && listed[0] == @"HID\VID_346E&PID_0005&MI_02\8&8052F69&0&0000", "--dev-list lines are read");
var stored = HidHide.StoredSpellings(new[] { @"HID\VID_346E&PID_0005&MI_02\8&8052f69&0&0000", @"HID\X" },
                                     new[] { @"HID\VID_346E&PID_0005&MI_02\8&8052F69&0&0000" });
Check(stored.Count == 1 && stored[0] == @"HID\VID_346E&PID_0005&MI_02\8&8052f69&0&0000",
      "unhide uses HidHide's own spelling of the path (case differs)");

// --- PID as Windows' pid.dll really sends it (FfbProbe, 2026-09-30): parameters with EBI 0, then Set Effect ---
sent.Clear();
var g = new PidForwarder(Capture, logs.Add);
byte[] Hex(string h) => Convert.FromHexString(h);
g.Handle(0x15, Hex("00A00F"), false, 0);                                            // constant 4000, EBI 0
g.Handle(0x11, Hex("0101DC05000000000000FFFF04FF1F0000FFFFFFFF"), false, 0);       // Set Effect EBI 1, 90 degrees
g.Handle(0x1A, Hex("010101"), false, 1);                                            // start
Check(sent.Any(p => p.Playing && Near(p.Level, 0.4f)), "EBI-0 constant goes to the Set Effect that follows (+0.4)");
g.Handle(0x1B, Hex("01"), false, 1500);                                             // freed
sent.Clear();
g.Handle(0x15, Hex("0060F0"), false, 1510);                                         // constant -4000, EBI 0
g.Handle(0x11, Hex("0201DC05000000000000FFFF04FF1F0000FFFFFFFF"), false, 1510);
g.Handle(0x1A, Hex("020101"), false, 1511);
Check(sent.Any(p => p.Playing && Near(p.Level, -0.4f)), "the next effect gets its own magnitude (-0.4)");
sent.Clear();
g.Handle(0x15, Hex("00E0FA"), false, 1600);                                         // update while running: -1312
Check(sent.Any(p => p.Playing && Near(p.Level, -0.1312f)), "an EBI-0 update alone changes the running effect");
sent.Clear();
g.Handle(0x13, Hex("000F000070177017102710270000"), false, 1700);                  // spring 6000, sat 10000
g.Handle(0x11, Hex("0308B80B000000000000FFFF04FF1F0000FFFFFFFF"), false, 1700);
g.Handle(0x1A, Hex("030101"), false, 1701);
Check(sent.Any(p => p.Playing && p.Type == 1 && Near(p.PosCoef, 0.6f) && Near(p.PosSat, 1f)),
      "EBI-0 condition with block nibble 0xF is the wheel's spring (0.6)");

// --- test clutch (ClutchProbe -> bridge) ---
Check(ClutchPacket.TryDecode(ClutchPacket.Encode(0.25f), out float pressed) && Near(pressed, 0.25f), "clutch packet round trip (25 %)");
Check(!ClutchPacket.TryDecode(new byte[12], out _), "clutch packet without magic rejected");
Check(ClutchPacket.ToAxis(0) == 65535 && ClutchPacket.ToAxis(1) == 0 && ClutchPacket.ToAxis(0.5f) == 32768,
      "clutch released = 65535, pressed = 0, like the other pedals");

// --- device health: Kernel-PnP event 225 (as wevtutil /f:text prints it, 2026-09-30) ---
string events = """
Event[0]:
  Log Name: System
  Source: Microsoft-Windows-Kernel-PnP
  Event ID: 225
  Description:
The application \Device\HarddiskVolume9\Steam\steamapps\common\Need for Speed\NFS16.exe with process id 6048 stopped the removal or ejection for the device ROOT\HIDCLASS\HM_0000. Process command line: "D:\Steam\steamapps\common\Need for Speed\NFS16.exe" -Render.ResolutionScale 2.0

Event[1]:
  Description:
The application \Device\HarddiskVolume3\Program Files (x86)\Steam\steam.exe with process id 9280 stopped the removal or ejection for the device ROOT\HIDCLASS\HM_0000. Process command line: "C:\Program Files (x86)\Steam\steam.exe" -silent

Event[2]:
  Description:
The application \Device\HarddiskVolume3\Windows\explorer.exe with process id 1 stopped the removal or ejection for the device USBSTOR\DISK&VEN_X\123.
""";
var blockers = DeviceHealth.RemovalBlockers(events);
Check(blockers.SequenceEqual(new[] { "NFS16.exe", "steam.exe" }), "removal blockers of the virtual G920 are named (other devices ignored)");
Check(DeviceHealth.StuckAdvice(blockers).Contains("NFS16.exe, steam.exe had it open"), "stuck advice names them");

// --- samples for the C++ cross-check ---
if (args.Length == 1)
{
    var e = FfbEffect.Default;
    e.Type = FfbEffectType.Spring; e.PositiveCoefficient = 0.9f; e.NegativeCoefficient = 0.8f;
    e.PositiveSaturation = 0.41f; e.NegativeSaturation = 0.4f; e.Gain = 0.5f; e.ConstantLevel = -0.25f;
    e.Magnitude = 0.3f; e.Offset = -0.1f; e.PeriodMs = 75; e.PhaseDeg = 180; e.Center = 0.05f; e.Deadband = 0.02f;
    File.WriteAllBytes(Path.Combine(args[0], "ffb.bin"), FfbPacket.Encode(FfbPacketKind.SlotState, 5, 1234, true, e));
    File.WriteAllBytes(Path.Combine(args[0], "state.bin"), StatePacket.Encode(4242, new TargetReport(40000, 1000, 65535, 32768, 0x405, 3)));
}

Console.WriteLine(failures == 0 ? "all checks passed" : $"{failures} check(s) FAILED");
return failures == 0 ? 0 : 1;
