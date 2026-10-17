# CPU measurements

Measured with `scripts/make/measure.sh` (MSYS2 UCRT64), which opens `measure.ps1` in its own Administrator PowerShell
window: `measure.sh [-Phase name] [-Game exe name without .exe] [-Seconds N]` (also `-IntervalMs`, `-Top`); keys M =
new phase, Q = stop. One `perf-<date>-<time>.log` per round in the logs folder (`%LOCALAPPDATA%\wheelbri\logs`,
left alone by housekeeping); `measure.sh` prints its `[perf.summary]` lines when the window closes.
"core %" is the share of one core (100 = one core busy); the PC has 16 logical processors. The rounds below are
historical data, each under its date; the code has changed since (each round says what it measured). Add new rounds
at the end.

## 2026-10-07: the virtual G920's state rate (main, fixed 1 kHz loop)

**Question:** does the virtual G920 driver's state receiver cost CPU per state packet, so that sending fewer
packets (`state_rate_hz`) cuts it?

**Setup:** branch `main` (with thread names and the `measure.ps1` fix), virtual device (G920 driver + hide filter in one
`WUDFHost.exe`), NFS 2015 in its menu, hands off, about 4 to 9 minutes per round, `state_rate_hz` changed in the
user's `my-g920-virtual-device.ini` between rounds (one file per mode then; one `my-g920.ini` for both modes since
2026-10-15). Packets a second = the service's `packet #` at Stop divided by the
session's length (Start to Stop, `wheelbri-service-<date>-<NN>.log`).

| Round (perf log) | `state_rate_hz` | Packets a second | Receiver thread avg / peak | Driver host total | wheelbri.exe (loop) |
|---|---|---|---|---|---|
| `perf-2026-10-07-185044` | 500 | 237 | 58.6 / 79.3 | 127.8 | 20.7 (8.1) |
| `perf-2026-10-07-190435` | 250 | 171 | 55.3 / 74.9 | 122.9 | 19.9 (7.7) |
| `perf-2026-10-07-191843` | 250 (meant 125*) | 177 | 49.6 / 68.9 | 114.9 | 20.5 (8.3) |
| `perf-2026-10-07-214236` (repeat) | 500 | 296 | 66.8 / 82.5 | 138.9 | 19.6 (8.4) |
| 2026-10-06 (earlier) | none (every tick) | 1000 | 84 | | |

\* The 125 edit of the config never reached the PC; the service logged `state_rate_hz=250` at that Start.

The repeat at 500 sent more packets (296 against 237 a second; the wheel's noise varies) and cost more (66.8 against 58.6), which fits a per-packet part; the part that does not depend on the rate stays at about 45-50 % of a core.

**Findings:**

- Hands off, the wheel's state still changes about 170-240 times a second (sensor noise), so a cap of 500 never
  limited anything and 250 hardly did.
- Through the points: about 33 us of the receiver's CPU per packet (3 % of a core per 100 packets a second), plus
  **about 50 % of a core that does not depend on the rate**. The rate is not the fix.
- The driver host's other threads (Windows' UMDF and HID threads, not ours) add about 65-70 % of a core on top.
- The driver log has no receive errors.

**Suspected cause (not yet proven):** `completeReads` (`driver/Driver.cpp`) completes reads until the manual read
queue is empty. Windows' HID class sends a new read as soon as one completes; when it arrives while the loop is still
running, the loop completes it again with the same report, and so on. One state would then be handed over many times,
which would also keep the host's other threads busy.

**Proposed fix (no input lost):** each report is handed to Windows once; a read that finds nothing new waits for the
next state (answered at once when it comes, so no added latency). A short queue (about 8 reports), handed out one per
read and in order, keeps a quick button press and release between two reads. Force feedback (port 41330) is not
touched. A once-a-minute driver log line, "states received N, reads completed M", proves the cause (M much larger
than N) and shows the fix working.

## 2026-10-07: the same round on `feature/push-input` (events loop)

**Setup:** as above, `state_rate_hz` 500, `loop_mode = events` (a setting of that branch only; removed with the fixed
loop the same day; the service logged "running on events" and never fell
back: 0 notifications missed, 178 late in 9 minutes).

| Round (perf log) | Loop | Packets a second | Receiver thread avg / peak | Driver host total | wheelbri.exe (loop) |
|---|---|---|---|---|---|
| `perf-2026-10-07-185044` | main, fixed 1 kHz | 237 | 58.6 / 79.3 | 127.8 | 20.7 (8.1) |
| `perf-2026-10-07-214236` | main, fixed 1 kHz | 296 | 66.8 / 82.5 | 138.9 | 19.6 (8.4) |
| `perf-2026-10-07-215929` | push, events | 239 | 59.0 / 76.3 | 133.9 | 18.0 (2.3) |

**Findings:**

- The driver costs the same whichever loop feeds it: the same packets a second (the wheel's noise sets them, about
  240), the same receiver (59 % of a core) and driver host (134 %). The cost is in the driver, as expected from the
  same driver code on both branches.
- The events loop does what it should for the app: its loop thread falls from about 8 % to 2.3 % of a core (about
  530 wake-ups a second, nearly all from the wheel's notifications). The rest of wheelbri.exe is mostly threads that
  are not ours (an unnamed thread at Highest priority, about 7 %: DirectInput's own, seen on main too).

## 2026-10-07: the G920 driver hands each state to Windows once (main, after the merge)

**Change:** `driver/Driver.cpp`: a HID read gets the oldest state not handed out yet, or waits for the next one (up
to 8 kept in order); before, every state answered every read that came while it was being handed out. Built
`20261007_192135`. **Setup:** as above, events loop, `state_rate_hz` 500; input checked by hand in the NFS menu first
(wheel, pedals, buttons, D-pad).

| Round (perf log) | Driver | Packets a second | Receiver thread avg / peak | Driver host total | wheelbri.exe |
|---|---|---|---|---|---|
| `perf-2026-10-07-215929` | before | 239 | 59.0 / 76.3 | 133.9 | 18.0 |
| `perf-2026-10-07-222812` | each state once | about 230 | **1.8 / 7.8** | **47.6** | 18.4 |

**Findings:**

- The cause is confirmed: the receiver falls from 59 % to 1.8 % of a core at the same state rate, and the driver
  host's other threads fall with it (the host from 134 % to 48 %).
- The driver's new minute line, e.g. `input, last 60 s: 14504 states, 14504 reads answered (0 at once, 14504 after
  waiting), 0 states dropped, 0 waiting`: every state is handed out exactly once, none is dropped, and Windows always
  has a read waiting, so a state goes out the moment it arrives (no added latency).
- The rest of the driver host (about 48 %) is close to what the hide filter alone cost in a shim session (47 %, same
  `WUDFHost.exe`): the next thing to look at.
- One `WARNING: receiving from wheelbri-service failed (997)` in the driver log at 22:31:42 (Windows'
  "overlapped I/O in progress" on a blocking `recv`), once, input carried on: to watch.
