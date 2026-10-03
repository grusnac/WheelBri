# Removes WheelBri: its drivers, shortcuts, the Settings > Apps entry and the install folder. Three ways to run
# it, all the same script (it asks for Administrator itself):
#   - Start menu > WheelBri > Uninstall WheelBri, or Settings > Apps > WheelBri > Uninstall;
#   - Uninstall.cmd in the install folder (double-click);
#   - Uninstall.cmd in the unpacked package: cleans up an installation whose folder is damaged or already deleted
#     (drivers, certificate, shortcuts, entry), using the package's own driver scripts.
# Logs and telemetry in %LOCALAPPDATA%\wheelbri\logs are left.
#
#   uninstall.ps1                      asks to confirm, whether to keep the config and whether to remove the certificate
#   uninstall.ps1 -KeepConfig          keep the config (copied to C:\ProgramData\wheelbri\config-backup)
#   uninstall.ps1 -RemoveCertificate   also remove the drivers' certificate
#   uninstall.ps1 -Quiet               no questions (removes the config, keeps the certificate unless -RemoveCertificate)
#   uninstall.ps1 -InstallDir D:\x     the install folder (default: the one Settings > Apps records)
#
# Exit codes: 0 removed (warnings listed at the end), 1 failed or cancelled.

param(
    [switch]$KeepConfig,
    [switch]$RemoveCertificate,
    [switch]$Quiet,
    [string]$InstallDir = ''
)

# Not inside the folder being deleted (Windows will not remove a folder that is some process's current directory).
Set-Location $env:TEMP
[Environment]::CurrentDirectory = $env:TEMP

$uninstallKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\wheelbri'
if (-not $InstallDir) {
    $recorded = (Get-ItemProperty -Path $uninstallKey -Name InstallLocation -ErrorAction SilentlyContinue).InstallLocation
    $InstallDir = if ($recorded) { $recorded } else { Join-Path $(if ($env:ProgramW6432) { $env:ProgramW6432 } else { $env:ProgramFiles }) 'WheelBri' }
}

function Forwarded {
    $list = @('-InstallDir', $InstallDir)
    if ($KeepConfig) { $list += '-KeepConfig' }
    if ($RemoveCertificate) { $list += '-RemoveCertificate' }
    if ($Quiet) { $list += '-Quiet' }
    return $list
}

if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
    & (Join-Path $env:SystemRoot 'Sysnative\WindowsPowerShell\v1.0\powershell.exe') -NoProfile -ExecutionPolicy Bypass `
        -File $PSCommandPath @(Forwarded)
    exit $LASTEXITCODE
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not ([Security.Principal.WindowsPrincipal]$identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" " +
                 ((Forwarded | ForEach-Object { if ($_ -like '-*') { $_ } else { "`"$_`"" } }) -join ' ')
    try {
        $p = Start-Process powershell.exe -Verb RunAs -ArgumentList $arguments -Wait -PassThru
        exit $p.ExitCode
    } catch {
        Write-Host 'Removing WheelBri needs Administrator, and that was refused.' -ForegroundColor Red
        if (-not $Quiet) { Read-Host 'Press Enter to close' | Out-Null }
        exit 1
    }
}

# Whether the folder is a real installation (not a package folder, not a system folder): only then is it deleted.
$InstallDir = $InstallDir.TrimEnd('\')
$folderOk = (Test-Path (Join-Path $InstallDir 'wheelbri.exe')) -and
            -not (Test-Path (Join-Path $InstallDir 'install.ps1')) -and          # the package has it, an install does not
            ($InstallDir.Length -gt 3) -and
            ($InstallDir -ine $env:ProgramFiles) -and ($InstallDir -ine $env:SystemRoot)

# The install folder is deleted at the end, this script with it: run from a copy outside it.
$here = (Resolve-Path $PSScriptRoot).Path.TrimEnd('\')
if ($folderOk -and ($here -ieq $InstallDir -or $here.StartsWith("$InstallDir\", [StringComparison]::OrdinalIgnoreCase))) {
    $temp = Join-Path $env:TEMP "wheelbri-uninstall-$PID"
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    Copy-Item $PSCommandPath $temp -Force
    Copy-Item (Join-Path $InstallDir 'driver') $temp -Recurse -Force   # the driver uninstall scripts
    & (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') -NoProfile -ExecutionPolicy Bypass `
        -File (Join-Path $temp 'uninstall.ps1') @(Forwarded)
    $code = $LASTEXITCODE
    Set-Location $env:TEMP
    Remove-Item $temp -Recurse -Force -ErrorAction SilentlyContinue
    exit $code
}

$ErrorActionPreference = 'Stop'
$logFolder = Join-Path $env:LOCALAPPDATA 'wheelbri\logs'   # with every other log
New-Item -ItemType Directory -Force -Path $logFolder | Out-Null
Start-Transcript -Append -Path (Join-Path $logFolder "wheelbri-install-$(Get-Date -Format yyyy-MM-dd).log") | Out-Null
$warnings = New-Object System.Collections.Generic.List[string]
function Warn([string]$text) { Write-Host $text -ForegroundColor Yellow; $warnings.Add($text) }
function Ask([string]$question, [bool]$default) {
    $answer = Read-Host ($question + $(if ($default) { ' [Y/n]' } else { ' [y/N]' }))
    if (-not $answer -or -not $answer.Trim()) { return $default }
    return $answer.Trim().ToLower() -in @('y', 'yes')
}
function Run-Script([string]$path, [string[]]$arguments = @()) {
    if (-not (Test-Path $path)) { Write-Host "    missing: $path"; return 1 }
    $powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    & $powershell -NoProfile -ExecutionPolicy Bypass -File $path @arguments | ForEach-Object { Write-Host "    $_" }
    return $LASTEXITCODE
}

$exitCode = 1
try {
    Write-Host 'WheelBri uninstall' -ForegroundColor Green
    if ($folderOk) {
        Write-Host "installed in: $InstallDir"
    } else {
        Write-Host "No installation in '$InstallDir': removing what is left of one (drivers, shortcuts, the Settings > Apps entry)."
    }
    $driverRoot = Join-Path $PSScriptRoot 'driver'
    if (-not (Test-Path (Join-Path $driverRoot 'common.ps1'))) {
        throw "the driver scripts are not next to this script ($driverRoot): run Uninstall.cmd from the install folder or the package."
    }

    if (-not $Quiet) {
        if (-not (Ask 'Remove WheelBri from this PC?' $true)) {
            Write-Host 'Nothing was removed.'
            throw 'cancelled'
        }
        if ($folderOk -and -not $KeepConfig) { $KeepConfig = Ask 'Keep your settings (config\*.ini) for a later install?' $false }
        if (-not $RemoveCertificate) {
            $RemoveCertificate = Ask 'Also remove the certificate the drivers were signed with from the trusted certificates?' $true
        }
    }

    # 1. A running copy: closed so it shows the real wheel again and removes its virtual devices itself.
    Write-Host '[1/5] closing a running WheelBri' -ForegroundColor Cyan
    $running = @(Get-Process -Name 'wheelbri', 'wheelbri-service', 'wheelbri-bridge' -ErrorAction SilentlyContinue)
    foreach ($process in $running) {
        if ($process.MainWindowHandle -ne 0) { [void]$process.CloseMainWindow() }
    }
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline -and @($running | Where-Object { -not $_.HasExited }).Count -gt 0) {
        Start-Sleep -Milliseconds 250
        $running | ForEach-Object { $_.Refresh() }
    }
    foreach ($process in @($running | Where-Object { -not $_.HasExited })) {
        Warn "    $($process.ProcessName) did not close within 15 s: ended"
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
    $service = Join-Path $InstallDir 'wheelbri-service.exe'
    if ($folderOk -and (Test-Path $service)) { & $service --unhide | Out-Null }   # no "hidden" setting left behind
    Write-Host "    $(if ($running.Count) { "closed $($running.Count)" } else { 'none running' })"

    # 2. Drivers: the filter first (the rig's devices go back to Windows' own driver), then the virtual devices'.
    Write-Host '[2/5] removing the drivers' -ForegroundColor Cyan
    foreach ($script in 'hide\uninstall-hide.ps1', 'uninstall-driver.ps1') {
        $code = Run-Script (Join-Path $driverRoot $script)
        if ($code -ne 0) { Warn "    $script ended with exit code $code (see above)" }
    }
    if ($RemoveCertificate) {
        $code = Run-Script (Join-Path $driverRoot 'uninstall-driver.ps1') @('-RemoveCertificate')
        if ($code -ne 0) { Warn "    the certificate was not removed (exit code $code, see above)" }
    }

    # 3. Shortcuts and the Settings > Apps entry.
    Write-Host '[3/5] removing shortcuts and the Settings > Apps entry' -ForegroundColor Cyan
    $programs = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs'
    $desktop = [Environment]::GetFolderPath('CommonDesktopDirectory')
    foreach ($name in 'WheelBri', 'wheel-emulator') {   # wheel-emulator: the name before the rename
        Remove-Item (Join-Path $programs $name) -Recurse -Force -ErrorAction SilentlyContinue   # the Start menu folder
        Remove-Item (Join-Path $programs "$name.lnk") -Force -ErrorAction SilentlyContinue      # older installs
        Remove-Item (Join-Path $desktop "$name.lnk") -Force -ErrorAction SilentlyContinue
    }
    Remove-Item $uninstallKey -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item 'HKLM:\SOFTWARE\wheelbri' -Recurse -Force -ErrorAction SilentlyContinue   # the hide setting
    # The same two under the project's old name (wheelemu, before the rename), if an upgrade left them.
    Remove-Item 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\wheelemu' -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item 'HKLM:\SOFTWARE\wheelemu' -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host '    done'

    # 4. Config, if asked to keep it.
    Write-Host '[4/5] settings' -ForegroundColor Cyan
    if ($folderOk -and $KeepConfig) {
        $backup = Join-Path $env:ProgramData 'wheelbri\config-backup'
        New-Item -ItemType Directory -Force -Path $backup | Out-Null
        Copy-Item (Join-Path $InstallDir 'config\*') $backup -Force
        Write-Host "    kept in $backup (copy them into the config folder after a new install)"
    } else {
        Write-Host '    removed with the program'
    }

    # 5. The program.
    Write-Host '[5/5] removing the program files' -ForegroundColor Cyan
    if ($folderOk) {
        Remove-Item $InstallDir -Recurse -Force -ErrorAction SilentlyContinue
        if (Test-Path $InstallDir) {
            Warn "    $InstallDir could not be removed completely (a file still in use?): delete it after a restart"
        } else {
            Write-Host '    done'
        }
    } else {
        Write-Host '    no install folder to remove'
    }
    $exitCode = 0
} catch {
    if ($_.Exception.Message -ne 'cancelled') { Write-Host "Uninstall failed: $($_.Exception.Message)" -ForegroundColor Red }
} finally {
    Write-Host ''
    if ($exitCode -eq 0) {
        Write-Host 'WheelBri is removed.' -ForegroundColor Green
        if ($warnings.Count -gt 0) {
            Write-Host 'Things to look at:' -ForegroundColor Yellow
            $warnings | ForEach-Object { Write-Host "  $($_.Trim())" -ForegroundColor Yellow }
        }
        Write-Host "Logs stay in $logFolder."
    }
    Stop-Transcript | Out-Null
    if (-not $Quiet) { Read-Host 'Press Enter to close' | Out-Null }
}
exit $exitCode
