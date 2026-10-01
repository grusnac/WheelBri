# Removes wheelemu's G920 driver package (and, with -RemoveCertificate, the certificate the install trusted).
# Administrator. Stop wheelemu-service first.

param([switch]$RemoveCertificate)
$ErrorActionPreference = 'Stop'

$lines = pnputil /enum-drivers
$published = $null
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match 'Published Name:\s*(oem\d+\.inf)') { $published = $Matches[1] }
    if ($lines[$i] -match 'Original Name:\s*wheelemu-g920\.inf' -and $published) {
        Write-Host "removing $published"
        pnputil /delete-driver $published /uninstall /force
    }
}
if ($RemoveCertificate) {
    foreach ($store in 'Root', 'TrustedPublisher') {
        Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $_.Subject -like '*WDKTestCert*' } | ForEach-Object {
            Write-Host "removing certificate $($_.Subject) from $store"
            Remove-Item $_.PSPath
        }
    }
}
