# driver/

UMDF 2, VS + WDK. Loaded when working in `driver/`; the root CLAUDE.md still applies.

- `wheelbri-wheel` = HID minidriver for every virtual wheel, picked by hardware id.
  `wheelbri\G923_TF` = the G923's TrueForce interface (TrueForce.hpp): `TrueForceLink` takes the steering state on
  41331 and sends the stream's force to the service on 41330 (`TrueForceFfb.hpp`).
- `hide/` (`wheelbri-hide`) = upper filter hiding the rig from all but the service's `AllowedPid`, fails open.
- Both share core's `HidWheelDevice`, `HidPid`, `HidPp`, `StatePacket`.
- Both links (wheel state on 41329, TrueForce on 41331) take datagrams only from WheelBri's programs
  (`SenderProbe.hpp`: the sending port's process, looked up per new sender); others are dropped with a warning.

Build in a Developer PowerShell, install from an Administrator PowerShell (`driver\hide\wheelbri-hide.vcxproj` +
`install-hide.ps1` likewise):

```powershell
msbuild driver\wheelbri-wheel.vcxproj /p:Configuration=Release /p:Platform=x64
powershell -NoProfile -ExecutionPolicy Bypass -File driver\install-driver.ps1
```
