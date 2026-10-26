@echo off
rem Starts the WheelBri installer (install.ps1). Two ways it runs:
rem   - inside Setup.exe (IExpress): next to wheelbri.zip, in a temporary folder that IExpress deletes as soon as
rem     this script returns; the zip is unpacked to %TEMP% and install.ps1 runs from there, waiting for it to end.
rem   - from the unpacked package folder (WheelBri-<version>\Setup.cmd): install.ps1 runs from that folder.
rem Arguments are passed to install.ps1 (-InstallDir D:\WheelBri, -NoDesktopShortcut, -Quiet).
setlocal
set "HERE=%~dp0"
set "POWERSHELL=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "POWERSHELL=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"

rem The installer needs PowerShell 5.1: Windows 10 and 11 have it, Windows 7 SP1 gets it with Windows Management
rem Framework 5.1. Checked here, where any PowerShell (2.0 on Windows 7) can answer, before install.ps1 runs.
"%POWERSHELL%" -NoProfile -Command "if ($PSVersionTable.PSVersion -lt [version]'5.1') { exit 1 }" >nul 2>&1
if errorlevel 1 (
    echo WheelBri's setup needs PowerShell 5.1, which this Windows does not have yet.
    echo On Windows 7 SP1: install Windows Management Framework 5.1 ^(KB3191566, from Microsoft; it needs .NET
    echo Framework 4.5.2 or newer^), restart Windows, then run this setup again. The folder win7 in the package
    echo ^(WheelBri-^<version^>.zip^) has scripts that download and install them: see README.md there.
    pause
    exit /b 1
)

if exist "%HERE%install.ps1" (
    "%POWERSHELL%" -NoProfile -ExecutionPolicy Bypass -File "%HERE%install.ps1" %*
    rem plain "exit /b" keeps install.ps1's exit code (%ERRORLEVEL% here would be read before it ran)
    exit /b
)

if not exist "%HERE%wheelbri.zip" (
    echo The package is incomplete: neither install.ps1 nor wheelbri.zip is next to Setup.cmd.
    pause
    exit /b 1
)

set "UNPACKED=%TEMP%\wheelbri-setup-%RANDOM%%RANDOM%"
echo Unpacking WheelBri to %UNPACKED% ...
rem The paths go to PowerShell through the environment, not pasted into its command: a user name such as O'Brien
rem (in %TEMP%) would end a quoted string there.
"%POWERSHELL%" -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; try { Expand-Archive -LiteralPath (Join-Path $env:HERE 'wheelbri.zip') -DestinationPath $env:UNPACKED -Force } catch { Write-Host $_.Exception.Message -ForegroundColor Red; exit 1 }"
if errorlevel 1 (
    echo Could not unpack wheelbri.zip ^(is the disk full?^).
    pause
    exit /b 1
)

"%POWERSHELL%" -NoProfile -ExecutionPolicy Bypass -File "%UNPACKED%\install.ps1" %*
set "CODE=%ERRORLEVEL%"
rmdir /s /q "%UNPACKED%" >nul 2>&1
exit /b %CODE%
