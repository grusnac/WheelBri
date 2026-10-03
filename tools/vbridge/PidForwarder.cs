// HID PID force feedback (the reports Windows' pid.dll writes for DirectInput effects) -> the service's
// FfbPackets. The PID counterpart of the shim's EmulatedEffect + FfbForwarder. Portable (no HIDMaestro),
// so SelfTest can check it on any OS.
//
// Report layouts are those of HIDMaestro's HidDescriptorBuilder.AddPidFfbBlock (data bytes, report ID
// removed). Effect blocks (EBI 1..40) are allocated by the driver on "Create New Effect"; the first report
// that names an EBI gives it one of the service's 8 slots, "Block Free" gives the slot back.
//
// Windows' pid.dll writes the type-specific reports (constant, ramp, periodic, condition) with effect block
// index 0 for this descriptor, just before the Set Effect that names the real block. Those are kept as
// pending and given to the next Set Effect of a matching type; alone (a game changing only the magnitude of
// a running effect) they go to the last block of that type.

using System.Buffers.Binary;

namespace Wheelemu.Bridge;

public sealed class PidForwarder
{
    public const int MaxSlots = 8;                    // same as the service (IFfbSink::kMaxSlots)
    const long KeepAliveMs = 100;
    const long RefreshMs = 250;
    // As core's HidPid::kGameQuietMs: a game that crashed sends nothing more, and the bridge cannot tell. After this
    // long without a report while a pushing force (constant, ramp, periodic) plays, keep-alives and refreshes stop,
    // so the service's watchdog stops the forces; the game's next report brings them back. Springs and dampers
    // alone keep going (they only resist, and games set them once).
    public const long GameQuietMs = 10_000;

    // Report IDs of the PID block.
    public const byte SetEffect = 0x11, SetEnvelope = 0x12, SetCondition = 0x13, SetPeriodic = 0x14,
        SetConstant = 0x15, SetRamp = 0x16, EffectOperation = 0x1A, BlockFree = 0x1B, DeviceControl = 0x1C,
        DeviceGain = 0x1D;

    sealed class Block
    {
        public int Slot = -1;
        public int PidType;                          // 1..12 in the descriptor's order, 0 = not set yet
        public long? DurationMs;                     // null = infinite
        public long StartDelayMs;
        public float Gain = 1;
        public float Along = 1;                      // share of the force along the wheel axis (-1..1)
        public float ConstantMagnitude;              // -1..1
        public float RampStart, RampEnd;
        public float Center, Deadband, PosCoef, NegCoef, PosSat = 1, NegSat = 1;
        public float Magnitude, Offset, PeriodMs, PhaseDeg;
        public bool Started;
        public long StartedAtMs;
        public int? Loops;                           // null = infinite
    }

    readonly Action<byte[]> _send;
    readonly Action<string> _log;
    readonly object _lock = new();
    readonly Dictionary<byte, Block> _blocks = new();
    readonly byte?[] _slotOwner = new byte?[MaxSlots];
    readonly (bool Playing, FfbEffect Effect)?[] _sent = new (bool, FfbEffect)?[MaxSlots];
    readonly HashSet<string> _seen = new();       // first report of each kind is logged in full
    uint _sequence;
    long _lastReportMs;
    bool _quiet;
    long _lastSendMs = long.MinValue / 2;
    long _lastRefreshMs = long.MinValue / 2;
    float _deviceGain = 1;
    bool _actuatorsOn = true;
    bool _paused;
    readonly Dictionary<ParamKind, BlockAction> _pending = new();    // EBI-0 parameters for the next Set Effect
    readonly Dictionary<ParamKind, byte> _lastOfKind = new();          // last block set up with each kind
    bool _loggedEbiZero;

    enum ParamKind { Constant, Ramp, Periodic, Condition }

    static ParamKind? KindOf(int pidType) => pidType switch
    {
        1 => ParamKind.Constant,
        2 => ParamKind.Ramp,
        >= 3 and <= 7 => ParamKind.Periodic,
        >= 8 and <= 11 => ParamKind.Condition,
        _ => null,
    };

    public long ReportsHandled { get; private set; }
    public long PacketsSent { get; private set; }

    public PidForwarder(Action<byte[]> send, Action<string> log)
    {
        _send = send;
        _log = log;
    }

    // PID effect type (1-based, AddPidFfbBlock order: constant, ramp, square, sine, triangle, saw up, saw down,
    // spring, damper, inertia, friction, custom) -> the service's type. Null = not supported.
    static FfbEffectType? TypeFor(int pidType) => pidType switch
    {
        1 or 2 => FfbEffectType.Constant,           // ramp is played as a constant at its mean level
        3 => FfbEffectType.Square,
        4 => FfbEffectType.Sine,
        5 => FfbEffectType.Triangle,
        6 => FfbEffectType.SawtoothUp,
        7 => FfbEffectType.SawtoothDown,
        8 => FfbEffectType.Spring,
        9 => FfbEffectType.Damper,
        10 => FfbEffectType.Inertia,
        11 => FfbEffectType.Friction,
        _ => null,
    };

    // One output (or feature) report from the game, without its report ID.
    public void Handle(byte reportId, ReadOnlySpan<byte> d, bool feature, long nowMs)
    {
        lock (_lock)
        {
            ++ReportsHandled;
            _lastReportMs = nowMs;
            if (_quiet)
            {
                _quiet = false;
                _log("pid: the game sends force feedback again: forces back on");
            }
            string kind = (feature ? "feature 0x" : "output 0x") + reportId.ToString("X2");
            if (_seen.Add(kind))
            {
                _log($"pid: first {kind}: {Convert.ToHexString(d)}");
            }
            if (feature)
            {
                return;                              // Create New Effect: the driver allocates the EBI itself
            }
            switch (reportId)
            {
                case SetEffect when d.Length >= 13: OnSetEffect(d); break;
                case SetEnvelope: break;             // attack/fade: ignored, as by the shim
                case SetCondition when d.Length >= 14: OnSetCondition(d); break;
                case SetPeriodic when d.Length >= 11: OnSetPeriodic(d); break;
                case SetConstant when d.Length >= 3:
                {
                    float magnitude = I16(d, 1) / 10000f;
                    WithParams(d[0], ParamKind.Constant, b => b.ConstantMagnitude = magnitude);
                    break;
                }
                case SetRamp when d.Length >= 5:
                {
                    float start = I16(d, 1) / 10000f, end = I16(d, 3) / 10000f;
                    WithParams(d[0], ParamKind.Ramp, b => { b.RampStart = start; b.RampEnd = end; });
                    break;
                }
                case EffectOperation when d.Length >= 3: OnEffectOperation(d, nowMs); break;
                case BlockFree when d.Length >= 1: OnBlockFree(d[0], nowMs); break;
                case DeviceControl when d.Length >= 1: OnDeviceControl(d[0], nowMs); break;
                case DeviceGain when d.Length >= 1: _deviceGain = d[0] / 255f; break;
            }
            SendChanges(nowMs, force: false);
        }
    }

    // Call regularly (every ~10 ms): effects that ran out, keep-alive, periodic refresh.
    public void Tick(long nowMs)
    {
        lock (_lock)
        {
            if (!_quiet && ReportsHandled > 0 && nowMs - _lastReportMs >= GameQuietMs && PushingForcePlays())
            {
                _quiet = true;
                _log($"pid: WARNING: no force feedback report from the game for {GameQuietMs / 1000} s while a pushing "
                     + "force plays (did the game crash?): keep-alives stopped, so the service stops the forces; they "
                     + "come back with the game's next report");
            }
            bool refresh = !_quiet && nowMs - _lastRefreshMs >= RefreshMs;
            if (refresh)
            {
                _lastRefreshMs = nowMs;
            }
            SendChanges(nowMs, refresh);
            // Keep-alives only once a game has used force feedback: otherwise the service would report a game
            // that is not there.
            if (!_quiet && ReportsHandled > 0 && nowMs - _lastSendMs >= KeepAliveMs)
            {
                Send(FfbPacketKind.KeepAlive, 0, false, FfbEffect.Default, nowMs);
            }
        }
    }

    public bool GameQuiet
    {
        get { lock (_lock) return _quiet; }
    }

    // A force that pushes the wheel by itself (not a spring, damper, friction or inertia), above 2 %.
    bool PushingForcePlays()
    {
        foreach (var s in _sent)
        {
            if (s is not { Playing: true } sent) continue;
            var e = sent.Effect;
            float strength = e.Type switch
            {
                FfbEffectType.Spring or FfbEffectType.Damper or FfbEffectType.Friction or FfbEffectType.Inertia => 0,
                FfbEffectType.Constant => Math.Abs(e.ConstantLevel),
                _ => Math.Abs(e.Magnitude) + Math.Abs(e.Offset),
            };
            if (strength * e.Gain > 0.02f) return true;
        }
        return false;
    }

    // Everything stops (bridge exiting, or the game reset its force feedback).
    public void StopAll(long nowMs)
    {
        lock (_lock)
        {
            foreach (var b in _blocks.Values) b.Started = false;
            Array.Fill(_sent, null);
            Send(FfbPacketKind.StopAll, 0, false, FfbEffect.Default, nowMs);
        }
    }

    public int ActiveSlots
    {
        get { lock (_lock) return _sent.Count(s => s is { Playing: true }); }
    }

    // The strongest force the game is playing now (0..1, gain folded in): the constant force's level, a
    // spring's or damper's coefficient, a periodic effect's magnitude. NFS keeps its effects created but at
    // zero in its menus, and well above zero while driving (springs and dampers 0.4..0.9 even when coasting).
    public float PlayingStrength
    {
        get
        {
            lock (_lock)
            {
                float strongest = 0;
                foreach (var s in _sent)
                {
                    if (s is not { Playing: true } sent) continue;
                    var e = sent.Effect;
                    float strength = e.Type switch
                    {
                        FfbEffectType.Constant => Math.Abs(e.ConstantLevel),
                        FfbEffectType.Spring or FfbEffectType.Damper or FfbEffectType.Friction or FfbEffectType.Inertia
                            => Math.Max(Math.Abs(e.PositiveCoefficient), Math.Abs(e.NegativeCoefficient)),
                        _ => e.Magnitude,
                    };
                    strongest = Math.Max(strongest, strength * e.Gain);
                }
                return strongest;
            }
        }
    }

    static short I16(ReadOnlySpan<byte> d, int at) => BinaryPrimitives.ReadInt16LittleEndian(d[at..]);
    static ushort U16(ReadOnlySpan<byte> d, int at) => BinaryPrimitives.ReadUInt16LittleEndian(d[at..]);

    delegate void BlockAction(Block b);

    void WithBlock(byte ebi, BlockAction action)
    {
        if (ebi == 0) return;
        if (!_blocks.TryGetValue(ebi, out var block))
        {
            block = new Block();
            _blocks[ebi] = block;
        }
        if (block.Slot < 0)
        {
            int free = Array.FindIndex(_slotOwner, o => o == null);
            if (free < 0)
            {
                if (_seen.Add($"slots full {ebi}"))   // once per block, not per report
                {
                    _log($"pid: all {MaxSlots} slots in use, effect block {ebi} is not played");
                }
            }
            else
            {
                block.Slot = free;
                _slotOwner[free] = ebi;
            }
        }
        action(block);
    }

    // A type-specific report: to its block, or (EBI 0) pending for the next Set Effect of that kind and, for an
    // update of a running effect, to the last block of that kind.
    void WithParams(byte ebi, ParamKind kind, BlockAction action)
    {
        if (ebi != 0)
        {
            WithBlock(ebi, action);
            return;
        }
        if (!_loggedEbiZero)
        {
            _loggedEbiZero = true;
            _log("pid: parameters come with effect block 0: matched to the Set Effect that follows");
        }
        _pending[kind] = action;
        if (_lastOfKind.TryGetValue(kind, out byte last) && _blocks.ContainsKey(last))
        {
            WithBlock(last, action);
        }
    }

    // Set Effect: 0 EBI, 1 type, 2 duration ms, 4 trigger repeat, 6 sample period, 8 start delay ms,
    // 10 gain 0..255, 11 trigger button, 12 bit0 X enable / bit1 Y enable / bit2 direction enable,
    // 13 direction 1 (0..32767 = 0..360 degrees), 15 direction 2, 17/19 type-specific block offsets.
    void OnSetEffect(ReadOnlySpan<byte> d)
    {
        int type = d[1];
        ushort duration = U16(d, 2);
        ushort startDelay = U16(d, 8);
        float gain = d[10] / 255f;
        byte axes = d[12];
        ushort direction = d.Length >= 15 ? U16(d, 13) : (ushort)0;
        byte ebi = d[0];
        WithBlock(ebi, b =>
        {
            b.PidType = type;
            b.DurationMs = duration is 0xFFFF or 0 ? null : duration;
            b.StartDelayMs = startDelay == 0xFFFF ? 0 : startDelay;
            b.Gain = gain;
            if (KindOf(type) is ParamKind kind)
            {
                _lastOfKind[kind] = ebi;
                if (_pending.Remove(kind, out var pending)) pending(b);
            }
            // With a direction, the force points that way (polar, 90 degrees = +X, as the shim reads DirectInput
            // polar directions); without one it acts along the enabled axes: here only the wheel counts.
            if ((axes & 0x04) != 0)
            {
                double radians = direction / 32767.0 * 2 * Math.PI;
                b.Along = (float)Math.Sin(radians);
            }
            else
            {
                b.Along = (axes & 0x01) != 0 ? 1f : 0f;
            }
        });
    }

    // Set Condition: 0 EBI, 1 low nibble = parameter block (0 = X, 1 = Y), 2 centre, 4 +coef, 6 -coef
    // (all -10000..10000), 8 +sat, 10 -sat, 12 deadband (0..10000).
    void OnSetCondition(ReadOnlySpan<byte> d)
    {
        if ((d[1] & 0x0F) == 1) return;              // only the X (wheel) block matters (pid.dll may write 0xF)
        float center = I16(d, 2) / 10000f, pos = I16(d, 4) / 10000f, neg = I16(d, 6) / 10000f;
        float posSat = U16(d, 8) / 10000f, negSat = U16(d, 10) / 10000f, dead = U16(d, 12) / 10000f;
        WithParams(d[0], ParamKind.Condition, b =>
        {
            b.Center = center; b.PosCoef = pos; b.NegCoef = neg;
            b.PosSat = posSat; b.NegSat = negSat; b.Deadband = dead;
        });
    }

    // Set Periodic: 0 EBI, 1 magnitude 0..10000, 3 offset -10000..10000, 5 phase (1/100 degree),
    // 7 period ms (32 bit).
    void OnSetPeriodic(ReadOnlySpan<byte> d)
    {
        float magnitude = U16(d, 1) / 10000f, offset = I16(d, 3) / 10000f, phase = U16(d, 5) / 100f;
        float period = BinaryPrimitives.ReadUInt32LittleEndian(d[7..]);
        WithParams(d[0], ParamKind.Periodic, b => { b.Magnitude = magnitude; b.Offset = offset; b.PhaseDeg = phase; b.PeriodMs = period; });
    }

    // Effect Operation: 0 EBI, 1 op (1 start, 2 start solo, 3 stop), 2 loop count (255 = infinite).
    void OnEffectOperation(ReadOnlySpan<byte> d, long nowMs)
    {
        byte op = d[1], loops = d[2];
        if (op == 2)
        {
            foreach (var other in _blocks) if (other.Key != d[0]) other.Value.Started = false;
        }
        WithBlock(d[0], b =>
        {
            if (op is 1 or 2)
            {
                b.Started = true;
                b.StartedAtMs = nowMs;
                b.Loops = loops == 255 ? null : Math.Max(1, (int)loops);
            }
            else if (op == 3)
            {
                b.Started = false;
            }
        });
    }

    void OnBlockFree(byte ebi, long nowMs)
    {
        foreach (var kind in _lastOfKind.Where(k => k.Value == ebi).Select(k => k.Key).ToList()) _lastOfKind.Remove(kind);
        if (!_blocks.Remove(ebi, out var block) || block.Slot < 0) return;
        _slotOwner[block.Slot] = null;
        if (_sent[block.Slot] is { Playing: true } last)
        {
            Send(FfbPacketKind.SlotState, (byte)block.Slot, false, last.Effect, nowMs);
        }
        _sent[block.Slot] = null;
    }

    // Device Control: 1 enable actuators, 2 disable, 3 stop all, 4 reset, 5 pause, 6 continue.
    void OnDeviceControl(byte command, long nowMs)
    {
        switch (command)
        {
            case 1: _actuatorsOn = true; break;
            case 2: _actuatorsOn = false; break;
            case 3:
                foreach (var b in _blocks.Values) b.Started = false;
                break;
            case 4:
                _blocks.Clear();
                Array.Fill(_slotOwner, null);
                Array.Fill(_sent, null);
                _paused = false;
                _actuatorsOn = true;
                Send(FfbPacketKind.StopAll, 0, false, FfbEffect.Default, nowMs);
                break;
            case 5: _paused = true; break;
            case 6: _paused = false; break;
        }
    }

    bool Playing(Block b, long nowMs)
    {
        if (!b.Started || !_actuatorsOn || _paused || TypeFor(b.PidType) == null) return false;
        long elapsed = nowMs - b.StartedAtMs;
        if (elapsed < b.StartDelayMs) return false;
        if (b.DurationMs is not long duration || b.Loops is not int loops) return true;
        return elapsed - b.StartDelayMs < duration * loops;
    }

    FfbEffect ToEffect(Block b)
    {
        var e = FfbEffect.Default;
        e.Type = TypeFor(b.PidType) ?? FfbEffectType.Constant;
        e.Gain = Math.Clamp(b.Gain * _deviceGain, 0f, 1f);
        switch (b.PidType)
        {
            case 1: e.ConstantLevel = Math.Clamp(b.ConstantMagnitude * b.Along, -1f, 1f); break;
            case 2: e.ConstantLevel = Math.Clamp((b.RampStart + b.RampEnd) / 2 * b.Along, -1f, 1f); break;
            case >= 3 and <= 7:
                e.Magnitude = b.Magnitude * Math.Abs(b.Along);
                e.Offset = b.Offset * b.Along;
                e.PeriodMs = b.PeriodMs;
                e.PhaseDeg = b.Along < 0 ? (b.PhaseDeg + 180f) % 360f : b.PhaseDeg;
                break;
            default:
                e.Center = b.Center; e.Deadband = b.Deadband;
                e.PositiveCoefficient = b.PosCoef; e.NegativeCoefficient = b.NegCoef;
                e.PositiveSaturation = b.PosSat; e.NegativeSaturation = b.NegSat;
                break;
        }
        return e;
    }

    void SendChanges(long nowMs, bool force)
    {
        for (int slot = 0; slot < MaxSlots; ++slot)
        {
            if (_slotOwner[slot] is not byte ebi || !_blocks.TryGetValue(ebi, out var b)) continue;
            bool playing = Playing(b, nowMs);
            var effect = ToEffect(b);
            var last = _sent[slot];
            bool changed = last is null || last.Value.Playing != playing || (playing && last.Value.Effect != effect);
            if (changed || (force && last is { Playing: true }))
            {
                Send(FfbPacketKind.SlotState, (byte)slot, playing, effect, nowMs);
                _sent[slot] = (playing, effect);
            }
        }
    }

    void Send(FfbPacketKind kind, byte slot, bool playing, in FfbEffect effect, long nowMs)
    {
        _send(FfbPacket.Encode(kind, slot, ++_sequence, playing, effect));
        _lastSendMs = nowMs;
        ++PacketsSent;
    }
}
