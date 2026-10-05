# Removes wheelbri's G920 driver package and the virtual G920 devices left on this PC (and, with -RemoveCertificate,
# the certificate the install trusted).
# Administrator. Stop wheelbri-service first.
#
# -RemoveCertificate refuses while wheelbri-hide is installed: it is signed with the same
# certificate, and wheelbri-hide sits on the rig's devices (without the certificate they would stop working).
# It removes only the certificates that signed our packages (by thumbprint: recorded at install, or read from the
# signed catalogs), never another test certificate on this PC.

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

# Our signing certificates, looked up before the package leaves Windows' driver store. wheelemu-*: the same drivers
# under the project's old name (before the rename to wheelbri).
$thumbprints = @()
if ($RemoveCertificate) {
    $thumbprints = @(Get-SigningThumbprints @('wheelbri-g920', 'wheelbri-hide', 'wheelemu-g920', 'wheelemu-hide') @($PSScriptRoot))
}

# The virtual G920s first (also ones a crash left behind), then the package.
if ((Remove-VirtualDevices) -gt 0) { Write-Host 'Some virtual devices are still listed (see above); they go after a restart.' -ForegroundColor Yellow }
Remove-InstalledPackages 'wheelbri-g920' | Out-Null
Remove-LegacyPackages 'wheelbri-g920' | Out-Null
if (@(Get-InstalledPackages 'wheelbri-g920').Count -gt 0) { Stop-WithError 'wheelbri-g920 is still installed.' }

if ($RemoveCertificate) {
    $others = @(@('wheelbri-hide') | Where-Object { @(Get-InstalledPackages $_).Count -gt 0 })
    if ($others.Count -gt 0) {
        Stop-WithError ("Not removing the certificate: $($others -join ' and ') still use it. Run " +
                        'hide\uninstall-hide.ps1 first.')
    }
    if ($thumbprints.Count -eq 0) {
        Write-Host 'No certificate of ours found (none recorded, no signed package next to this script): nothing to remove.' -ForegroundColor Yellow
    } elseif ((Remove-SigningCertificates $thumbprints) -gt 0) {
        Stop-WithError 'Not every certificate could be removed (see above).'
    }
}
Write-Host 'Removed.' -ForegroundColor Green
