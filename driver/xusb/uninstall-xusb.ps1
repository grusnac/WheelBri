# Removes wheelemu-xusb, the virtual Xbox controller's driver. Administrator; stop wheelemu-service first.

$ErrorActionPreference = 'Stop'

$lines = pnputil /enum-drivers
$published = $null
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match 'Published Name:\s*(oem\d+\.inf)') { $published = $Matches[1] }
    if ($lines[$i] -match 'Original Name:\s*wheelemu-xusb\.inf' -and $published) {
        Write-Host "removing $published"
        pnputil /delete-driver $published /uninstall /force
    }
}
