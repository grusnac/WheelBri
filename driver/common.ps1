# Shared by the driver install/uninstall scripts (dot-sourced: . "$PSScriptRoot\..\common.ps1"). Each step either
# works or stops the script with a red line saying what failed; nothing reports success after a failed step.

# pnputil exit codes that mean "done": 0, 259 (ERROR_NO_MORE_ITEMS: added, but no device to put it on now) and
# 3010 (ERROR_SUCCESS_REBOOT_REQUIRED: done, active after a restart).
$script:PnputilOk = @(0, 259, 3010)

function Stop-WithError([string]$message, [int]$code = 1) {
    Write-Host $message -ForegroundColor Red
    exit $code
}

function Assert-Admin {
    $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $admin) { Stop-WithError 'Run this from an Administrator PowerShell.' }
}

# Runs pnputil and stops the script unless it ended with one of the codes above. Returns the exit code.
function Invoke-Pnputil([string[]]$arguments, [string]$what) {
    & pnputil @arguments | ForEach-Object { Write-Host $_ }   # shown, not returned
    $code = $LASTEXITCODE
    if ($script:PnputilOk -notcontains $code) {
        Stop-WithError "$what failed: pnputil $($arguments -join ' ') exited with $code"
    }
    if ($code -eq 3010) { Write-Host "$what needs a restart of Windows to take effect." -ForegroundColor Yellow }
    return $code
}

# The built package <name>.inf + <name>.cat under <folder>\x64\<Configuration>, or stop.
function Find-BuiltPackage([string]$folder, [string]$name, [string]$configuration) {
    $out = Join-Path $folder "x64\$configuration"
    $inf = Get-ChildItem $out -Recurse -Filter "$name.inf" -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.DirectoryName "$name.cat") } | Select-Object -First 1
    if (-not $inf) { Stop-WithError "No built driver package under $out ($name.inf + .cat): build it first." }
    Write-Host "package: $($inf.DirectoryName)"
    return $inf
}

# Trusts the certificate that signed the package's catalog (Root and TrustedPublisher of this PC).
function Add-PackageCertificate($inf, [string]$name) {
    $cat = Join-Path $inf.DirectoryName "$name.cat"
    $signature = Get-AuthenticodeSignature $cat
    if (-not $signature.SignerCertificate) { Stop-WithError "The catalog is not signed: $cat" }
    $cer = Join-Path $env:TEMP "$name.cer"
    [IO.File]::WriteAllBytes($cer, $signature.SignerCertificate.Export('Cert'))
    foreach ($store in 'Root', 'TrustedPublisher') {
        try {
            Import-Certificate -FilePath $cer -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
        } catch {
            Stop-WithError "Cannot trust the signing certificate in $store`: $($_.Exception.Message)"
        }
    }
    Write-Host "trusted on this PC: $($signature.SignerCertificate.Subject) ($($signature.SignerCertificate.Thumbprint))"
}

# The published names (oemNN.inf) of the installed packages whose original file is <name>.inf. Uses DISM's view,
# which does not depend on the language of pnputil's output; falls back to pnputil's English labels.
function Get-InstalledPackages([string]$name) {
    try {
        return @(Get-WindowsDriver -Online -ErrorAction Stop |
            Where-Object { (Split-Path $_.OriginalFileName -Leaf) -ieq "$name.inf" } | ForEach-Object { $_.Driver })
    } catch {
        Write-Host "Get-WindowsDriver is not available ($($_.Exception.Message)); reading pnputil instead" -ForegroundColor Yellow
    }
    $found = @()
    $published = $null
    foreach ($line in (pnputil /enum-drivers)) {
        if ($line -match '(oem\d+\.inf)') { $published = $Matches[1] }
        if ($line -match [regex]::Escape("$name.inf") -and $published) { $found += $published; $published = $null }
    }
    return $found
}

# The same driver under the project's old name (wheelemu-<x>.inf, before the rename to wheelbri on 2026-10-03): removed
# before the new one goes on, so two hiding filters never sit on a rig device (the old one would also count as "the
# filter is there" while reading a setting nothing writes any more, so the device would stay visible). Returns how many.
function Remove-LegacyPackages([string]$name) {
    $legacy = $name -replace '^wheelbri-', 'wheelemu-'
    $packages = @(Get-InstalledPackages $legacy)
    foreach ($package in $packages) {
        Write-Host "removing $package ($legacy.inf, the old name of $name)"
        Invoke-Pnputil @('/delete-driver', $package, '/uninstall', '/force') "removing $package" | Out-Null
    }
    return $packages.Count
}

# The devices (HIDClass, present) whose driver is one of the installed copies of <name>.inf (or its old wheelemu
# name): for wheelbri-hide, the devices that have the filter, whatever they are.
function Get-DevicesUsing([string]$name) {
    $legacy = $name -replace '^wheelbri-', 'wheelemu-'
    $published = @(@(Get-InstalledPackages $name) + @(Get-InstalledPackages $legacy) | ForEach-Object { $_.ToLower() })
    if ($published.Count -eq 0) { return @() }
    # One property query for all of them (one per device is slow).
    $present = @(Get-PnpDevice -PresentOnly -Class HIDClass -ErrorAction SilentlyContinue)
    if ($present.Count -eq 0) { return @() }
    $using = @(Get-PnpDeviceProperty -InstanceId @($present | ForEach-Object { $_.InstanceId }) `
                   -KeyName 'DEVPKEY_Device_DriverInfPath' -ErrorAction SilentlyContinue |
               Where-Object { $_.Data -and ($published -contains ([string]$_.Data).ToLower()) } |
               ForEach-Object { $_.InstanceId })
    return @($present | Where-Object { $using -contains $_.InstanceId })
}

# Removes every installed copy of <name>.inf. Returns how many were removed.
function Remove-InstalledPackages([string]$name) {
    $packages = @(Get-InstalledPackages $name)
    if ($packages.Count -eq 0) {
        Write-Host "$name is not installed: nothing to remove."
        return 0
    }
    foreach ($package in $packages) {
        Write-Host "removing $package ($name)"
        Invoke-Pnputil @('/delete-driver', $package, '/uninstall', '/force') "removing $package" | Out-Null
    }
    return $packages.Count
}
