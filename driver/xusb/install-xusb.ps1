# Installs wheelemu-xusb, the virtual Xbox 360 controller's driver (build it first, see ..\README.md).
# Administrator. The controller itself appears only while wheelemu-service --virtual-g920 runs. Secure Boot and test
# mode stay as they are: the package is signed with a certificate made on this PC by the WDK build
# (WDKTestCert), which this script adds to the machine's trusted Root and TrustedPublisher stores.
#
#   install-xusb.ps1                   Release build
#   install-xusb.ps1 -Configuration Debug

param([string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { Write-Host 'Run this from an Administrator PowerShell.' -ForegroundColor Red; exit 1 }

$out = Join-Path $PSScriptRoot "x64\$Configuration"
$inf = Get-ChildItem $out -Recurse -Filter 'wheelemu-xusb.inf' -ErrorAction SilentlyContinue |
    Where-Object { Test-Path (Join-Path $_.DirectoryName 'wheelemu-xusb.cat') } | Select-Object -First 1
if (-not $inf) { Write-Host "No built driver package under $out (wheelemu-xusb.inf + .cat): build it first." -ForegroundColor Red; exit 1 }
$cat = Join-Path $inf.DirectoryName 'wheelemu-xusb.cat'
Write-Host "package: $($inf.DirectoryName)"

# The certificate that signed the catalog.
$signature = Get-AuthenticodeSignature $cat
if (-not $signature.SignerCertificate) { Write-Host "The catalog is not signed: $cat" -ForegroundColor Red; exit 1 }
$cer = Join-Path $env:TEMP 'wheelemu-xusb.cer'
[IO.File]::WriteAllBytes($cer, $signature.SignerCertificate.Export('Cert'))
foreach ($store in 'Root', 'TrustedPublisher') {
    Import-Certificate -FilePath $cer -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
}
Write-Host "trusted on this PC: $($signature.SignerCertificate.Subject) ($($signature.SignerCertificate.Thumbprint))"

pnputil /add-driver $inf.FullName /install
if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 259) { Write-Host "pnputil failed ($LASTEXITCODE)" -ForegroundColor Red; exit 1 }
Write-Host ''
Write-Host 'Installed. wheelemu-service --virtual-g920 now also creates the Xbox controller (XInput).' -ForegroundColor Green
