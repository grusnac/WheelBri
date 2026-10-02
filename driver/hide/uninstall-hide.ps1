# Removes wheelemu-hide: the R3's game-controller interface goes back to Windows' own setup (input.inf) and
# nothing hides it any more. Administrator; stop wheelemu-service first.

# A 32-bit PowerShell (one started from a 32-bit prompt, e.g. some Developer PowerShell windows) sees SysWOW64,
# where pnputil and the 64-bit driver tools are missing: run this again in the 64-bit PowerShell.
if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
    & (Join-Path $env:SystemRoot 'Sysnative\WindowsPowerShell\v1.0\powershell.exe') -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath 
    exit $LASTEXITCODE
}
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common.ps1')
Assert-Admin

# No stale "hidden" setting left behind, whatever happens next.
Remove-ItemProperty -Path 'HKLM:\SOFTWARE\wheelemu\hide' -Name 'AllowedPid', 'AllowedPidStart' -ErrorAction SilentlyContinue

Remove-InstalledPackages 'wheelemu-hide' | Out-Null
if (@(Get-InstalledPackages 'wheelemu-hide').Count -gt 0) { Stop-WithError 'wheelemu-hide is still installed.' }
# Windows picks its own driver for the R3 again.
Invoke-Pnputil @('/scan-devices') 'rescanning devices' | Out-Null
Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'HID\VID_346E&PID_0005&MI_02*' } |
    ForEach-Object { Write-Host "$($_.InstanceId): $($_.Status), $($_.FriendlyName)" }
Write-Host 'Removed.' -ForegroundColor Green
