@echo off
rem Double-click: starts wheelemu-service (with its window) and the virtual G920 bridge, as Administrator.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0start-g920.ps1" %*
