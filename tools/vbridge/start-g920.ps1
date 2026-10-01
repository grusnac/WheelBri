# Starts the virtual G920 in the right order: wheelemu-service first (if it is not running yet), then
# wheelemu-bridge in this window. Stop with Ctrl+C here, AFTER closing the game and Steam.
#
#   start-g920.ps1                 service with its --ui window, then the bridge
#   start-g920.ps1 -NoUi           service without window
#   start-g920.ps1 -StopService    also stop the service when the bridge stops
#   start-g920.ps1 -Config config\moza-r3-as-g920.ini
#   start-g920.ps1 -XboxPad buttons  also a virtual Xbox controller, for Xbox button prompts in the game's
#                                    menus (buttons: the wheel's buttons press it in menus; idle: never pressed)
#
# Asks for Administrator itself (the bridge needs it).

param(
    [switch]$NoUi,
    [switch]$StopService,
    [string]$Config = 'config\moza-r3-as-g920.ini',
    [ValidateSet('off', 'buttons', 'idle')]
    [string]$XboxPad = 'off'
)

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    $relaunch = @('-NoExit', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Config', "`"$Config`"")
    if ($NoUi) { $relaunch += '-NoUi' }
    if ($StopService) { $relaunch += '-StopService' }
    if ($XboxPad -ne 'off') { $relaunch += @('-XboxPad', $XboxPad) }
    Start-Process powershell.exe -Verb RunAs -ArgumentList $relaunch
    return
}

$service = Join-Path $root 'build\service\wheelemu-service.exe'
$bridge = Join-Path $root 'tools\vbridge\bin\Release\net10.0-windows10.0.26100.0\wheelemu-bridge.exe'
foreach ($exe in $service, $bridge) {
    if (-not (Test-Path $exe)) { Write-Host "missing: $exe (build it first)" -ForegroundColor Red; exit 1 }
}

$game = Get-Process NFS16 -ErrorAction SilentlyContinue
if ($game) {
    Write-Host 'NFS is running: close it first, the game must be started after the bridge.' -ForegroundColor Yellow
    exit 1
}
if (Get-Process wheelemu-bridge -ErrorAction SilentlyContinue) {
    Write-Host 'wheelemu-bridge is already running (use that window).' -ForegroundColor Yellow
    exit 1
}

$running = Get-Process wheelemu-service -ErrorAction SilentlyContinue
if ($running) {
    Write-Host "wheelemu-service already running (pid $($running.Id))"
} else {
    # The service's DLLs (SDL3, the C++ runtime) come from MSYS2 UCRT64.
    $ucrt = 'C:\msys64\ucrt64\bin'
    if ((Test-Path $ucrt) -and ($env:PATH -notlike "*$ucrt*")) { $env:PATH = "$ucrt;$env:PATH" }
    $serviceArgs = @('--config', $Config)
    if (-not $NoUi) { $serviceArgs += '--ui' }
    $running = Start-Process $service -ArgumentList $serviceArgs -WorkingDirectory $root -PassThru
    Write-Host "wheelemu-service started (pid $($running.Id)): $Config"
    Start-Sleep -Seconds 2
    if ($running.HasExited) {
        Write-Host "wheelemu-service stopped at once (exit code $($running.ExitCode)); see %LOCALAPPDATA%\wheelemu\logs" -ForegroundColor Red
        exit 1
    }
}

Write-Host ''
Write-Host 'Starting the bridge. When done: close the game, then Steam, then press Ctrl+C here.' -ForegroundColor Cyan
Write-Host ''
$bridgeArgs = @()
if ($XboxPad -ne 'off') { $bridgeArgs += @('--xbox-pad', $XboxPad) }
& $bridge @bridgeArgs
$bridgeExit = $LASTEXITCODE

if ($StopService) {
    Get-Process wheelemu-service -ErrorAction SilentlyContinue | Stop-Process
    Write-Host 'wheelemu-service stopped'
}
exit $bridgeExit
