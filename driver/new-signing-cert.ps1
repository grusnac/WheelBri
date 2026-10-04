# Creates WheelBri's own certificate for signing the drivers built on this PC, instead of the WDK test certificate
# (WDKTestCert) that Visual Studio makes and that other driver projects on this PC may share: removing "our"
# certificate then can never take another project's trust with it. Code signing only, private key not exportable, in
# your own store (Cert:\CurrentUser\My), valid 5 years. Run once in a normal PowerShell (no Administrator needed), then
# rebuild both drivers and install them again (install-driver.ps1 / install-hide.ps1 trust the new certificate).
#
#   new-signing-cert.ps1           creates the certificate (or reuses a valid one) and writes signing-certificate.props
#   new-signing-cert.ps1 -Remove   deletes signing-certificate.props: builds sign with the WDK test certificate again
#                                  (the certificate stays in your store; certmgr.msc removes it)
#
# Both .vcxproj files import driver\signing-certificate.props when it exists (it names this certificate as their
# TestCertificate); without it they sign with the WDK test certificate as before, so a build never depends on this
# script. The file is for this PC only (git-ignored, driver\.gitignore): the certificate is only in this PC's store.
#
# Exit codes: 0 done (the thumbprint is printed), 1 failed (the red line says why).

param([switch]$Remove)
$ErrorActionPreference = 'Stop'
$subject = 'CN=WheelBri driver signing (test)'
$props = Join-Path $PSScriptRoot 'signing-certificate.props'

if ($Remove) {
    Remove-Item $props -Force -ErrorAction SilentlyContinue
    Write-Host "Removed $props`: the drivers are signed with the WDK test certificate again (rebuild them)." -ForegroundColor Green
    exit 0
}

try {
    # One made earlier and still valid for a while: reused, so the drivers already installed keep working.
    $now = Get-Date
    $certificate = Get-ChildItem Cert:\CurrentUser\My | Where-Object {
        $_.Subject -eq $subject -and $_.HasPrivateKey -and $_.NotAfter -gt $now.AddDays(30) -and
        @($_.EnhancedKeyUsageList | Where-Object { $_.ObjectId -eq '1.3.6.1.5.5.7.3.3' }).Count -gt 0   # code signing
    } | Sort-Object NotAfter -Descending | Select-Object -First 1
    if ($certificate) {
        Write-Host "Using the existing certificate $subject (valid until $($certificate.NotAfter.ToString('yyyy-MM-dd')))."
    } else {
        $certificate = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject -KeyExportPolicy NonExportable `
            -CertStoreLocation Cert:\CurrentUser\My -NotAfter $now.AddYears(5)
        Write-Host "Created $subject in Cert:\CurrentUser\My (valid until $($certificate.NotAfter.ToString('yyyy-MM-dd')))."
    }

    # The WDK reads TestCertificate as "<subject> | <thumbprint>" (what Visual Studio writes when a certificate is
    # picked from the store) and signs with the certificate of that thumbprint from Cert:\CurrentUser\My.
    $xml = @"
<?xml version="1.0" encoding="utf-8"?>
<!-- Written by driver\new-signing-cert.ps1 for this PC only (git-ignored): the drivers are signed with WheelBri's own
     test certificate from Cert:\CurrentUser\My. Delete this file (new-signing-cert.ps1 -Remove) to sign with the WDK
     test certificate (WDKTestCert) again. -->
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup>
    <TestCertificate>CN="WheelBri driver signing (test)" | $($certificate.Thumbprint)</TestCertificate>
  </PropertyGroup>
</Project>
"@
    [IO.File]::WriteAllText($props, ($xml -replace "`r`n", "`n" -replace "`n", "`r`n"), (New-Object Text.UTF8Encoding($false)))
    Write-Host "Wrote $props"
    Write-Host ''
    Write-Host "Thumbprint: $($certificate.Thumbprint)" -ForegroundColor Green
    Write-Host 'Next: rebuild both drivers (scripts/make/drivers.sh), then install them again (drivers.sh -Install or the installer).'
    Write-Host 'If a build then fails to sign, new-signing-cert.ps1 -Remove goes back to the WDK test certificate.'
    exit 0
} catch {
    Write-Host "new-signing-cert.ps1 failed: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
