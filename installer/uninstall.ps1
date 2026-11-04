# Removes WheelBri: the WheelBri Helper service, its drivers and virtual devices, shortcuts, the Settings > Apps
# entry, its registry entries and the install folder. Three ways to run
# it, all the same script (it asks for Administrator itself):
#   - Start menu > WheelBri > Uninstall WheelBri, or Settings > Apps > WheelBri > Uninstall;
#   - Uninstall.cmd in the install folder (double-click);
#   - Uninstall.cmd in the unpacked package: cleans up an installation whose folder is damaged or already deleted
#     (drivers, certificate, shortcuts, entry), using the package's own driver scripts.
# The user's own data in %LOCALAPPDATA%\wheelbri (mappings, rigs, app settings, logs and telemetry) is never removed.
# Before Windows 11 the installer puts no drivers and no Helper in (the in-game shim way only), so there is nothing of
# them to remove there.
#
#   uninstall.ps1                      asks to confirm, whether to keep the config and whether to remove the certificate
#   uninstall.ps1 -KeepConfig          keep the config (copied to %LOCALAPPDATA%\wheelbri\config-backup)
#   uninstall.ps1 -RemoveCertificate   also remove the drivers' certificate
#   uninstall.ps1 -Quiet               no questions (Settings > Apps' quiet uninstall): the configs in the install
#                                      folder that were changed after install are moved to
#                                      %LOCALAPPDATA%\wheelbri\config-backup-<yyyy-MM-dd-HHmmss>, never deleted; the
#                                      certificate stays unless -RemoveCertificate
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

# DirectInput's OEM entries WheelBri writes while a virtual wheel exists (service/src/VirtualWheelDevice.cpp
# writeDirectInputEntries), under Joystick\OEM\<ids> in HKLM and HKCU: OEMName (the name below, kVirtualWheels'
# oemName in core/src/HidWheelDevice.cpp), OEMData, for the G923 an Axes key marked with a "WheelBri" value, and for
# the G25/G27 in Logitech order an OEMForceFeedback key (Logitech's older-wheel driver) and Axes key, both marked the
# same way (removed at Stop; still there after a crash). A real Logitech wheel has entries under the same ids
# (Windows', Logitech's software's): those stay. By target: G25, G27,
# G920, G923.
$oemNames = [ordered]@{
    'VID_046D&PID_C299' = 'Logitech G25 Racing Wheel USB'
    'VID_046D&PID_C29B' = 'Logitech G27 Racing Wheel USB'
    'VID_046D&PID_C262' = 'Logitech G920 Driving Force Racing Wheel USB'
    'VID_046D&PID_C26E' = 'Logitech G923 Racing Wheel for Xbox One and PC'
}
# What of one OEM key is WheelBri's, from what is in it: OEMName and OEMData when OEMName is the name WheelBri writes
# (the app removes OEMData at stop and leaves OEMName), the Axes and OEMForceFeedback keys when they have the
# "WheelBri" value. The key itself
# goes only when nothing else is left in it (Remove-OemEntries). No registry here, so it is tested on its own.
function Get-OemRemoval([string]$oemName, [string]$ourName, [bool]$axesMarked, [bool]$ffbMarked = $false) {
    $values = @()
    if ($oemName -and $oemName -ceq $ourName) { $values = @('OEMName', 'OEMData') }
    return [pscustomobject]@{ Values = $values; Axes = $axesMarked; Ffb = $ffbMarked }
}
# Whether the key under <key> named <subKey> carries the "WheelBri" value (WheelBri wrote it).
function Test-OemMarked([string]$key, [string]$subKey) {
    $item = Get-Item -LiteralPath "$key\$subKey" -ErrorAction SilentlyContinue
    return [bool]($item -and (@($item.GetValueNames()) -contains 'WheelBri'))
}
# Removes WheelBri's OEM entries under <key> (Get-OemRemoval) and the key when it is then empty. Returns what went.
function Remove-OemEntries([string]$key, [string]$ourName) {
    $item = Get-Item -LiteralPath $key -ErrorAction SilentlyContinue
    if (-not $item) { return @() }
    $removal = Get-OemRemoval ([string]$item.GetValue('OEMName')) $ourName (Test-OemMarked $key 'Axes') `
                              (Test-OemMarked $key 'OEMForceFeedback')
    $removed = @()
    foreach ($value in $removal.Values) {
        if (@($item.GetValueNames()) -contains $value) {
            Remove-ItemProperty -LiteralPath $key -Name $value -Force -ErrorAction SilentlyContinue
            $removed += $value
        }
    }
    if ($removal.Axes) {
        Remove-Item -LiteralPath "$key\Axes" -Recurse -Force -ErrorAction SilentlyContinue
        $removed += 'Axes'
    }
    if ($removal.Ffb) {
        Remove-Item -LiteralPath "$key\OEMForceFeedback" -Recurse -Force -ErrorAction SilentlyContinue
        $removed += 'OEMForceFeedback'
    }
    $item = Get-Item -LiteralPath $key -ErrorAction SilentlyContinue
    if ($removed.Count -gt 0 -and $item -and $item.ValueCount -eq 0 -and $item.SubKeyCount -eq 0) {
        Remove-Item -LiteralPath $key -Force -ErrorAction SilentlyContinue
        $removed += '(the key, left empty)'
    }
    return $removed
}

# The configs in <installDir>\config that the user changed: each *.ini whose SHA-256 is not the one install.ps1
# recorded for the default it put there (default-configs.txt); every *.ini when there is no record (an install from
# before it), since nothing then tells the defaults apart. The defaults' .ini.new copies are not counted.
function Get-EditedConfigs([string]$installDir) {
    $config = Join-Path $installDir 'config'
    if (-not (Test-Path -LiteralPath $config -PathType Container)) { return @() }
    $record = Join-Path $installDir 'default-configs.txt'
    $defaults = @{}
    if (Test-Path -LiteralPath $record -PathType Leaf) {
        foreach ($line in @(Get-Content -LiteralPath $record)) {
            $parts = @($line -split "`t")
            if ($parts.Count -eq 2) { $defaults[$parts[0].Trim()] = $parts[1].Trim() }
        }
    }
    return @(Get-ChildItem -LiteralPath $config -File -Force | Where-Object { $_.Extension -ieq '.ini' } |
        Where-Object {
            $known = $defaults["config\$($_.Name)"]
            -not $known -or (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash -ne $known
        })
}
# Moves <files> into a new <localAppData>\wheelbri\config-backup-<yyyy-MM-dd-HHmmss> and returns that folder. Stops
# (throws) when one cannot be moved, before the program folder is removed.
function Move-ConfigsToBackup($files, [string]$localAppData, [datetime]$now) {
    $backup = Join-Path $localAppData ('wheelbri\config-backup-' + $now.ToString('yyyy-MM-dd-HHmmss'))
    New-Item -ItemType Directory -Force -Path $backup -ErrorAction Stop | Out-Null
    foreach ($file in $files) {
        Move-Item -LiteralPath $file.FullName -Destination (Join-Path $backup $file.Name) -Force -ErrorAction Stop
    }
    return $backup
}

# The drivers and the certificate exist only on Windows 11 (install.ps1 installs them only there).
$withDrivers = [Environment]::OSVersion.Version.Build -ge 22000

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
        if ($withDrivers -and -not $RemoveCertificate) {
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
    if ($withDrivers) {
        # A driver installer the app started at Start (it keeps running when the app closes): finished first.
        if (-not (Wait-DriverInstallers)) {
            Warn '    a driver installer WheelBri started is still running after 2 minutes: removing anyway'
        }
        $service = Join-Path $InstallDir 'wheelbri-service.exe'
        if ($folderOk -and (Test-Path $service)) { & $service --unhide | Out-Null }   # no "hidden" setting left behind
    }
    Write-Host "    $(if ($running.Count) { "closed $($running.Count)" } else { 'none running' })"

    # 2. Drivers: the filter first (the rig's devices go back to Windows' own driver), then the virtual devices'.
    Write-Host '[2/5] removing the drivers' -ForegroundColor Cyan
    if (-not $withDrivers) { Write-Host '    none on this Windows (the in-game shim way only)' }
    $driverScripts = @()
    if ($withDrivers) { $driverScripts = @('hide\uninstall-hide.ps1', 'uninstall-driver.ps1') }
    foreach ($script in $driverScripts) {
        $code = Run-Script (Join-Path $driverRoot $script)
        if ($code -ne 0) { Warn "    $script ended with exit code $code (see above)" }
    }
    $certificateRemoved = $false
    if ($RemoveCertificate -and $withDrivers) {
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
    # Firefox's gamepad support, when the installer turned it off (install.ps1 6b): only that preference goes, for the
    # user it was set for; any other preference or policy there stays. Before HKLM\SOFTWARE\wheelbri, which records it.
    $gamepadUser = (Get-ItemProperty 'HKLM:\SOFTWARE\wheelbri' -Name BrowserGamepadPolicy -ErrorAction SilentlyContinue).BrowserGamepadPolicy
    if ($gamepadUser) {
        $firefoxKey = "Registry::HKEY_USERS\$gamepadUser\Software\Policies\Mozilla\Firefox"
        $value = (Get-ItemProperty -LiteralPath $firefoxKey -Name Preferences -ErrorAction SilentlyContinue).Preferences
        if (-not (Test-Path -LiteralPath "Registry::HKEY_USERS\$gamepadUser")) {
            Warn ("    Firefox's gamepad support stays off: the Windows user it was turned off for is not signed in " +
                  "now (remove dom.gamepad.enabled from HKEY_USERS\$gamepadUser\Software\Policies\Mozilla\Firefox " +
                  'Preferences, or reinstall and uninstall while signed in as that user)')
        } elseif ($null -ne $value) {
            try { $preferences = (@($value) -join "`n") | ConvertFrom-Json } catch { $preferences = $null }
            $setting = if ($preferences) { $preferences.'dom.gamepad.enabled' } else { $null }
            if ($setting -and $setting.Value -eq $false) {
                $preferences.PSObject.Properties.Remove('dom.gamepad.enabled')
                if (@($preferences.PSObject.Properties).Count -eq 0) {
                    Remove-ItemProperty -LiteralPath $firefoxKey -Name Preferences -ErrorAction SilentlyContinue
                } else {
                    New-ItemProperty -LiteralPath $firefoxKey -Name Preferences -PropertyType MultiString -Force `
                        -Value @($preferences | ConvertTo-Json -Depth 8 -Compress) | Out-Null
                }
                # Firefox's policy key, then Mozilla's: removed only when nothing else is left in them.
                foreach ($key in $firefoxKey, (Split-Path -Path $firefoxKey -Parent)) {
                    $item = Get-Item -LiteralPath $key -ErrorAction SilentlyContinue
                    if ($item -and $item.ValueCount -eq 0 -and $item.SubKeyCount -eq 0) {
                        Remove-Item -LiteralPath $key -Force -ErrorAction SilentlyContinue
                    }
                }
                Write-Host '    Firefox: gamepad support on again (from its next start)'
            }
        }
    }
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
    # The drivers' UMDF registrations (some removals leave them).
    foreach ($name in 'wheelbri', 'wheelemu') {
        foreach ($part in 'hide', 'wheel', 'g920') {   # g920: the wheel driver's name until 2026-10-19
            Remove-Item "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\WUDF\Services\${name}_$part" -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
    # DirectInput's OEM entries WheelBri wrote for its virtual G25, G27, G920 and G923 ($oemNames), and only those: a
    # real Logitech wheel's stay. HKCU: the account this runs as.
    foreach ($ids in $oemNames.Keys) {
        foreach ($hive in 'HKLM:\SYSTEM', 'HKCU:\System') {
            $key = "$hive\CurrentControlSet\Control\MediaProperties\PrivateProperties\Joystick\OEM\$ids"
            $removed = @(Remove-OemEntries $key $oemNames[$ids])
            if ($removed.Count -gt 0) { Write-Host "    $key`: removed $($removed -join ', ')" }
        }
    }
    # The same two under the project's old name (wheelemu, before the rename), if an upgrade left them.
    Remove-Item 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\wheelemu' -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item 'HKLM:\SOFTWARE\wheelemu' -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host '    done'

    # 4. Config: kept if asked; with -Quiet (no one to ask) the changed ones are moved to a backup, never deleted.
    Write-Host '[4/5] settings' -ForegroundColor Cyan
    if ($folderOk -and $Quiet -and -not $KeepConfig) {
        $edited = @(Get-EditedConfigs $InstallDir)
        if ($edited.Count -gt 0) {
            $backup = Move-ConfigsToBackup $edited $env:LOCALAPPDATA (Get-Date)
            $names = ($edited | ForEach-Object { $_.Name }) -join ', '
            $which = if (Test-Path -LiteralPath (Join-Path $InstallDir 'default-configs.txt')) { 'changed configs' }
                     else { 'configs (this install recorded no defaults to tell changed ones by)' }
            Warn "    moved your $which to $backup`: $names"
        } else {
            Write-Host '    none changed: removed with the program'
        }
    } elseif ($folderOk -and $KeepConfig) {
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
