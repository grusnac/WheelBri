# Removes WheelBri: the WheelBri Helper service, its drivers and virtual devices, shortcuts, the Settings > Apps
# entry, its registry entries and the install folder. Three ways to run
# it, all the same script (it asks for Administrator itself):
#   - Start menu > WheelBri > Uninstall WheelBri, or Settings > Apps > WheelBri > Uninstall;
#   - Uninstall.cmd in the install folder (double-click);
#   - Uninstall.cmd in the unpacked package: cleans up an installation whose folder is damaged or already deleted
#     (drivers, certificate, shortcuts, entry), using the package's own driver scripts.
# Logs and telemetry in %LOCALAPPDATA%\wheelbri\logs are left.
#
#   uninstall.ps1                      asks to confirm, whether to keep the config and whether to remove the certificate
#   uninstall.ps1 -KeepConfig          keep the config (copied to %LOCALAPPDATA%\wheelbri\config-backup)
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

# A full path without a trailing backslash, resolved here (before the Set-Location below, and before elevation, after
# which a relative path would be resolved against System32).
if ($InstallDir) {
    $InstallDir = [IO.Path]::GetFullPath($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($InstallDir))
}

# Not inside the folder being deleted (Windows will not remove a folder that is some process's current directory).
Set-Location $env:TEMP
[Environment]::CurrentDirectory = $env:TEMP

$uninstallKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\wheelbri'
if (-not $InstallDir) {
    $recorded = (Get-ItemProperty -Path $uninstallKey -Name InstallLocation -ErrorAction SilentlyContinue).InstallLocation
    $InstallDir = if ($recorded) { $recorded } else { Join-Path $(if ($env:ProgramW6432) { $env:ProgramW6432 } else { $env:ProgramFiles }) 'WheelBri' }
}
if ($InstallDir.Length -gt 3) { $InstallDir = $InstallDir.TrimEnd('\') }

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
                 ((Forwarded | ForEach-Object { if ($_ -like '-*') { $_ } else { "`"$($_ -replace '\\$', '\\')`"" } }) -join ' ')   # "D:\" -> "D:\\"
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
$folderOk = (Test-Path -LiteralPath (Join-Path $InstallDir 'wheelbri.exe')) -and
            -not (Test-Path -LiteralPath (Join-Path $InstallDir 'install.ps1')) -and          # the package has it, an install does not
            ($InstallDir.Length -gt 3) -and
            ($InstallDir -ine $env:ProgramFiles) -and ($InstallDir -ine $env:SystemRoot)

# The install folder is deleted at the end, this script with it: run from a copy outside it. The copy runs as
# Administrator, so it goes where only Administrators and SYSTEM can write (not %TEMP%, where another program of this
# user could swap the files between the copy and the run): a new folder under %SystemRoot%\Temp with a random name,
# locked down before anything is copied into it. If that cannot be done the uninstall stops (no fallback).
$here = (Resolve-Path -LiteralPath $PSScriptRoot).Path.TrimEnd('\')
if ($folderOk -and ($here -ieq $InstallDir -or $here.StartsWith("$InstallDir\", [StringComparison]::OrdinalIgnoreCase))) {
    $temp = Join-Path $env:SystemRoot "Temp\wheelbri-uninstall-$([guid]::NewGuid().ToString('N'))"
    try {
        New-Item -ItemType Directory -Path $temp -ErrorAction Stop | Out-Null   # new: fails if it exists
        & icacls $temp /inheritance:r /grant:r '*S-1-5-32-544:(OI)(CI)F' '*S-1-5-18:(OI)(CI)F' | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "icacls could not restrict it to Administrators and SYSTEM (exit code $LASTEXITCODE)" }
        if (@(Get-ChildItem -LiteralPath $temp -Force).Count -ne 0) { throw 'something was put in it before it was locked down' }
        Copy-Item -LiteralPath $PSCommandPath -Destination $temp -Force -ErrorAction Stop
        Copy-Item -LiteralPath (Join-Path $InstallDir 'driver') -Destination $temp -Recurse -Force -ErrorAction Stop   # the driver uninstall scripts
    } catch {
        Write-Host "Cannot prepare the uninstaller's copy in $temp`: $($_.Exception.Message)" -ForegroundColor Red
        Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
        if (-not $Quiet) { Read-Host 'Press Enter to close' | Out-Null }
        exit 1
    }
    & (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') -NoProfile -ExecutionPolicy Bypass `
        -File (Join-Path $temp 'uninstall.ps1') @(Forwarded)
    $code = $LASTEXITCODE
    Set-Location $env:TEMP
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
    exit $code
}

$ErrorActionPreference = 'Stop'
$logFolder = Join-Path $env:LOCALAPPDATA 'wheelbri\logs'   # with every other log
# Not through a link (junction) in the user's folders, as Administrator; and a log that cannot start is no reason
# to stop: the window still says what happens.
$logsLinked = @((Split-Path $logFolder), $logFolder | Where-Object {
    $item = Get-Item -LiteralPath $_ -Force -ErrorAction SilentlyContinue
    $item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)
}).Count -gt 0
try {
    if ($logsLinked) { $logFolder = Join-Path $env:SystemRoot 'Temp' } else { New-Item -ItemType Directory -Force -Path $logFolder | Out-Null }
    Start-Transcript -Append -Path (Join-Path $logFolder "wheelbri-install-$(Get-Date -Format yyyy-MM-dd).log") | Out-Null
} catch {
    Write-Host "(no log file: $($_.Exception.Message))"
}
$warnings = New-Object System.Collections.Generic.List[string]
function Warn([string]$text) { Write-Host $text -ForegroundColor Yellow; $warnings.Add($text) }
function Ask([string]$question, [bool]$default) {
    $answer = Read-Host ($question + $(if ($default) { ' [Y/n]' } else { ' [y/N]' }))
    if (-not $answer -or -not $answer.Trim()) { return $default }
    return $answer.Trim().ToLower() -in @('y', 'yes')
}
function Run-Script([string]$path, [string[]]$arguments = @()) {
    if (-not (Test-Path -LiteralPath $path)) { Write-Host "    missing: $path"; return 1 }
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
    . (Join-Path $driverRoot 'common.ps1')   # Remove-InstallFolder

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
    # The WheelBri Helper service: stopped (it removes the virtual G920 it made and shows the rig again) and removed,
    # before its program and the drivers go.
    if (Get-Service -Name 'WheelBriHelper' -ErrorAction SilentlyContinue) {
        if (Remove-HelperService) { Write-Host '    removed the WheelBri Helper service' }
        else { Warn '    the WheelBri Helper service could not be removed: sc.exe delete WheelBriHelper (as Administrator)' }
    }
    # A driver installer the app started at Start (it keeps running when the app closes): finished first.
    if (-not (Wait-DriverInstallers)) { Warn '    a driver installer WheelBri started is still running after 2 minutes: removing anyway' }
    $service = Join-Path $InstallDir 'wheelbri-service.exe'
    if ($folderOk -and (Test-Path $service)) { & $service --unhide | Out-Null }   # no "hidden" setting left behind
    Write-Host "    $(if ($running.Count) { "closed $($running.Count)" } else { 'none running' })"

    # 2. Drivers: the filter first (the rig's devices go back to Windows' own driver), then the virtual devices'.
    Write-Host '[2/5] removing the drivers' -ForegroundColor Cyan
    foreach ($script in 'hide\uninstall-hide.ps1', 'uninstall-driver.ps1') {
        $code = Run-Script (Join-Path $driverRoot $script)
        if ($code -ne 0) { Warn "    $script ended with exit code $code (see above)" }
    }
    $certificateRemoved = $false
    if ($RemoveCertificate) {
        $code = Run-Script (Join-Path $driverRoot 'uninstall-driver.ps1') @('-RemoveCertificate')
        if ($code -ne 0) { Warn "    the certificate was not removed (exit code $code, see above)" } else { $certificateRemoved = $true }
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
    if ($certificateRemoved) {
        Remove-Item 'HKLM:\SOFTWARE\wheelbri' -Recurse -Force -ErrorAction SilentlyContinue   # the hide setting
    } else {
        # Everything but SigningCertificates: the record of the certificate stays with the certificate, so that removing
        # it later (clean-pc.ps1, uninstall-driver.ps1 -RemoveCertificate) finds exactly that one.
        Get-ChildItem 'HKLM:\SOFTWARE\wheelbri' -ErrorAction SilentlyContinue | Where-Object { $_.PSChildName -ne 'SigningCertificates' } |
            Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
        Remove-ItemProperty -Path 'HKLM:\SOFTWARE\wheelbri' -Name '*' -ErrorAction SilentlyContinue   # LogFolder
        if (@(Get-ChildItem 'HKLM:\SOFTWARE\wheelbri' -ErrorAction SilentlyContinue).Count -eq 0) {
            Remove-Item 'HKLM:\SOFTWARE\wheelbri' -Force -ErrorAction SilentlyContinue
        }
    }
    # The drivers' UMDF registrations (some removals leave them) and DirectInput's entries for the G920 the app writes
    # (its name and type; a real G920 gets them again when plugged in). HKCU: the account this runs as.
    foreach ($name in 'wheelbri', 'wheelemu') {
        foreach ($part in 'hide', 'g920') {
            Remove-Item "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\WUDF\Services\${name}_$part" -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
    # DirectInput's entries for the virtual G920 (C262) and G25 (C299).
    foreach ($ids in 'VID_046D&PID_C262', 'VID_046D&PID_C299') {
        foreach ($key in "HKLM:\SYSTEM\CurrentControlSet\Control\MediaProperties\PrivateProperties\Joystick\OEM\$ids",
                         "HKCU:\System\CurrentControlSet\Control\MediaProperties\PrivateProperties\Joystick\OEM\$ids",
                         "HKCU:\System\CurrentControlSet\Control\MediaProperties\PrivateProperties\DirectInput\$ids") {
            Remove-Item -LiteralPath $key -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
    # The same two under the project's old name (wheelemu, before the rename), if an upgrade left them.
    Remove-Item 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\wheelemu' -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item 'HKLM:\SOFTWARE\wheelemu' -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host '    done'

    # 4. Config, if asked to keep it.
    Write-Host '[4/5] settings' -ForegroundColor Cyan
    if ($folderOk -and $KeepConfig) {
        $config = Join-Path $InstallDir 'config'
        if (Test-Path -LiteralPath $config -PathType Container) {
            $backup = Join-Path $env:LOCALAPPDATA 'wheelbri\config-backup'   # the user's folder, like every setting
            New-Item -ItemType Directory -Force -Path $backup | Out-Null
            Copy-Item (Join-Path $config '*') $backup -Force
            Write-Host "    kept in $backup (copy them into the config folder after a new install)"
        } else {
            Warn "    no config folder in $InstallDir`: no settings to keep"
        }
    } else {
        Write-Host '    removed with the program'
    }

    # 5. The program.
    Write-Host '[5/5] removing the program files' -ForegroundColor Cyan
    if ($folderOk) {
        # The files in its installed-files.txt and WheelBri's subfolders, then the folders left empty (an install from
        # before that list: the whole folder, which holds wheelbri.exe and no install.ps1, see $folderOk).
        if (-not (Remove-InstallFolder $InstallDir)) {
            Warn ("    $InstallDir could not be removed completely (a file still in use, or files WheelBri did not put " +
                  'there): delete what is left after a restart')
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
    try { Stop-Transcript | Out-Null } catch { }
    if (-not $Quiet) { Read-Host 'Press Enter to close' | Out-Null }
}
exit $exitCode
