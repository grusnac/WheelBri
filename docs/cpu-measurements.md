# CPU measurements

Measured with `scripts/make/measure.sh` (`measure.ps1`): one `perf-<date>-<time>.log` per round in the logs folder.
"core %" is the share of one core (100 = one core busy); the PC has 16 logical processors.

## 2026-10-07: the virtual G920's state rate (main, fixed 1 kHz loop)

**Question:** does the virtual G920 driver's state receiver cost CPU per state packet, so that sending fewer
packets (`state_rate_hz`) cuts it?

**Setup:** branch `main` (with thread names and the `measure.ps1` fix), virtual device (G920 driver + hide filter in one
`WUDFHost.exe`), NFS 2015 in its menu, hands off, about 4 to 9 minutes per round, `state_rate_hz` changed in the
user's `my-g920-virtual-device.ini` between rounds. Packets a second = the service's `packet #` at Stop divided by the
session's length (Start to Stop, `wheelbri-service-<date>.log`).

| Round (perf log) | `state_rate_hz` | Packets a second | Receiver thread avg / peak | Driver host total | wheelbri.exe (loop) |
|---|---|---|---|---|---|
| `perf-2026-10-07-185044` | 500 | 237 | 58.6 / 79.3 | 127.8 | 20.7 (8.1) |
| `perf-2026-10-07-190435` | 250 | 171 | 55.3 / 74.9 | 122.9 | 19.9 (7.7) |
| `perf-2026-10-07-191843` | 250 (meant 125*) | 177 | 49.6 / 68.9 | 114.9 | 20.5 (8.3) |
| 2026-10-06 (earlier) | none (every tick) | 1000 | 84 | | |

\* The 125 edit of the config never reached the PC; the service logged `state_rate_hz=250` at that Start.

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

**Next:** the same round on `feature/push-input` (`loop_mode = events`). The driver is the same on both branches, so
only the app's sending differs; if the driver costs the same there, the fix goes into the driver.
