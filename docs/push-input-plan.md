# Plan: the service loop from pull to push

Status: implemented on the branch `feature/push-input` (from `v0.8.1`), 2026-10-07; to be measured against
`loop_mode = fixed` with `scripts/make/measure.sh` before it is merged. `loop_mode = events` is the default on the
branch.

## Why

The service loop wakes 1000 times a second (`loop_hz`) and pulls: it asks DirectInput for the wheel's state
(`DinputReader::read`: `Poll()` + `GetDeviceState()`) and checks the force feedback socket without waiting, whether
anything changed or not. Since 0.8.0 the wait between ticks is cheap (the loop thread at 8-10 % of a core, measured
with `scripts/make/measure.sh`), but it is still 1000 wake-ups a second, also with the wheel at rest in a menu.

In push mode the loop sleeps until Windows (or Linux) says something happened, and only then does the work.

## What changes

1. **The wheel and accessories wake the loop.**
   - Windows: `DinputReader` creates an auto-reset event and calls `SetEventNotification` before `Acquire`;
     DirectInput signals it when the device's state changes. Reading stays `GetDeviceState`, so the numbering of
     axes, buttons and hats does not change.
   - A device DirectInput calls polled (`DIDC_POLLEDDEVICE`, no notifications) has no event: the session falls back
     to the fixed 1 kHz loop and says so in the log.
   - Linux: the evdev file descriptor can be waited on (`poll`).
2. **The sockets wake the loop**: the game's force feedback (41330) and the test clutch (41331).
   - Windows: `WSAEventSelect(FD_READ)` gives each socket an event. Linux: the socket fd.
   - `UdpLoopbackListener` gets an accessor for its native socket.
3. **One waiting point**, `platform::WaitSet`.
   - Windows: `WaitForMultipleObjects` on those events plus the high-resolution waitable timer for the next deadline.
   - Linux: `ppoll`.
   - At least 0.5 ms between ticks, so a device or socket that signals without pause cannot spin the loop.
4. **A timer for what must happen on time.** A core class (working name `WakePlanner`, unit-tested) works out the next
   deadline from:
   - force feedback while effects play: every 4 ms (`FfbController` `minUpdateNs`), so the limiter's slew stays
     smooth, and the 500 ms watchdog;
   - the 100 ms keep-alive to the game (`StateSendGate::kKeepAliveNs`);
   - the window's frame copy, every 33 ms while a window is open;
   - the test clutch expiring 1.5 s after the last packet;
   - the 10 s loop statistics and the telemetry heartbeat;
   - nothing playing and no window: 100 ms.
5. **What counts ticks becomes time-based**: `frameEveryTicks`, `printEveryTicks`, the loop statistics. "Period" and
   "overruns" mean nothing without a fixed rate: the log reports wake-ups a second by reason (wheel, force feedback,
   timer) and the delay from a wake-up to the packet sent.
6. **A setting to compare both ways**: `[emulator] loop_mode = events | fixed` (`events` the default on the branch,
   `fixed` = today's 1 kHz loop). Into the reader, `EmulatorConfig`, both `config/g920-*.ini` and
   `EmulatorConfigTest`. It allows A/B measurements with `measure.sh` and going back at once if driving feels different.

## What stays

- Force feedback safety: `FfbSafetyLimiter` already works from real elapsed time (`dt` from the last update), the
  watchdog is checked at every deadline, `AsyncFfbSink` is untouched.
- The pipeline, the mapping, `StateSendGate`, the wire format, the shim and both drivers.

## Commits, in order

Each one builds, passes the tests and comes with build and test steps.

1. `WaitSet` (Windows and Linux) and the socket events; a test with a real loopback socket (as `UdpLoopbackTest`).
2. The wheel's notification event (DirectInput, evdev). The log says whether the wheel supports it and how often it
   signals a second. Already answers the main open question (risk 1) with `loop_mode` still fixed.
3. `WakePlanner` with tests (fake clock; effects playing or not; window open or not); the UI frame, print and
   statistics made time-based.
4. The loop in `events` mode and the `loop_mode` setting.
5. CLAUDE.md, README, and the README entry under Functionalities once it is called finished.

## How to check it

- `measure.sh` phases: `closed`, `stopped`, then for each `loop_mode`: in a menu (wheel at rest), driving with the
  shim, driving with the virtual device.
- The service log: wake-ups a second by reason, the wake-to-packet delay (well under 1 ms), no "no wheel state"
  warnings in the shim or driver logs.
- Feel: steering, pedals, buttons and force feedback in NFS, the two modes side by side.

## Risks

1. Not known yet how the Moza R3 reports at rest: if it sends the same values all the time, push mode saves little
   (commit 2 measures it before the bigger steps).
2. While driving the gain is small: the wheel reports ~1000 times a second while it moves, and force feedback keeps it
   moving.
3. Linux (evdev) would be unit-tested only until it runs on a Steam Deck.
4. Not addressed: the driver host (the hide filter and the G920 driver, ~10 % of the PC in virtual sessions); it
   needs its own fix.

## Edge cases handled (2026-10-07)

- **A driver that does not signal its event** (DirectInput gives no error for that): each pass the timer woke that
  still found new input counts as a missed notification (`Pipeline::inputChanged`, `NotificationCheck`); 20 within
  10 s switch the session to `fixed` for good, with a warning naming `loop_hz`.
- **The wait itself fails** (`WaitForMultipleObjects` / `poll` error): `WaitSet::kFailed` with the reason; the session
  switches to `fixed` at once instead of sleeping until the deadline and failing again.
- **A source that never stops signalling** (or an evdev descriptor left in error): never more than one pass every
  0.5 ms (`floodGuardWaitNs`).
- **A datagram that arrives while the loop works**: the socket's event is reset before the pass reads, so nothing is
  lost; one left unread wakes the next wait at once (`WaitSetTest`).
- **Few passes**: the window's copy, `--print` and a lost wheel go by the clock (`Periodic`, `LossTimer`: 0.5 s of
  failed reads, re-acquiring at most every 50 ms), not by counting passes.
- **A clock that goes back**: `Periodic`, `LossTimer` and `NotificationCheck` start again rather than wrap around.
- **A notification lost now and then while the wheel moves** (too few to fall back): for 100 ms after a pass that
  found new input the timer wakes every 4 ms, so such a change waits 4 ms at most. At rest a lost one still waits for
  the idle timer (up to 100 ms): `PushLoop.ALostNotificationAtRestWaitsForTheIdleTimer` measures it.
- **A late notification** (the event a moment after the state): the timer's pass reads the input, the notification
  comes within 1 ms; counted as late, not as missed, so it never causes a fallback.

Checked for abuse from other programs (2026-10-07; any local program can send to the UDP ports):

- **An empty datagram** no longer hides the ones behind it on Windows: a socket counts as ready while a datagram of
  any size waits (a one-byte peek, not FIONREAD, which gave the first one's size: 0).
- **A flood on 41330 / 41331** wakes the loop at most every 0.5 ms (about the cost of loop_mode = fixed); the game's
  packets and the wheel's input are still read within that gap (`PushLoop.AFloodOnTheForceFeedbackPort...`). Who
  may send force feedback is still decided by SenderGate.
- **A descriptor closed behind the wait's back** (Linux, POLLNVAL) is a failed wait: the loop falls back to the
  fixed rate instead of waking without pause. On Windows a closed handle already failed the wait.
- **A wait that wakes for nothing again and again** gives up after 64 rounds and lets the loop run a pass.
- Still open, older than the events loop: the test clutch port (41331) takes packets from any local program, with no
  sender check; it only replaces the clutch while packets come.

All of these run as scenarios on emulated devices (`tests/emulators`, `PushLoopTest`): latency of every change, wake
counts, when and why it falls back. Not covered there: whether DirectInput and evdev really behave like the emulators;
that needs the rig (the "loop (events)" line's missed / late counts).

Size: about 8-10 files plus tests over the five commits.
