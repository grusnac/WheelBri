# Removes wheelemu-hide: the R3's game-controller interface goes back to Windows' own setup (input.inf) and
# nothing hides it any more. Administrator; stop wheelemu-service first.

$ErrorActionPreference = 'Stop'

$lines = pnputil /enum-drivers
$published = $null
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match 'Published Name:\s*(oem\d+\.inf)') { $published = $Matches[1] }
    if ($lines[$i] -match 'Original Name:\s*wheelemu-hide\.inf' -and $published) {
        Write-Host "removing $published"
        pnputil /delete-driver $published /uninstall /force
    }
}
# Make sure no stale "hidden" setting is left behind.
Remove-ItemProperty -Path 'HKLM:\SOFTWARE\wheelemu\hide' -Name 'AllowedPid' -ErrorAction SilentlyContinue
# Windows picks its own driver for the R3 again.
pnputil /scan-devices | Out-Null
Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'HID\VID_346E&PID_0005&MI_02*' } |
    ForEach-Object { Write-Host "$($_.InstanceId): $($_.Status), $($_.FriendlyName)" }
