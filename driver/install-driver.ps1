# Installs wheelbri-wheel, the one driver package behind all four virtual wheels (G25, G27, G920, G923; build it first,
# see docs\building.md). Administrator. Secure Boot and test mode stay as they are: the package is signed with a
# certificate made on this PC by the WDK build (WDKTestCert, or WheelBri's own from new-signing-cert.ps1), which this
# script adds to the machine's trusted Root and TrustedPublisher stores (its thumbprint recorded in
# HKLM\SOFTWARE\wheelbri\SigningCertificates for the uninstall).
#
#   install-driver.ps1                 Release build
#   install-driver.ps1 -Configuration Debug
#
# Exit codes: 0 installed (a yellow line says when Windows wants a restart), 1 failed (the red line says why).
# The app runs it by itself at Start when Windows has no wheelbri-wheel driver (see ensureWheelDriver in main.cpp).
# It removes the same driver under its older names first (wheelbri-g920 until 2026-10-19; Get-LegacyNames).

# -LogFolder <folder>: its log in that folder (the WheelBri Helper, which runs as SYSTEM, gives a scratch folder only
# SYSTEM may change, and then adds the log to the user's own logs folder, writing as the user).
param([string]$Configuration = 'Release', [string]$LogFolder = '')
# A 32-bit PowerShell (one started from a 32-bit prompt, e.g. some Developer PowerShell windows) sees SysWOW64,
# where pnputil and the 64-bit driver tools are missing: run this again in the 64-bit PowerShell.
if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
    $forward = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath, '-Configuration', $Configuration)
    if ($LogFolder) { $forward += @('-LogFolder', $LogFolder) }   # never an empty argument: 5.1 would drop it
    & (Join-Path $env:SystemRoot 'Sysnative\WindowsPowerShell\v1.0\powershell.exe') @forward
    exit $LASTEXITCODE
}
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
Assert-Admin

# Everything below also goes to %LOCALAPPDATA%\wheelbri\logs\wheelbri-driver-install-<date>.log: the app runs this
# without a window when Start finds the driver missing, so that file is where to look when it fails. Not through a link
# (junction) in the user's folders: then the log goes to %SystemRoot%\Temp instead. With -LogFolder (the WheelBri
# Helper, as SYSTEM): only into that folder, and only when it is a plain folder there already; else no log.
$logFolder = if ($LogFolder) { $LogFolder } else { Join-Path $env:LOCALAPPDATA 'wheelbri\logs' }
$logName = "wheelbri-driver-install-$(Get-Date -Format yyyy-MM-dd).log"
$transcript = $false
try {
    $logsLinked = @((Split-Path $logFolder), $logFolder | Where-Object {
        $item = Get-Item -LiteralPath $_ -Force -ErrorAction SilentlyContinue
        $item -and (Test-Link $item)
    }).Count -gt 0
    if ($LogFolder) {
        if (-not $logsLinked -and (Test-Path -LiteralPath $LogFolder -PathType Container)) {
            Start-Transcript -Append -Path (Join-Path $LogFolder $logName) | Out-Null
            $transcript = $true
        } else {
            Write-Host "no install log: $LogFolder is not a plain folder" -ForegroundColor Yellow
        }
    } elseif ($logsLinked) {
        Start-Transcript -Append -Path (Join-Path $env:SystemRoot "Temp\$logName") | Out-Null
        $transcript = $true
    } else {
        New-Item -ItemType Directory -Force -Path $logFolder | Out-Null
        Start-Transcript -Append -Path (Join-Path $logFolder $logName) | Out-Null
        $transcript = $true
    }
} catch {
    Write-Host "no install log: $($_.Exception.Message)" -ForegroundColor Yellow
}

$exitCode = 1
try {
    # The new package first: without a build, nothing old is removed (the script stops here, Windows keeps what it has).
    $inf = Find-BuiltPackage $PSScriptRoot 'wheelbri-wheel' $Configuration
    Add-PackageCertificate $inf 'wheelbri-wheel'
    Remove-LegacyPackages 'wheelbri-wheel' | Out-Null
    $published = Add-Package $inf 'wheelbri-wheel' $true 'installing wheelbri-wheel'
    if (@(Get-InstalledPackages 'wheelbri-wheel').Count -eq 0) {
        Stop-WithError 'pnputil reported success, but Windows does not list wheelbri-wheel.inf as installed.'
    }
    # The earlier builds' copies (the devices are on the new one now; one still in use stays).
    Remove-OlderCopies 'wheelbri-wheel' $published | Out-Null
    Write-Host ''
    Write-Host 'Installed. Press Start in WheelBri (or run wheelbri-service --virtual-wheel as Administrator) to create the virtual wheel.' -ForegroundColor Green
    $exitCode = 0
} catch {
    Write-Host "install-driver.ps1 failed: $($_.Exception.Message)" -ForegroundColor Red
} finally {
    # Stop-WithError ends the script with exit 1 from inside the try: this still runs.
    if ($transcript) { try { Stop-Transcript | Out-Null } catch {} }
}
exit $exitCode
