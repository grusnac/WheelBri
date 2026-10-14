@echo off
rem Downloads, from Microsoft, what WheelBri needs on Windows 7 SP1 (64-bit) into this folder, each checked to be
rem signed by Microsoft (download-prerequisites.ps1 does the work, in the Windows PowerShell every Windows from 7 on
rem has). Double-click it, on any PC with internet. Then copy this folder to the Windows 7 PC and run
rem install-prerequisites.cmd there as Administrator. -Force downloads everything again.
rem Exit codes: 0 everything is here and signed, 1 something failed (it says what).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0download-prerequisites.ps1" %*
set "CODE=%ERRORLEVEL%"
pause
exit /b %CODE%
