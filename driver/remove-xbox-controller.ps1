# Removes the virtual Xbox 360 controller that WheelBri had until 2026-10-03 (removed then: its button prompts never
# worked in NFS): the wheelbri-xusb driver (and wheelemu-xusb, its name before the rename) and the devices it served
# (SWD\wheelbri\XUSB, SWD\wheelbri\XBOXHID, the same under wheelemu). Nothing there: nothing to do. The installer runs it
# on every install (an upgrade takes the old controller away) and the uninstaller too. Administrator.
#
# Exit codes: 0 removed or nothing there, 1 something could not be removed (the lines above say what).

if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
    & (Join-Path $env:SystemRoot 'Sysnative\WindowsPowerShell\v1.0\powershell.exe') -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath
    exit $LASTEXITCODE
}
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
Assert-Admin

$failed = $false
$devices = @(Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object {
    $_.InstanceId -like 'SWD\wheelbri\XUSB*' -or $_.InstanceId -like 'SWD\wheelbri\XBOXHID*' -or
    $_.InstanceId -like 'SWD\wheelemu\XUSB*' -or $_.InstanceId -like 'SWD\wheelemu\XBOXHID*' })
foreach ($device in $devices) {
    pnputil /remove-device "$($device.InstanceId)" | Out-Null
    if ($LASTEXITCODE -in @(0, 3010)) {
        Write-Host "removed the old Xbox controller device $($device.InstanceId)"
    } else {
        Write-Host "could not remove $($device.InstanceId) (pnputil $LASTEXITCODE)" -ForegroundColor Yellow
        $failed = $true
    }
}
foreach ($name in 'wheelbri-xusb', 'wheelemu-xusb') {
    foreach ($package in @(Get-InstalledPackages $name)) {
        pnputil /delete-driver $package /uninstall /force | Out-Null
        if ($LASTEXITCODE -in $script:PnputilOk) {
            Write-Host "removed the old Xbox controller driver $package ($name.inf)"
        } else {
            Write-Host "could not remove $package ($name.inf): pnputil exited with $LASTEXITCODE" -ForegroundColor Yellow
            $failed = $true
        }
    }
}
if ($devices.Count -eq 0 -and -not $failed) { Write-Host 'no Xbox controller from an earlier version: nothing to remove' }
exit $(if ($failed) { 1 } else { 0 })
