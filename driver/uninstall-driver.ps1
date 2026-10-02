# Removes wheelemu's G920 driver package (and, with -RemoveCertificate, the certificate the install trusted).
# Administrator. Stop wheelemu-service first.
#
# -RemoveCertificate refuses while wheelemu-hide or wheelemu-xusb is installed: they are signed with the same
# certificate, and wheelemu-hide sits on the R3 itself (without the certificate the R3 would stop working).

param([switch]$RemoveCertificate)
# A 32-bit PowerShell (one started from a 32-bit prompt, e.g. some Developer PowerShell windows) sees SysWOW64,
# where pnputil and the 64-bit driver tools are missing: run this again in the 64-bit PowerShell.
if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
    & (Join-Path $env:SystemRoot 'Sysnative\WindowsPowerShell\v1.0\powershell.exe') -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -RemoveCertificate:$RemoveCertificate
    exit $LASTEXITCODE
}
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
Assert-Admin

Remove-InstalledPackages 'wheelemu-g920' | Out-Null
if (@(Get-InstalledPackages 'wheelemu-g920').Count -gt 0) { Stop-WithError 'wheelemu-g920 is still installed.' }

if ($RemoveCertificate) {
    $others = @('wheelemu-hide', 'wheelemu-xusb') | Where-Object { @(Get-InstalledPackages $_).Count -gt 0 }
    if ($others.Count -gt 0) {
        Stop-WithError ("Not removing the certificate: $($others -join ' and ') still use it. Run " +
                        'hide\uninstall-hide.ps1 and xusb\uninstall-xusb.ps1 first.')
    }
    foreach ($store in 'Root', 'TrustedPublisher') {
        Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $_.Subject -like '*WDKTestCert*' } | ForEach-Object {
            Write-Host "removing certificate $($_.Subject) from $store"
            Remove-Item $_.PSPath
        }
    }
}
Write-Host 'Removed.' -ForegroundColor Green
