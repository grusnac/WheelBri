# Removes wheelbri-hide: every device that has the filter (the rig's, whatever they are) goes back to Windows' own
# setup (input.inf) and nothing hides it any more. Administrator; stop wheelbri-service first.

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
Remove-ItemProperty -Path 'HKLM:\SOFTWARE\wheelbri\hide' -Name 'AllowedPid', 'AllowedPidStart' -ErrorAction SilentlyContinue

# Which devices have it now: found by their driver, not by any id.
$devices = @(Get-DevicesUsing 'wheelbri-hide')
$devices | ForEach-Object { Write-Host "has the filter: $($_.InstanceId) ($($_.FriendlyName))" }
Remove-InstalledPackages 'wheelbri-hide' | Out-Null
Remove-LegacyPackages 'wheelbri-hide' | Out-Null
if (@(Get-InstalledPackages 'wheelbri-hide').Count -gt 0) { Stop-WithError 'wheelbri-hide is still installed.' }
# Windows picks its own driver for those devices again.
Invoke-Pnputil @('/scan-devices') 'rescanning devices' | Out-Null
foreach ($device in $devices) {
    $now = Get-PnpDevice -InstanceId $device.InstanceId -ErrorAction SilentlyContinue
    if ($now) { Write-Host "$($now.InstanceId): $($now.Status), $($now.FriendlyName)" }
}
Write-Host 'Removed.' -ForegroundColor Green
