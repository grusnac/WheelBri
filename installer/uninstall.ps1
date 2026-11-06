# Removes WheelBri: the WheelBri Helper service, its drivers and virtual devices, shortcuts, the Settings > Apps
# entry, its registry entries and the install folder. Three ways to run
# it, all the same script (it asks for Administrator itself):
#   - Start menu > WheelBri > Uninstaller (Uninstaller.exe in the install folder: wheelbri-setup.exe --uninstall), or Settings > Apps > WheelBri > Uninstall;
#   - Uninstall.cmd in the install folder (double-click);
#   - Uninstall.cmd in the unpacked package: cleans up an installation whose folder is damaged or already deleted
#     (drivers, certificate, shortcuts, entry), using the package's own driver scripts.
# The user's own data in %LOCALAPPDATA%\wheelbri: the rig, mappings and settings are kept unless -RemoveSettings, the
# logs (and dumps) unless -RemoveLogs. The shim is taken out of the games the app put it into (its shim-games.txt)
# unless -KeepShims: another mod's DLL the app chained gets its name back, as the app's Remove shim does.
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
#   uninstall.ps1 -RemoveSettings      also remove the rig, mappings and settings (%LOCALAPPDATA%\wheelbri, but logs\)
#   uninstall.ps1 -RemoveLogs          also remove the logs (%LOCALAPPDATA%\wheelbri\logs, dumps, and the drivers'
#                                      C:\ProgramData\wheelbri); this uninstall's own log then goes to %TEMP%
#   uninstall.ps1 -KeepShims           leave the shim in the game folders
#   uninstall.ps1 -UserLocalAppData X  whose data (default: this user's %LOCALAPPDATA%; passed on when it elevates,
#                                      so an Administrator who is another account does not get that account's)
#   uninstall.ps1 -Progress            also prints lines for wheelbri-setup.exe's checklist: @@STEP <n> <count> <text>,
#                                      @@ITEM <key> <state> <text>, @@NOTE <text> (on the running row), @@WARN <text>,
#                                      @@RESTART (Windows must restart to finish), @@RESULT <exit code>
#
# Exit codes: 0 removed (warnings listed at the end), 1 failed or cancelled.

param(
    [switch]$KeepConfig,
    [switch]$RemoveCertificate,
    [switch]$Quiet,
    [string]$InstallDir = '',
    [switch]$RemoveSettings,
    [switch]$RemoveLogs,
    [switch]$KeepShims,
    [string]$UserLocalAppData = '',
    [switch]$Progress
)
# Whose data: this user's, decided before elevation (after it, %LOCALAPPDATA% may be another account's).
if (-not $UserLocalAppData) { $UserLocalAppData = $env:LOCALAPPDATA }
$UserLocalAppData = $UserLocalAppData.TrimEnd('\')

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
    if ($RemoveSettings) { $list += '-RemoveSettings' }
    if ($RemoveLogs) { $list += '-RemoveLogs' }
    if ($KeepShims) { $list += '-KeepShims' }
    if ($Progress) { $list += '-Progress' }
    $list += @('-UserLocalAppData', $UserLocalAppData)
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
$dataFolder = Join-Path $UserLocalAppData 'wheelbri'
$logFolder = Join-Path $dataFolder 'logs'   # with every other log
# Not through a link (junction) in the user's folders, as Administrator; and a log that cannot start is no reason
# to stop: the window still says what happens.
$logsLinked = @((Split-Path $logFolder), $logFolder | Where-Object {
    $item = Get-Item -LiteralPath $_ -Force -ErrorAction SilentlyContinue
    $item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)
}).Count -gt 0
try {
    # Logs that are going: this log goes to %TEMP% instead, so that it outlives them.
    $transcriptFolder = if ($logsLinked) { Join-Path $env:SystemRoot 'Temp' } elseif ($RemoveLogs) { $env:TEMP } else { $logFolder }
    if (-not $logsLinked -and -not $RemoveLogs) { New-Item -ItemType Directory -Force -Path $logFolder | Out-Null }
    $transcriptName = if ($RemoveLogs) { "wheelbri-uninstall-$(Get-Date -Format yyyy-MM-dd-HHmmss).log" } else { "wheelbri-install-$(Get-Date -Format yyyy-MM-dd).log" }
    Start-Transcript -Append -Path (Join-Path $transcriptFolder $transcriptName) | Out-Null
} catch {
    Write-Host "(no log file: $($_.Exception.Message))"
}
$warnings = New-Object System.Collections.Generic.List[string]
function Warn([string]$text) {
    Write-Host $text -ForegroundColor Yellow
    $warnings.Add($text)
    if ($Progress) { Write-Host "@@WARN $($text.Trim())" }
}
$stepCount = 7
function Step([int]$n, [string]$text) {
    Write-Host "[$n/$stepCount] $text" -ForegroundColor Cyan
    if ($Progress) { Write-Host "@@STEP $n $stepCount $text" }
}
# A part of a step, and a note on the running row, for wheelbri-setup.exe's checklist (-Progress only).
function Item([string]$key, [string]$state, [string]$text) { if ($Progress) { Write-Host "@@ITEM $key $state $text" } }
function Note([string]$text) { if ($Progress) { Write-Host "@@NOTE $text" } }
$restartNeeded = $false
function Ask([string]$question, [bool]$default) {
    $answer = Read-Host ($question + $(if ($default) { ' [Y/n]' } else { ' [y/N]' }))
    if (-not $answer -or -not $answer.Trim()) { return $default }
    return $answer.Trim().ToLower() -in @('y', 'yes')
}
function Run-Script([string]$path, [string[]]$arguments = @()) {
    if (-not (Test-Path -LiteralPath $path)) { Write-Host "    missing: $path"; return 1 }
    $powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    & $powershell -NoProfile -ExecutionPolicy Bypass -File $path @arguments | ForEach-Object {
        Write-Host "    $_"
        if ("$_" -match 'after a restart|restart Windows|3010') { $script:restartNeeded = $true }
    }
    return $LASTEXITCODE
}

# The game folders the app put the shim into: "game<TAB>name<TAB>folder" lines of shim-games.txt (UTF-8) in the
# user's data folder, as [name, folder]; folders no longer there are left out. The same
# reading as scripts/make/clean-pc.ps1 (keep them in step).
function Get-ShimGames {
    $games = @()
    $list = Join-Path (Join-Path $UserLocalAppData 'wheelbri') 'shim-games.txt'
    if (-not (Test-Path -LiteralPath $list -PathType Leaf)) { return @() }
    foreach ($line in @([IO.File]::ReadAllLines($list, [Text.Encoding]::UTF8))) {
        $parts = $line.TrimEnd("`r") -split "`t"
        if ($parts.Count -eq 3 -and $parts[0] -eq 'game' -and $parts[2] -and (Test-Path -LiteralPath $parts[2] -PathType Container)) {
            $games += [pscustomobject]@{ Name = $parts[1]; Folder = $parts[2].TrimEnd('\') }
        }
    }
    return @($games | Group-Object { $_.Folder.ToLowerInvariant() } | ForEach-Object { $_.Group[0] })
}
# A shim DLL of ours: "wheelbri" is in it.
function Test-OurShim([string]$path) {
    try {
        $text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($path))
        return $text.Contains('wheelbri')
    } catch { return $false }
}
# Takes the shim out of one game folder, as the app's Remove shim does: our dinput8.dll / dinput8Hooked.dll (and their
# .off copies) and wheelbri-shim.ini go; another mod's dll the app chained (dinput8-chain.dll) gets its name back
# (dinput8Hooked.dll next to the Ultimate ASI Loader, else dinput8.dll). Returns the problems (empty: done).
function Remove-ShimFrom([string]$folder) {
    $problems = @()
    foreach ($name in 'dinput8.dll', 'dinput8.dll.off', 'dinput8Hooked.dll', 'dinput8Hooked.dll.off') {
        $path = Join-Path $folder $name
        if ((Test-Path -LiteralPath $path -PathType Leaf) -and (Test-OurShim $path)) {
            try { Remove-Item -LiteralPath $path -Force -ErrorAction Stop }
            catch { $problems += "$name ($($_.Exception.Message))" }
        }
    }
    Remove-Item -LiteralPath (Join-Path $folder 'wheelbri-shim.ini') -Force -ErrorAction SilentlyContinue
    $chain = Join-Path $folder 'dinput8-chain.dll'
    if (Test-Path -LiteralPath $chain -PathType Leaf) {
        $loader = Join-Path $folder 'dinput8.dll'
        $asi = $false
        if (Test-Path -LiteralPath $loader -PathType Leaf) {
            try {
                $text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($loader))
                $asi = $text.Contains('IsUltimateASILoader') -and -not $text.Contains('IsWheelBriShim')
            } catch { $asi = $false }
        }
        $to = Join-Path $folder $(if ($asi) { 'dinput8Hooked.dll' } else { 'dinput8.dll' })
        if (Test-Path -LiteralPath $to) { $problems += "dinput8-chain.dll not renamed back: $([IO.Path]::GetFileName($to)) is there" }
        else {
            try { Move-Item -LiteralPath $chain -Destination $to -ErrorAction Stop }
            catch { $problems += "dinput8-chain.dll ($($_.Exception.Message))" }
        }
    }
    return $problems
}

# Removes a folder of the user's data without following links (a junction in it is removed as a link, never what it
# points to: this runs as Administrator). Returns whether it is gone.
function Remove-DataFolder([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return $true }
    $item = Get-Item -LiteralPath $path -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        [IO.Directory]::Delete($path, $false)   # the link only
    } else {
        Get-ChildItem -LiteralPath $path -Force | ForEach-Object {
            if ($_.PSIsContainer) { [void](Remove-DataFolder $_.FullName) }
            else { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue }
        }
        Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
    }
    return -not (Test-Path -LiteralPath $path)
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
function Move-ConfigsToBackup($files, [string]$localAppData, [datetime]$now) {   # $localAppData: the user's
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
        if (-not $RemoveSettings) { $RemoveSettings = -not (Ask 'Keep your rig, mappings and settings for a later install?' $true) }
        if (-not $RemoveLogs) { $RemoveLogs = Ask 'Remove the logs too?' $true }
        if ($folderOk -and -not $KeepConfig -and -not $RemoveSettings) { $KeepConfig = Ask 'Keep the changed configs of the install folder (config\*.ini)?' $false }
        if ($withDrivers -and -not $RemoveCertificate) {
            $RemoveCertificate = Ask 'Also remove the certificate the drivers were signed with from the trusted certificates?' $true
        }
    }

    # 1. A running copy: closed so it shows the real wheel again and removes its virtual devices itself.
    Step 1 'closing a running WheelBri'
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
    # The WheelBri Helper service: stopped (it removes the virtual wheel it made and shows the rig again) and removed,
    # before its program and the drivers go.
    if (Get-Service -Name 'WheelBriHelper' -ErrorAction SilentlyContinue) {
        Item 'helper' 'running' 'WheelBri Helper'
        if (Remove-HelperService) { Write-Host '    removed the WheelBri Helper service'; Item 'helper' 'done' 'removed' }
        else {
            Item 'helper' 'warn' 'could not be removed'
            Warn '    the WheelBri Helper service could not be removed: sc.exe delete WheelBriHelper (as Administrator)'
        }
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
    Note $(if ($running.Count) { "closed $($running.Count)" } else { 'none running' })

    # 2. The shim in the games the app put it into (shim-games.txt), unless asked to leave it.
    Step 2 'taking the shim out of games'
    $shimGames = @(Get-ShimGames)
    if ($KeepShims) {
        Write-Host "    left in $($shimGames.Count) game folder(s) (asked)"
        Note 'left (asked)'
    } elseif ($shimGames.Count -eq 0) {
        Write-Host '    no game has it'
        Note 'no game has it'
    } else {
        $cleaned = 0
        foreach ($game in $shimGames) {
            $problems = @(Remove-ShimFrom $game.Folder)
            if ($problems.Count -eq 0) { Write-Host "    $($game.Name): removed ($($game.Folder))"; $cleaned++ }
            else { Warn "    $($game.Name) ($($game.Folder)): $($problems -join '; ') (is the game running?)" }
        }
        Note "$cleaned of $($shimGames.Count) game(s)"
    }

    # 2. Drivers: the filter first (the rig's devices go back to Windows' own driver), then the virtual devices'.
    Step 3 'removing the drivers'
    if (-not $withDrivers) { Write-Host '    none on this Windows (the in-game shim way only)' }
    $driverScripts = @()
    if ($withDrivers) { $driverScripts = @('hide\uninstall-hide.ps1', 'uninstall-driver.ps1') }
    foreach ($script in $driverScripts) {
        $key = if ($script -like 'hide\*') { 'hide-driver' } else { 'wheel-driver' }
        Item $key 'running' $script
        $code = Run-Script (Join-Path $driverRoot $script)
        if ($code -ne 0) { Item $key 'warn' "exit code $code"; Warn "    $script ended with exit code $code (see above)" }
        else { Item $key 'done' 'removed' }
    }
    $certificateRemoved = $false
    if ($RemoveCertificate -and $withDrivers) {
        Item 'certificate' 'running' 'certificate'
        $code = Run-Script (Join-Path $driverRoot 'uninstall-driver.ps1') @('-RemoveCertificate')
        if ($code -ne 0) { Item 'certificate' 'warn' "exit code $code"; Warn "    the certificate was not removed (exit code $code, see above)" }
        else { $certificateRemoved = $true; Item 'certificate' 'done' 'removed' }
    }

    # 3. Shortcuts and the Settings > Apps entry.
    Step 4 'removing shortcuts, the Settings > Apps entry and registry entries'
    $programs = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs'
    $desktop = [Environment]::GetFolderPath('CommonDesktopDirectory')
    foreach ($name in 'WheelBri', 'wheel-emulator') {   # wheel-emulator: the name before the rename
        Remove-Item (Join-Path $programs $name) -Recurse -Force -ErrorAction SilentlyContinue   # the Start menu folder
        Remove-Item (Join-Path $programs "$name.lnk") -Force -ErrorAction SilentlyContinue      # older installs
        Remove-Item (Join-Path $desktop "$name.lnk") -Force -ErrorAction SilentlyContinue
    }
    # The Start menu folder the installer was told to use (install.ps1 -StartMenuFolder), recorded in the entry.
    $menuFolder = (Get-ItemProperty -Path $uninstallKey -Name StartMenuFolder -ErrorAction SilentlyContinue).StartMenuFolder
    if ($menuFolder -and $menuFolder -notmatch '\.\.' -and -not [IO.Path]::IsPathRooted($menuFolder)) {
        Remove-Item (Join-Path $programs $menuFolder) -Recurse -Force -ErrorAction SilentlyContinue
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
    foreach ($part in 'hide', 'wheel', 'g920') {   # g920: the wheel driver's name until 2026-10-19
        Remove-Item "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\WUDF\Services\wheelbri_$part" -Recurse -Force -ErrorAction SilentlyContinue
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
    Write-Host '    done'

    # 4. Config: kept if asked; with -Quiet (no one to ask) the changed ones are moved to a backup, never deleted.
    Step 5 'your rig, mappings and settings'
    if ($folderOk -and $Quiet -and -not $KeepConfig -and -not $RemoveSettings) {
        $edited = @(Get-EditedConfigs $InstallDir)
        if ($edited.Count -gt 0) {
            $backup = Move-ConfigsToBackup $edited $UserLocalAppData (Get-Date)
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
            $backup = Join-Path $dataFolder 'config-backup'   # the user's folder, like every setting
            New-Item -ItemType Directory -Force -Path $backup | Out-Null
            Copy-Item (Join-Path $config '*') $backup -Force
            Write-Host "    kept in $backup (copy them into the config folder after a new install)"
        } else {
            Warn "    no config folder in $InstallDir`: no settings to keep"
        }
    } else {
        Write-Host '    the install folder''s configs: removed with the program'
    }
    if ($RemoveSettings) {
        # Everything in the data folder but logs\ and dumps\ (the next step's).
        $gone = $true
        if (Test-Path -LiteralPath $dataFolder) {
            Get-ChildItem -LiteralPath $dataFolder -Force | Where-Object { $_.Name -notin @('logs', 'dumps') } | ForEach-Object {
                if ($_.PSIsContainer) { if (-not (Remove-DataFolder $_.FullName)) { $gone = $false } }
                else { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue; if (Test-Path -LiteralPath $_.FullName) { $gone = $false } }
            }
        }
        if ($gone) { Write-Host "    removed from $dataFolder"; Note 'removed' }
        else { Warn "    some of your settings in $dataFolder could not be removed (in use?): delete them by hand" }
    } else {
        Write-Host "    kept in $dataFolder"
        Note 'kept'
    }

    # 6. Logs.
    Step 6 'logs'
    if ($RemoveLogs) {
        $gone = (Remove-DataFolder $logFolder) -and (Remove-DataFolder (Join-Path $dataFolder 'dumps'))
        # The drivers' own folder, used when they could not write to the user's (driver\Driver.cpp kFallbackLogFolder).
        [void](Remove-DataFolder (Join-Path $env:ProgramData 'wheelbri'))
        if ($gone) { Write-Host '    removed'; Note 'removed' }
        else { Warn "    some logs in $logFolder could not be removed (in use?): delete them by hand" }
        # The data folder itself, once nothing is left in it.
        if ((Test-Path -LiteralPath $dataFolder) -and @(Get-ChildItem -LiteralPath $dataFolder -Force).Count -eq 0) {
            Remove-Item -LiteralPath $dataFolder -Force -ErrorAction SilentlyContinue
        }
    } else {
        Write-Host "    kept in $logFolder"
        Note 'kept'
    }

    # 7. The program.
    Step 7 'removing the program files'
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
    if ($_.Exception.Message -ne 'cancelled') {
        Write-Host "Uninstall failed: $($_.Exception.Message)" -ForegroundColor Red
        if ($Progress) { Write-Host "@@FAIL $($_.Exception.Message)" }
    }
} finally {
    Write-Host ''
    if ($exitCode -eq 0) {
        Write-Host 'WheelBri is removed.' -ForegroundColor Green
        if ($warnings.Count -gt 0) {
            Write-Host 'Things to look at:' -ForegroundColor Yellow
            $warnings | ForEach-Object { Write-Host "  $($_.Trim())" -ForegroundColor Yellow }
        }
        if ($RemoveLogs) { Write-Host "This uninstall's log: $(Join-Path $transcriptFolder $transcriptName)" }
        else { Write-Host "Logs stay in $logFolder." }
        if ($restartNeeded) { Write-Host 'Restart Windows to finish removing the drivers.' -ForegroundColor Yellow }
    }
    if ($Progress) {
        if ($restartNeeded -and $exitCode -eq 0) { Write-Host '@@RESTART' }
        Write-Host "@@RESULT $exitCode"
    }
    try { Stop-Transcript | Out-Null } catch { }
    if (-not $Quiet) { Read-Host 'Press Enter to close' | Out-Null }
}
exit $exitCode
