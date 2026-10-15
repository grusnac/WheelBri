# Windows 7: the in-game shim only

Status: steps 1 and 2 built (2026-10-14), not yet tried on Windows 7; step 3 waits for that test. Scope: **Windows 7
SP1, 64-bit**. Only the in-game shim way: no drivers, no virtual G920, no hiding on Windows 7 (those stay Windows 11
only, as today). A virtual device there would need our own kernel driver, which 64-bit Windows 7 loads only in
test-signing mode, so it was dropped.

## What already works there (checked 2026-10-14 on the current build)

Every Windows function the app (`wheelbri.exe`, `wheelbri-service.exe`, `wheelbri-helper.exe`) and the shim
(`dinput8.dll`) import exists on Windows 7 (the newest are Vista and 7 ones: `CreateWaitableTimerExW`,
`SetWaitableTimerEx`, `GetNamedPipeServerProcessId`, `CancelIoEx`). The Windows 8+ ones are looked up at run time
and skipped when missing (`SwDeviceCreate`, `CM_Register_Notification`, the DPI calls, thread names). DirectInput 8,
which the shim and the service use, is in Windows 7.

The one thing missing: the programs and the shim use the **Universal C Runtime** (`api-ms-win-crt-*.dll`, MSYS2
UCRT64), which Windows 7 has only with update **KB2999226** (Windows Update installs it; most up-to-date Windows 7 PCs
have it). Without it the app does not start, and a game cannot load the shim.

## Steps

1. **Mode: Shim only on Windows 7.** The app knows which Windows it runs on (build number); where the virtual
   device cannot exist (before Windows 11, build 22000, as the driver's INF requires), Mode is fixed to Shim, the
   Virtual button is greyed out with a tooltip ("needs Windows 11"), and an app.ini saying Virtual is read as Shim.
   Check that a shim session needs no privileged step (the Helper is not installed there). Unit tests for the
   choice.
2. **Installer on Windows 7.** `install.ps1` refuses anything before build 22000 today. New: Windows 7 SP1 (build
   7601) to Windows 10 install the app, the configs and the shim, without the drivers, the hiding filter and the
   WheelBri Helper; the uninstaller copes with them missing. Checks first, with what to do:
   - KB2999226 (the Universal C Runtime: `ucrtbase.dll` in System32);
   - PowerShell 5.1 (Windows 7 ships 2.0; WMF 5.1, KB3191566, gives 5.1): `Setup.cmd` checks the version before
     starting `install.ps1`, so the scripts stay as they are.
   - Both, with .NET Framework 4.5.2 (WMF 5.1 needs it), downloaded from Microsoft by
     `installer/win7/download-prerequisites.cmd` (Windows PowerShell 2.0 or newer; on Windows 7 the download may
     fail for lack of TLS 1.2, then on another PC) and installed by `installer/win7/install-prerequisites.cmd` (cmd
     and Windows' own tools only: it runs on a Windows 7 that has nothing yet).
3. **The rest of the app on Windows 7**: things only a real Windows 7 shows (the window, fonts and DPI, the tray
   notification, the logs folder, the Games panel's folder picker). Fixed as found.
4. **Docs**: README (requirements, the Windows 7 limits), `installer/README.txt`, `docs/games.md` per game.

## How to test

A Windows 7 SP1 x64 PC or VM (the R3 passed through over USB), fully updated, with WMF 5.1:

- the installer on it: app, configs and shim in, no drivers, no Helper service;
- the app: Mode Shim, Virtual greyed out; Add game; Start; a game (NFS 2015 runs on Windows 7) with steering,
  pedals, buttons and force feedback through the limiter; the shim's and the service's logs;
- the uninstaller.

## Risks

- A future MSYS2 runtime could start importing a Windows 8+ function: then the app or the shim stops loading on
  Windows 7. A check in `package.ps1` (the import table against a list of Windows 7 functions) would catch it at
  packaging time.
- Nothing can be tried on the development PC (Windows 11): every step needs the Windows 7 PC or VM.
