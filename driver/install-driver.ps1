# Installs wheelbri's G920 driver (build it first, see README.md). Administrator. Secure Boot and test
# mode stay as they are: the package is signed with a certificate made on this PC by the WDK build
# (WDKTestCert, or WheelBri's own from new-signing-cert.ps1), which this script adds to the machine's trusted Root and
# TrustedPublisher stores (its thumbprint recorded in HKLM\SOFTWARE\wheelbri\SigningCertificates for the uninstall).
#
#   install-driver.ps1                 Release build
#   install-driver.ps1 -Configuration Debug
#
# Exit codes: 0 installed (a yellow line says when Windows wants a restart), 1 failed (the red line says why).
# The app runs it by itself at Start when Windows has no wheelbri-g920 driver (see ensureG920Driver in main.cpp).

param([string]$Configuration = 'Release')
# A 32-bit PowerShell (one started from a 32-bit prompt, e.g. some Developer PowerShell windows) sees SysWOW64,
# where pnputil and the 64-bit driver tools are missing: run this again in the 64-bit PowerShell.
if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
    & (Join-Path $env:SystemRoot 'Sysnative\WindowsPowerShell\v1.0\powershell.exe') -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -Configuration $Configuration
    exit $LASTEXITCODE
}
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
Assert-Admin

# Everything below also goes to %LOCALAPPDATA%\wheelbri\logs\wheelbri-driver-install-<date>.log: the app runs this
# without a window when Start finds the driver missing, so that file is where to look when it fails. Not through a link
# (junction) in the user's folders: then the log goes to %SystemRoot%\Temp instead.
$logFolder = Join-Path $env:LOCALAPPDATA 'wheelbri\logs'
$logName = "wheelbri-driver-install-$(Get-Date -Format yyyy-MM-dd).log"
$transcript = $false
try {
    $logsLinked = @((Split-Path $logFolder), $logFolder | Where-Object {
        $item = Get-Item -LiteralPath $_ -Force -ErrorAction SilentlyContinue
        $item -and (Test-Link $item)
    }).Count -gt 0
    if ($logsLinked) {
        Start-Transcript -Append -Path (Join-Path $env:SystemRoot "Temp\$logName") | Out-Null
    } else {
        New-Item -ItemType Directory -Force -Path $logFolder | Out-Null
        Start-Transcript -Append -Path (Join-Path $logFolder $logName) | Out-Null
    }
    $transcript = $true
} catch {
    Write-Host "no install log: $($_.Exception.Message)" -ForegroundColor Yellow
}

$exitCode = 1
try {
    # The new package first: without a build, nothing old is removed (the script stops here, Windows keeps what it has).
    $inf = Find-BuiltPackage $PSScriptRoot 'wheelbri-g920' $Configuration
    Add-PackageCertificate $inf 'wheelbri-g920'
    Remove-LegacyPackages 'wheelbri-g920' | Out-Null
    Invoke-Pnputil @('/add-driver', $inf.FullName, '/install') 'installing wheelbri-g920' | Out-Null
    if (@(Get-InstalledPackages 'wheelbri-g920').Count -eq 0) {
        Stop-WithError 'pnputil reported success, but Windows does not list wheelbri-g920.inf as installed.'
    }
    Write-Host ''
    Write-Host 'Installed. Press Start in WheelBri (or run wheelbri-service --virtual-g920 as Administrator) to create the G920.' -ForegroundColor Green
    $exitCode = 0
} catch {
    Write-Host "install-driver.ps1 failed: $($_.Exception.Message)" -ForegroundColor Red
} finally {
    # Stop-WithError ends the script with exit 1 from inside the try: this still runs.
    if ($transcript) { try { Stop-Transcript | Out-Null } catch {} }
}
exit $exitCode
