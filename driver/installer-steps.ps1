# The two steps of the installer that need Windows PowerShell 5.1's cmdlets (Get-WindowsDriver, Get-CimInstance):
# wheelbri-setup.exe runs this from the install folder, on Windows 11 only (the only Windows it installs drivers on).
#
#   installer-steps.ps1 -RemoveUnusedCertificates   the certificates earlier installs trusted that no installed driver
#                                                   uses any more go (common.ps1 Remove-UnusedSigningCertificates)
#   installer-steps.ps1 -WaitForDriverInstallers    waits (up to 2 minutes) for a driver install the app started at
#                                                   Start, before the uninstaller removes anything
#
# Exit codes: 0 done, 1 not done (the lines above say why).

param([switch]$RemoveUnusedCertificates, [switch]$WaitForDriverInstallers)
if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
    & (Join-Path $env:SystemRoot 'Sysnative\WindowsPowerShell\v1.0\powershell.exe') -NoProfile -ExecutionPolicy Bypass `
        -File $PSCommandPath -RemoveUnusedCertificates:$RemoveUnusedCertificates -WaitForDriverInstallers:$WaitForDriverInstallers
    exit $LASTEXITCODE
}
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
Assert-Admin

$code = 0
if ($RemoveUnusedCertificates -and (Remove-UnusedSigningCertificates) -gt 0) { $code = 1 }
if ($WaitForDriverInstallers -and -not (Wait-DriverInstallers)) { $code = 1 }
exit $code
