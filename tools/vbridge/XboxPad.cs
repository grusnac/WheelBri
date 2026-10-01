// A virtual Xbox 360 controller next to the virtual G920 (wheelemu-bridge --xbox-pad).
//
// Some games choose their on-screen button prompts by the kind of controller they see: NFS 2015 shows
// keyboard keys in its menus with a wheel, Xbox buttons with an Xbox controller. This pad is there so the
// game can show Xbox buttons. Its sticks and triggers never move, so it can never steer or drive.
//
//   idle    : connected, never pressed. Enough for games that look only at which controllers exist.
//   buttons : the wheel's A, B, X, Y, Menu (Start), View (Back), stick clicks and D-pad also press the pad's,
//             for games that switch prompts to the controller used last. Only while no pedal is pressed and
//             for 2 s after: in menus the pedals rest, while driving a button must not also press the pad's
//             (the game's own Xbox controller bindings would fire as well). The paddles (gears) and the
//             Xbox button (Game Bar) are never passed on.
//             In menus the D-pad goes to the pad INSTEAD of the G920: NFS navigates its menus with the G920's
//             D-pad too, and that switched the prompts back to the keyboard after every step (the buttons
//             do not, so they go to both).
//             Turning the wheel in a menu did the same, so in a menu the G920's steering also stays where it
//             was. "In a menu" for the steering is stricter than for the buttons, because getting it wrong
//             while driving would be dangerous: the game has force feedback effects but plays them all at zero
//             (NFS does that in its menus; while driving its spring and damper alone are 0.4..0.9) for 1.5 s,
//             AND no pedal for 2 s. Any force or pedal gives the steering back at once. A game that never used
//             force feedback in this run never gets its steering held.

using HIDMaestro;

namespace Wheelemu.Bridge;

internal sealed class XboxPad : IDisposable
{
    public enum Mode { Off, Idle, Buttons }

    const string ProfileId = "xbox-360-wired";
    const string Identity = "wheelemu-xbox360";
    const long PedalQuietMs = 2000;
    const long ForceQuietMs = 1500;
    const float ForceZero = 0.01f;
    const int PedalPressed = 65535 / 10;          // 10 % of travel (G920 pedals: 65535 = released)

    // G920 button bit -> Xbox button (A, B, X, Y, paddles R/L, Menu, View, RSB, LSB, Xbox).
    static readonly (int Bit, HMButton Button)[] Map =
    {
        (0, HMButton.A), (1, HMButton.B), (2, HMButton.X), (3, HMButton.Y),
        (6, HMButton.Start), (7, HMButton.Back), (8, HMButton.RightStick), (9, HMButton.LeftStick),
    };

    readonly Mode _mode;
    readonly Action<string> _log;
    readonly Func<float> _gameForce;              // strongest force the game plays now (PidForwarder)
    readonly Func<bool> _gameUsesForce;           // the game has used force feedback in this run
    long _lastForceMs;
    bool _inMenu;
    int _heldSteering;
    readonly System.Diagnostics.Stopwatch _clock = System.Diagnostics.Stopwatch.StartNew();
    long _lastPedalMs = long.MinValue / 2;
    HMButton _blocked;                            // held while driving: ignored until released
    HMButton _lastButtons;
    HMHat _lastHat;
    bool _hatBlocked;
    bool _first = true;
    long _presses;
    readonly object _lock = new();
    bool _disposed;

    public HMController Controller { get; }

    XboxPad(HMController controller, Mode mode, Action<string> log, Func<float> gameForce, Func<bool> gameUsesForce)
    {
        Controller = controller;
        _mode = mode;
        _log = log;
        _gameForce = gameForce;
        _gameUsesForce = gameUsesForce;
    }

    public static XboxPad? Create(HMContext ctx, Mode mode, Action<string> log, Func<float> gameForce,
                                  Func<bool> gameUsesForce)
    {
        try
        {
            if (ctx.GetProfile(ProfileId) == null)
            {
                ctx.LoadDefaultProfiles();
            }
            var profile = ctx.GetProfile(ProfileId);
            if (profile == null)
            {
                log($"Xbox controller: HIDMaestro has no '{ProfileId}' profile; carrying on without it");
                return null;
            }
            var controller = ctx.CreateController(profile, Identity);
            var pad = new XboxPad(controller, mode, log, gameForce, gameUsesForce);
            controller.SubmitState(new HMGamepadState());   // centred sticks, released triggers, nothing pressed
            log(mode == Mode.Idle
                ? "virtual Xbox 360 controller created (idle: connected, never pressed)"
                : "virtual Xbox 360 controller created: the wheel's A/B/X/Y/Menu/View/D-pad press it too while "
                  + "no pedal is pressed");
            return pad;
        }
        catch (Exception e)
        {
            log($"Xbox controller: could not be created ({e.Message}); carrying on without it");
            return null;
        }
    }

    // Every wheel state the bridge receives. Returns what the G920 reports: the same state, except that the
    // D-pad is centred while the pad has it, and the steering is held while in a menu.
    public TargetReport Update(in TargetReport state)
    {
        if (_mode != Mode.Buttons)
        {
            return state;
        }
        long now = _clock.ElapsedMilliseconds;
        if (65535 - state.Throttle > PedalPressed || 65535 - state.Brake > PedalPressed)
        {
            _lastPedalMs = now;
        }
        bool driving = now - _lastPedalMs < PedalQuietMs;
        if (_gameForce() > ForceZero)
        {
            _lastForceMs = now;
        }
        bool inMenu = !driving && _gameUsesForce() && now - _lastForceMs >= ForceQuietMs;
        if (inMenu != _inMenu)
        {
            _inMenu = inMenu;
            _heldSteering = state.Steering;
            _log(inMenu ? "Xbox controller: in a menu (no force, no pedal): the G920's steering is held"
                        : "Xbox controller: driving again: the G920's steering follows the wheel");
        }

        HMButton wheel = HMButton.None;
        foreach (var (bit, button) in Map)
        {
            if ((state.Buttons & (1u << bit)) != 0) wheel |= button;
        }
        HMHat hat = state.Hat is >= 0 and <= 7 ? (HMHat)(state.Hat + 1) : HMHat.None;

        // A button held when driving stops is not a press: it must be let go first.
        if (driving)
        {
            _blocked = wheel;
            _hatBlocked = hat != HMHat.None;
        }
        else
        {
            _blocked &= wheel;
            _hatBlocked &= hat != HMHat.None;
        }
        HMButton buttons = driving ? HMButton.None : wheel & ~_blocked;
        HMHat padHat = driving || _hatBlocked ? HMHat.None : hat;

        TargetReport g920 = state;
        if (padHat != HMHat.None) g920 = g920 with { Hat = -1 };
        if (_inMenu) g920 = g920 with { Steering = _heldSteering };
        if (buttons == _lastButtons && padHat == _lastHat)
        {
            return g920;
        }
        if ((buttons & ~_lastButtons) != 0 || (padHat != HMHat.None && _lastHat == HMHat.None))
        {
            ++_presses;
            if (_first)
            {
                _first = false;
                _log($"Xbox controller: first button press passed on ({(buttons != HMButton.None ? buttons : padHat)})");
            }
        }
        _lastButtons = buttons;
        _lastHat = padHat;
        lock (_lock)
        {
            if (!_disposed) Controller.SubmitState(new HMGamepadState { Buttons = buttons, Hat = padHat });
        }
        return g920;
    }

    public void Dispose()
    {
        if (_mode == Mode.Buttons)
        {
            _log($"Xbox controller: {_presses} button presses passed on");
        }
        lock (_lock)
        {
            if (_disposed) return;
            _disposed = true;
            try { Controller.SubmitState(new HMGamepadState()); } catch (Exception) { }
            Controller.Dispose();
        }
    }
}
