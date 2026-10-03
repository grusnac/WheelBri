@echo off
rem Removes WheelBri (uninstall.ps1 next to this file; it asks for Administrator itself).
rem   - in the install folder: removes that installation;
rem   - in the unpacked package: removes the recorded installation, or what is left of one whose folder is gone.
rem Arguments go to uninstall.ps1 (-KeepConfig, -RemoveCertificate, -Quiet, -InstallDir D:\WheelBri).
setlocal
set "POWERSHELL=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "POWERSHELL=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%~dp0uninstall.ps1" (
    echo uninstall.ps1 is not next to Uninstall.cmd.
    pause
    exit /b 1
)
rem Out of the folder being deleted (a current directory inside it keeps it from being removed), and the exit on
rem the same line: this file is gone by the time the uninstall returns.
cd /d "%TEMP%"
"%POWERSHELL%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1" %* & exit /b
