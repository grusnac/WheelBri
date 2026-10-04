# Installs wheelbri's G920 driver (build it first, see README.md). Administrator. Secure Boot and test
# mode stay as they are: the package is signed with a certificate made on this PC by the WDK build
# (WDKTestCert, or WheelBri's own from new-signing-cert.ps1), which this script adds to the machine's trusted Root and
# TrustedPublisher stores (its thumbprint recorded in HKLM\SOFTWARE\wheelbri\SigningCertificates for the uninstall).
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

# The new package first: without a build, nothing old is removed (the script stops here, Windows keeps what it has).
$inf = Find-BuiltPackage $PSScriptRoot 'wheelbri-g920' $Configuration
Add-PackageCertificate $inf 'wheelbri-g920'
Remove-LegacyPackages 'wheelbri-g920' | Out-Null
Invoke-Pnputil @('/add-driver', $inf.FullName, '/install') 'installing wheelbri-g920' | Out-Null
if (@(Get-InstalledPackages 'wheelbri-g920').Count -eq 0) {
    Stop-WithError 'pnputil reported success, but Windows does not list wheelbri-g920.inf as installed.'
}
Write-Host ''
Write-Host 'Installed. Start wheelbri-service with --virtual-g920 (as Administrator) to create the G920.' -ForegroundColor Green
