# Builds wheelbri's UMDF drivers with MSBuild (Visual Studio + WDK, see driver/README.md) and optionally
# installs them with their own install scripts. Works from any PowerShell: MSBuild is found with vswhere.
#
#   drivers.ps1                          build g920 and hide (Release)
#   drivers.ps1 -Only g920               build one
#   drivers.ps1 -Install                 build, then install (asks for Administrator)
#   drivers.ps1 -Clean                   rebuild from scratch
#   drivers.ps1 -Configuration Debug

param(
    [string]$Configuration = 'Release',
    [string[]]$Only = @('g920', 'hide'),
    [switch]$Install,
    [switch]$Clean
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

$drivers = [ordered]@{
    g920 = @{ Project = 'driver\wheelbri-g920.vcxproj';      Installer = 'driver\install-driver.ps1'; Log = 'wheelbri-driver-install-' }
    hide = @{ Project = 'driver\hide\wheelbri-hide.vcxproj'; Installer = 'driver\hide\install-hide.ps1'; Log = 'wheelbri-hide-install-' }
}
$Only = $Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim().ToLower() } | Where-Object { $_ }
foreach ($name in $Only) {
    if (-not $drivers.Contains($name)) { Write-Host "unknown driver '$name' (g920, hide)" -ForegroundColor Red; exit 2 }
}

function Find-MSBuild {
    $cmd = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $found = & $vswhere -latest -prerelease -products * -requires Microsoft.Component.MSBuild `
            -find 'MSBuild\**\Bin\amd64\MSBuild.exe' | Select-Object -First 1
        if ($found) { return $found }
    }
    return $null
}

$msbuild = Find-MSBuild
if (-not $msbuild) { Write-Host 'MSBuild not found: install Visual Studio with the WDK (driver/README.md).' -ForegroundColor Red; exit 1 }
Write-Host "msbuild: $msbuild" -ForegroundColor Cyan

$target = if ($Clean) { 'Rebuild' } else { 'Build' }
foreach ($name in $Only) {
    $project = Join-Path $root $drivers[$name].Project
    Write-Host "building $name ($Configuration, $target)..." -ForegroundColor Cyan
    & $msbuild $project /t:$target /p:Configuration=$Configuration /p:Platform=x64 /m /nologo /v:minimal
    if ($LASTEXITCODE -ne 0) { Write-Host "$name build failed" -ForegroundColor Red; exit 1 }
}
Write-Host "built: $($Only -join ', ') (packages under driver\...\x64\$Configuration)" -ForegroundColor Green

if (-not $Install) { exit 0 }

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
foreach ($name in $Only) {
    $installer = Join-Path $root $drivers[$name].Installer
    Write-Host "installing $name..." -ForegroundColor Cyan
    if ($admin) {
        & $installer -Configuration $Configuration
        if ($LASTEXITCODE) { Write-Host "$name install failed" -ForegroundColor Red; exit 1 }
    } else {
        # One Administrator window per driver; it closes when done (never waits for Enter). On failure the end of
        # the installer's own log is shown here instead. -LogFolder: this user's logs folder, where that log is
        # looked for below; without it the installer would use the elevated account's %LOCALAPPDATA% (another one
        # when a different Administrator signs in). When that folder (or the one above it) is a link, the
        # installers write to %SystemRoot%\Temp instead.
        $logFolder = Join-Path $env:LOCALAPPDATA 'wheelbri\logs'
        $quoted = { param($text) "'" + ($text -replace "'", "''") + "'" }
        $command = "& $(& $quoted $installer) -Configuration $Configuration -LogFolder $(& $quoted $logFolder); " +
                   "exit `$LASTEXITCODE"
        try {
            $p = Start-Process powershell.exe -Verb RunAs -PassThru -Wait `
                -ArgumentList "-NoProfile -ExecutionPolicy Bypass -Command `"$command`""
        } catch {
            Write-Host "$name install was not started (Administrator refused?)" -ForegroundColor Red; exit 1
        }
        if ($p.ExitCode -ne 0) {
            $logName = "$($drivers[$name].Log)$(Get-Date -Format yyyy-MM-dd).log"
            $log = @((Join-Path $logFolder $logName), (Join-Path $env:SystemRoot "Temp\$logName") |
                Where-Object { Test-Path -LiteralPath $_ } |
                Sort-Object { (Get-Item -LiteralPath $_).LastWriteTime } -Descending) | Select-Object -First 1
            if ($log) {
                Write-Host "the end of $log :"
                Get-Content -LiteralPath $log -Tail 25 | ForEach-Object { Write-Host "  $_" }
            }
            Write-Host "$name install failed (exit $($p.ExitCode))" -ForegroundColor Red
            exit 1
        }
    }
}
Write-Host "installed: $($Only -join ', ')" -ForegroundColor Green
