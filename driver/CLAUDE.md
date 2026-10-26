# driver/

UMDF 2, VS + WDK. Loaded when working in `driver/`; the root CLAUDE.md still applies.

- `wheelbri-wheel` = HID minidriver for every virtual wheel, picked by hardware id.
- `hide/` (`wheelbri-hide`) = upper filter hiding the rig from all but the service's `AllowedPid`, fails open.
- Both share core's `HidWheelDevice`, `HidPid`, `HidPp`, `StatePacket`.

Build in a Developer PowerShell, install from an Administrator PowerShell (`driver\hide\wheelbri-hide.vcxproj` +
`install-hide.ps1` likewise):

```powershell
msbuild driver\wheelbri-wheel.vcxproj /p:Configuration=Release /p:Platform=x64
powershell -NoProfile -ExecutionPolicy Bypass -File driver\install-driver.ps1
```
