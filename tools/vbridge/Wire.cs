// The service's UDP wire formats, byte for byte as core/src/StatePacket.cpp and core/src/FfbPacket.cpp
// write and read them. Portable (no HIDMaestro), so SelfTest can check it on any OS.

using System.Buffers.Binary;

namespace Wheelemu.Bridge;

// What the emulated wheel reports: G920 ranges (steering 0..65535 centred at 32768, pedals 0..65535 with
// 65535 = released), buttons bit n = G920 button n, hat -1 = centred or 0..7 clockwise from up.
public readonly record struct TargetReport(int Steering, int Throttle, int Brake, int Clutch, uint Buttons, sbyte Hat);

public static class StatePacket
{
    public const uint Magic = 0x554D4557;   // "WEMU"
    public const ushort Version = 1;
    public const int Size = 33;

    // Layout (little-endian): 0 u32 magic, 4 u16 version, 6 u16 reserved, 8 u32 sequence,
    // 12 i32 steering, 16 i32 throttle, 20 i32 brake, 24 i32 clutch, 28 u32 buttons, 32 i8 hat.
    public static bool TryDecode(ReadOnlySpan<byte> bytes, out uint sequence, out TargetReport report)
    {
        sequence = 0;
        report = default;
        if (bytes.Length != Size || BinaryPrimitives.ReadUInt32LittleEndian(bytes) != Magic ||
            BinaryPrimitives.ReadUInt16LittleEndian(bytes[4..]) != Version)
        {
            return false;
        }
        sequence = BinaryPrimitives.ReadUInt32LittleEndian(bytes[8..]);
        report = new TargetReport(
            BinaryPrimitives.ReadInt32LittleEndian(bytes[12..]),
            BinaryPrimitives.ReadInt32LittleEndian(bytes[16..]),
            BinaryPrimitives.ReadInt32LittleEndian(bytes[20..]),
            BinaryPrimitives.ReadInt32LittleEndian(bytes[24..]),
            BinaryPrimitives.ReadUInt32LittleEndian(bytes[28..]),
            (sbyte)bytes[32]);
        return true;
    }

    // For SelfTest: the same bytes the service sends.
    public static byte[] Encode(uint sequence, TargetReport r)
    {
        var bytes = new byte[Size];
        BinaryPrimitives.WriteUInt32LittleEndian(bytes, Magic);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(4), Version);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(8), sequence);
        BinaryPrimitives.WriteInt32LittleEndian(bytes.AsSpan(12), r.Steering);
        BinaryPrimitives.WriteInt32LittleEndian(bytes.AsSpan(16), r.Throttle);
        BinaryPrimitives.WriteInt32LittleEndian(bytes.AsSpan(20), r.Brake);
        BinaryPrimitives.WriteInt32LittleEndian(bytes.AsSpan(24), r.Clutch);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(28), r.Buttons);
        bytes[32] = (byte)r.Hat;
        return bytes;
    }
}

// Test clutch, sent by ClutchProbe to wheelemu-service (UDP 127.0.0.1:41331) while no clutch pedal is
// connected; the service uses it instead of the mapped clutch (core/include/wheelemu/TestClutch.hpp). 12 bytes, little-endian: 0 u32 magic "WECL", 4 u16 version, 6 u16 reserved,
// 8 u16 pressed 0..10000 (0 = released, 10000 = fully pressed), 10 u16 reserved.
public static class ClutchPacket
{
    public const uint Magic = 0x4C434557;   // "WECL"
    public const ushort Version = 1;
    public const int Size = 12;
    public const int DefaultPort = 41331;

    public static byte[] Encode(float pressed)
    {
        var bytes = new byte[Size];
        BinaryPrimitives.WriteUInt32LittleEndian(bytes, Magic);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(4), Version);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(8), (ushort)Math.Round(Math.Clamp(pressed, 0f, 1f) * 10000));
        return bytes;
    }

    public static bool TryDecode(ReadOnlySpan<byte> bytes, out float pressed)
    {
        pressed = 0;
        if (bytes.Length != Size || BinaryPrimitives.ReadUInt32LittleEndian(bytes) != Magic ||
            BinaryPrimitives.ReadUInt16LittleEndian(bytes[4..]) != Version)
        {
            return false;
        }
        pressed = Math.Min(BinaryPrimitives.ReadUInt16LittleEndian(bytes[8..]), (ushort)10000) / 10000f;
        return true;
    }

    // Pressed share -> the G920 pedal range (65535 = released, 0 = fully pressed).
    public static int ToAxis(float pressed) => (int)Math.Round(65535 * (1 - Math.Clamp(pressed, 0f, 1f)));
}

// Same order as wheelemu::FfbEffectType.
public enum FfbEffectType : byte
{
    Constant, Spring, Damper, Friction, Inertia, Sine, Square, Triangle, SawtoothUp, SawtoothDown,
}

// wheelemu::FfbEffect: constant level -1..1 (sign as DirectInput means it), condition coefficients -1..1,
// saturations 0..1, periodic magnitude 0..1, gain 0..1.
public record struct FfbEffect
{
    public FfbEffectType Type;
    public float ConstantLevel;
    public float Center, Deadband, PositiveCoefficient, NegativeCoefficient;
    public float PositiveSaturation, NegativeSaturation;
    public float Magnitude, Offset, PeriodMs, PhaseDeg;
    public float Gain;

    public static FfbEffect Default => new() { PositiveSaturation = 1, NegativeSaturation = 1, Gain = 1 };
}

public enum FfbPacketKind : byte { SlotState = 0, KeepAlive = 1, StopAll = 2 }

public static class FfbPacket
{
    public const uint Magic = 0x42464557;   // "WEFB"
    public const ushort Version = 1;
    public const int Size = 64;

    // Layout (little-endian, 64 bytes): 0 u32 magic, 4 u16 version, 6 u8 kind, 7 u8 slot, 8 u32 sequence,
    // 12 u8 playing, 13 u8 type, 14 u16 reserved, 16 f32 gain, 20 f32 constant level,
    // 24 f32 x6 condition (center, deadband, +coef, -coef, +sat, -sat),
    // 48 f32 x4 periodic (magnitude, offset, period ms, phase deg).
    public static byte[] Encode(FfbPacketKind kind, byte slot, uint sequence, bool playing, in FfbEffect e)
    {
        var b = new byte[Size];
        var s = b.AsSpan();
        BinaryPrimitives.WriteUInt32LittleEndian(s, Magic);
        BinaryPrimitives.WriteUInt16LittleEndian(s[4..], Version);
        b[6] = (byte)kind;
        b[7] = slot;
        BinaryPrimitives.WriteUInt32LittleEndian(s[8..], sequence);
        b[12] = playing ? (byte)1 : (byte)0;
        b[13] = (byte)e.Type;
        float[] values =
        {
            e.Gain, e.ConstantLevel,
            e.Center, e.Deadband, e.PositiveCoefficient, e.NegativeCoefficient, e.PositiveSaturation,
            e.NegativeSaturation,
            e.Magnitude, e.Offset, e.PeriodMs, e.PhaseDeg,
        };
        for (int i = 0; i < values.Length; ++i)
        {
            BinaryPrimitives.WriteSingleLittleEndian(s[(16 + 4 * i)..], values[i]);
        }
        return b;
    }
}
