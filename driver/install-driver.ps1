# Installs wheelemu's G920 driver (build it first, see README.md). Administrator. Secure Boot and test
# mode stay as they are: the package is signed with a certificate made on this PC by the WDK build
# (WDKTestCert), which this script adds to the machine's trusted Root and TrustedPublisher stores.
# The same driver serves the Xbox controller's HID side (what joy.cpl lists).
#
#   install-driver.ps1                 Release build
#   install-driver.ps1 -Configuration Debug
#
# Exit codes: 0 installed (a yellow line says when Windows wants a restart), 1 failed (the red line says why).

param([string]$Configuration = 'Release')
# A 32-bit PowerShell (one started from a 32-bit prompt, e.g. some Developer PowerShell windows) sees SysWOW64,
# where pnputil and the 64-bit driver tools are missing: run this again in the 64-bit PowerShell.
if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
    & (Join-Path $env:SystemRoot 'Sysnative\WindowsPowerShell\v1.0\powershell.exe') -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -Configuration $Configuration
    exit $LASTEXITCODE
}
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
Assert-Admin

$inf = Find-BuiltPackage $PSScriptRoot 'wheelemu-g920' $Configuration
Add-PackageCertificate $inf 'wheelemu-g920'
Invoke-Pnputil @('/add-driver', $inf.FullName, '/install') 'installing wheelemu-g920' | Out-Null
if (@(Get-InstalledPackages 'wheelemu-g920').Count -eq 0) {
    Stop-WithError 'pnputil reported success, but Windows does not list wheelemu-g920.inf as installed.'
}
Write-Host ''
Write-Host 'Installed. Start wheelemu-service with --virtual-g920 (as Administrator) to create the G920.' -ForegroundColor Green
