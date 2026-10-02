# Removes wheelemu-xusb, the virtual Xbox controller's driver. Administrator; stop wheelemu-service first.

# A 32-bit PowerShell (one started from a 32-bit prompt, e.g. some Developer PowerShell windows) sees SysWOW64,
# where pnputil and the 64-bit driver tools are missing: run this again in the 64-bit PowerShell.
if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
    & (Join-Path $env:SystemRoot 'Sysnative\WindowsPowerShell\v1.0\powershell.exe') -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath 
    exit $LASTEXITCODE
}
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common.ps1')
Assert-Admin

Remove-InstalledPackages 'wheelemu-xusb' | Out-Null
if (@(Get-InstalledPackages 'wheelemu-xusb').Count -gt 0) { Stop-WithError 'wheelemu-xusb is still installed.' }
Write-Host 'Removed.' -ForegroundColor Green
