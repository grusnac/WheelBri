# Removes everything WheelBri (internal name wheelbri; wheelemu before the rename) has put on this PC, so the next
# install starts from a clean PC. It lists what it found, asks once, then removes it. It asks for Administrator itself.
# Every name below is looked for under both names: $names = wheelbri and the old wheelemu.
#
#   clean-pc.ps1                          list, ask, remove
#   clean-pc.ps1 -Yes                     no question
# It never waits for Enter at the end: run without Administrator, it goes on in an Administrator window that closes
# when done, and this window then shows that window's log.
#   clean-pc.ps1 -KeepData                keep %LOCALAPPDATA%\wheelbri (rigs, configs, app.ini, logs, telemetry) and
#                                         C:\ProgramData\wheelbri (logs of older versions), and the old-named ones
#   clean-pc.ps1 -GameFolder "D:\Games\Need for Speed"
#                                         also take the shim out of a game folder (dinput8.dll / dinput8.dll.off when
#                                         they are ours, wheelbri-shim.ini, wheelemu-shim.ini); several folders:
#                                         comma-separated
#   clean-pc.ps1 -KeepThirdParty          keep HidHide and HIDMaestro (drivers and devices), which the project used
#                                         before it had its own drivers; by default they are removed too, so the PC
#                                         has nothing left from any earlier version (-ThirdParty, the old switch, is
#                                         still accepted and changes nothing)
#
# What it removes: the WheelBri Helper service (WheelBriHelper); running WheelBri processes; the virtual devices
# (SWD\wheelbri\..., SWD\wheelemu\...) and the HID devices under them, including ones Windows keeps half-removed;
# the driver packages under both names (the hide filter first, then g920); every device our drivers are on (found
# by their driver description, so also when the package is already gone: the rig's devices with the hide filter,
# whatever they are) is reinstalled on Windows' own driver; the drivers' UMDF registrations (WUDF\Services); the certificates that signed our driver packages (by
# thumbprint: recorded at install in HKLM\SOFTWARE\wheelbri\SigningCertificates, or read from the signed catalogs of
# the installed packages, the install folders and this repository's builds; never another test certificate) from the
# machine's Root and TrustedPublisher stores; the install folder (%ProgramFiles%\WheelBri, an older
# %ProgramFiles%\wheelemu, or where Settings > Apps says: the files in its installed-files.txt and WheelBri's
# subfolders, or all of it for an install from before that list), its shortcuts and its Settings > Apps entries;
# HKLM\SOFTWARE\wheelbri and \wheelemu (the hide setting); the installer's and uninstaller's copies in %TEMP% and
# %SystemRoot%\Temp (wheelbri-setup-*, wheelbri-uninstall-*, and install logs written there when the logs folder was a
# link);
# DirectInput's entries for VID_046D&PID_C262 and PID_C299 (the G920's and G25's name and force feedback driver,
# which the service writes; a real one gets them again when plugged in); the app's data folders unless -KeepData.
# It never touches this repository (build\, dist\) or the WDK certificate in your own store that signs builds.
#
# Exit codes: 0 clean (yellow lines say what to look at, e.g. a restart), 1 failed or cancelled.

param(
    [switch]$Yes,
    [switch]$KeepData,
    [switch]$ThirdParty,       # old switch: removing them is now the default
    [switch]$KeepThirdParty,
    [string[]]$GameFolder = @(),
    [string]$UserLocalAppData = '',  # the user's %LOCALAPPDATA%, passed on when the script elevates itself
    [string]$LogPath = ''            # the log, chosen by the window that elevated it (it shows the log afterwards)
)

if (-not $UserLocalAppData) { $UserLocalAppData = $env:LOCALAPPDATA }
$ThirdParty = -not $KeepThirdParty
# Game folders as full paths without a trailing backslash, before they are passed on: after elevation a relative path
# would be resolved against System32, and "D:\Games\" quoted on a command line escapes its own closing quote.
$GameFolder = @($GameFolder | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ } | ForEach-Object {
    $full = [IO.Path]::GetFullPath($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($_))
    if ($full.Length -gt 3) { $full.TrimEnd('\') } else { $full }
})

function Forwarded {
    $list = @('-UserLocalAppData', $UserLocalAppData)
    if ($LogPath) { $list += '-LogPath'; $list += $LogPath }
    if ($Yes) { $list += '-Yes' }
    if ($KeepData) { $list += '-KeepData' }
    if ($KeepThirdParty) { $list += '-KeepThirdParty' }
    if ($GameFolder.Count -gt 0) { $list += '-GameFolder'; $list += ($GameFolder -join ',') }
    return $list
}

# 64-bit PowerShell (pnputil and the driver tools are missing from a 32-bit one's view).
if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
    & (Join-Path $env:SystemRoot 'Sysnative\WindowsPowerShell\v1.0\powershell.exe') -NoProfile -ExecutionPolicy Bypass `
        -File $PSCommandPath @(Forwarded)
    exit $LASTEXITCODE
}

# Administrator: start again elevated, in its own window, and pass its exit code on.
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not ([Security.Principal.WindowsPrincipal]$identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    # The Administrator window closes when it is done (no "press Enter"): its log is shown here afterwards.
    $LogPath = Join-Path $env:TEMP "wheelbri-clean-pc-$(Get-Date -Format yyyy-MM-dd-HHmmss).log"
    $arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" " +
                 ((Forwarded | ForEach-Object { if ($_ -like '-*') { $_ } else { "`"$($_ -replace '\\$', '\\')`"" } }) -join ' ')   # "D:\" -> "D:\\"
    Write-Host 'Cleaning the PC needs Administrator: answer the Windows prompt; it goes on in a new window.'
    try {
        $p = Start-Process powershell.exe -Verb RunAs -ArgumentList $arguments -Wait -PassThru
        if (Test-Path -LiteralPath $LogPath) {
            # Its transcript without PowerShell's header and footer blocks.
            $inHeader = $false
            foreach ($line in @(Get-Content -LiteralPath $LogPath)) {
                if ($line -eq ('*' * 22)) { $inHeader = -not $inHeader; continue }
                if (-not $inHeader -and $line -notlike 'Windows PowerShell transcript*') { Write-Host $line }
            }
        } else {
            Write-Host "(the Administrator window wrote no log: exit code $($p.ExitCode))"
        }
        exit $p.ExitCode
    } catch {
        Write-Host 'Administrator was refused: nothing was removed.' -ForegroundColor Red
        exit 1
    }
}
# "a,b" from the elevation above (or typed with commas) -> separate folders.
$GameFolder = @($GameFolder | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })

Set-Location $env:TEMP
$ErrorActionPreference = 'Continue'
# The drivers' shared helpers: the signing certificates' thumbprints and the install folder's removal.
$commonScript = Join-Path $PSScriptRoot '..\..\driver\common.ps1'
if (-not (Test-Path $commonScript)) {
    Write-Host "$commonScript is missing: run this from the repository (scripts\make)." -ForegroundColor Red
    exit 1
}
. $commonScript
$log = if ($LogPath) { $LogPath } else { Join-Path $env:TEMP "wheelbri-clean-pc-$(Get-Date -Format yyyy-MM-dd-HHmmss).log" }
# A log is a help, not a must: if it cannot start, say so and go on (the window still explains what happens).
try { Start-Transcript -Path $log | Out-Null } catch { Write-Host "(no log file: $($_.Exception.Message))"; $log = '(none)' }
$warnings = New-Object System.Collections.Generic.List[string]
function Warn([string]$text) { Write-Host "    $text" -ForegroundColor Yellow; $warnings.Add($text) }
function Step([string]$text) { Write-Host $text -ForegroundColor Cyan }

# --- What is there ----------------------------------------------------------------------------------------------

$names = @('wheelbri', 'wheelemu')   # the name, and the project's old name (before the rename)
# Removal order: the hide filter on the rig's devices first.
$ourInfs = @(foreach ($part in 'hide', 'g920') { foreach ($name in $names) { "$name-$part.inf" } })
function Test-ThirdPartyName([string]$text) { return $text -match 'hidhide|hidmaestro' }

# Installed driver packages: objects with Driver (oemNN.inf) and Original (the package's own .inf name). DISM's view
# does not depend on pnputil's language; pnputil's output is the fallback (published name first in each block).
function Get-DriverPackages {
    try {
        return @(Get-WindowsDriver -Online -ErrorAction Stop | ForEach-Object {
            [pscustomobject]@{ Driver = $_.Driver; Original = (Split-Path $_.OriginalFileName -Leaf) } })
    } catch {
        Write-Host "    (Get-WindowsDriver not available, reading pnputil: $($_.Exception.Message))"
    }
    $packages = @()
    $infs = @()
    foreach ($line in @(pnputil /enum-drivers) + '') {
        if (-not $line.Trim()) {
            if ($infs.Count -ge 2) { $packages += [pscustomobject]@{ Driver = $infs[0]; Original = $infs[1] } }
            $infs = @()
        } elseif ($line -match '([\w\-\.]+\.inf)\s*$') {
            $infs += $Matches[1]
        }
    }
    return $packages
}

function Get-InstallFolders {
    $folders = @()
    foreach ($name in $names) {
        $recorded = (Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\$name" `
                         -Name InstallLocation -ErrorAction SilentlyContinue).InstallLocation
        if ($recorded -and (Test-Path (Join-Path $recorded "$name.exe")) -and
            -not (Test-Path (Join-Path $recorded 'install.ps1')) -and $recorded.TrimEnd('\').Length -gt 3) {
            $folders += $recorded.TrimEnd('\')   # an install, not a package folder
        }
    }
    # Ours by name, even when damaged; also under Program Files (x86), where a 32-bit PowerShell once installed it.
    foreach ($root in @($env:ProgramW6432, $env:ProgramFiles, ${env:ProgramFiles(x86)}) | Where-Object { $_ } | Sort-Object -Unique) {
        foreach ($name in @('WheelBri') + $names) {
            $path = Join-Path $root $name
            if (Test-Path $path) { $folders += $path }
        }
    }
    return @($folders | Sort-Object -Unique)
}

function Get-Shortcuts {
    $roots = @((Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs'),
               [Environment]::GetFolderPath('CommonDesktopDirectory'),
               [Environment]::GetFolderPath('Programs'), [Environment]::GetFolderPath('Desktop'))
    $found = @()
    foreach ($root in $roots) {
        foreach ($name in 'WheelBri', 'WheelBri.lnk', 'wheel-emulator', 'wheel-emulator.lnk') {
            $path = Join-Path $root $name
            if (Test-Path $path) { $found += $path }
        }
    }
    return @($found | Sort-Object -Unique)
}

$diKeys = @(foreach ($ids in 'VID_046D&PID_C262', 'VID_046D&PID_C299', 'VID_046D&PID_C29B') {   # virtual G920, G25, G27
    "HKCU:\System\CurrentControlSet\Control\MediaProperties\PrivateProperties\Joystick\OEM\$ids"
    "HKLM:\SYSTEM\CurrentControlSet\Control\MediaProperties\PrivateProperties\Joystick\OEM\$ids"
    "HKCU:\System\CurrentControlSet\Control\MediaProperties\PrivateProperties\DirectInput\$ids"
})
$otherKeys = @(foreach ($name in $names) {
    "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\$name"; "HKLM:\SOFTWARE\$name"
    # The UMDF host's registration of each driver (UmdfService in the INFs), left behind by some removals.
    foreach ($part in 'hide', 'g920') { "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\WUDF\Services\${name}_$part" }
})

# The installer's unpacked package and the uninstaller's copy (Setup.cmd, uninstall.ps1), left in %TEMP%.
function Get-TempCopies {
    # %SystemRoot%\Temp: where uninstall.ps1 runs its copy (only Administrators may write there).
    $roots = @($env:TEMP, (Join-Path $UserLocalAppData 'Temp'), (Join-Path $env:SystemRoot 'Temp')) |
             Where-Object { $_ -and (Test-Path $_) } | Sort-Object -Unique
    return @(foreach ($root in $roots) {
        foreach ($name in $names) {
            Get-ChildItem $root -Directory -Filter "$name-setup-*" -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName }
            Get-ChildItem $root -Directory -Filter "$name-uninstall-*" -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName }
            # Install logs written here when the user's logs folder was a link (install.ps1, uninstall.ps1,
            # install-driver.ps1, install-hide.ps1).
            Get-ChildItem $root -File -Filter "$name-*install-*.log" -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName }
        }
    }) | Sort-Object -Unique
}

# A shim file of ours: the DLL has "wheelbri" (or the old "wheelemu") in it.
function Test-OurShim([string]$path) {
    try {
        $text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($path))
        return @($names | Where-Object { $text.Contains($_) }).Count -gt 0
    } catch { return $false }
}
function Get-ShimFiles {
    $found = @()
    foreach ($folder in $GameFolder) {
        # -LiteralPath throughout: a folder name with [ or ] is not a wildcard.
        if (-not (Test-Path -LiteralPath $folder)) { Warn "game folder not found: $folder"; continue }
        foreach ($name in 'dinput8.dll', 'dinput8.dll.off') {
            $path = Join-Path $folder $name
            if ((Test-Path -LiteralPath $path) -and (Test-OurShim $path)) { $found += $path }
        }
        foreach ($name in $names) {
            $ini = Join-Path $folder "$name-shim.ini"
            if (Test-Path -LiteralPath $ini) { $found += $ini }
        }
    }
    return $found
}

Write-Host 'WheelBri: clean this PC for a fresh install' -ForegroundColor Green
Write-Host "log: $log"
Write-Host ''
Write-Host 'Looking for what WheelBri left on this PC...'

$helperService = Get-Service -Name 'WheelBriHelper' -ErrorAction SilentlyContinue
$processes = @(Get-Process -Name (@(foreach ($name in $names) { $name; "$name-service"; "$name-bridge"; "$name-helper" }) +
                                 @('ClutchProbe', 'FfbProbe')) -ErrorAction SilentlyContinue)
$allDevices = @(Get-PnpDevice -ErrorAction SilentlyContinue)   # present and not present (half-removed ones too)
$ourDevices = @($allDevices | Where-Object { $id = $_.InstanceId; @($names | Where-Object { $id -like "SWD\$_\*" }).Count -gt 0 })
# Two properties of every device in one query (one query per device takes minutes): its parent, for the HID devices
# under our virtual ones, and its driver's description, for every device on one of our drivers. Ours say "wheelbri"
# (or "wheelemu"): "... (wheelbri: hidden from games ...)" on a device with the hide filter, "... (wheelbri)" on the
# virtual G920. Found this way even when the driver package is already gone (an orphaned filter).
$parents = @{}
$onOurDriver = @{}
if ($allDevices.Count -gt 0) {
    foreach ($property in @(Get-PnpDeviceProperty -InstanceId @($allDevices | ForEach-Object { $_.InstanceId }) `
                                -KeyName 'DEVPKEY_Device_Parent', 'DEVPKEY_Device_DriverDesc' -ErrorAction SilentlyContinue)) {
        if (-not $property.Data) { continue }
        if ($property.KeyName -eq 'DEVPKEY_Device_Parent') { $parents[$property.InstanceId] = [string]$property.Data }
        elseif ([string]$property.Data -match 'wheelbri|wheelemu') { $onOurDriver[$property.InstanceId] = [string]$property.Data }
    }
}
$ourIds = @($ourDevices | ForEach-Object { $_.InstanceId })
$ourChildren = @($allDevices | Where-Object { $parents[$_.InstanceId] -and ($ourIds -contains $parents[$_.InstanceId]) })
$ourDevices = @($ourChildren) + @($ourDevices)   # children first
# The real devices on our drivers (the rig's, with the hide filter): put back on Windows' own driver.
$ourIds = @($ourDevices | ForEach-Object { $_.InstanceId })
$filteredDevices = @($allDevices | Where-Object { $onOurDriver.ContainsKey($_.InstanceId) -and ($ourIds -notcontains $_.InstanceId) })
$thirdDevices = @($allDevices | Where-Object { (Test-ThirdPartyName "$($_.InstanceId) $($_.FriendlyName)") })
$packages = @(Get-DriverPackages)
$ourPackages = @(foreach ($inf in $ourInfs) { $packages | Where-Object { $_.Original -ieq $inf } })
$thirdPackages = @($packages | Where-Object { Test-ThirdPartyName $_.Original })
$installFolders = @(Get-InstallFolders)
# Only the certificates that signed our packages (by thumbprint), looked up before the packages are removed.
$signers = @(Get-SigningThumbprints @($ourInfs | ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_) }) `
                                    (@($installFolders) + @(Join-Path $PSScriptRoot '..\..\driver')))
$certificates = @(foreach ($store in 'Root', 'TrustedPublisher') {
    foreach ($thumbprint in $signers) {
        $certificate = Get-Item "Cert:\LocalMachine\$store\$thumbprint" -ErrorAction SilentlyContinue
        if ($certificate) { [pscustomobject]@{ Store = $store; Certificate = $certificate } }
    }
})
$tempCopies = @(Get-TempCopies)
$shortcuts = @(Get-Shortcuts)
$keys = @(($diKeys + $otherKeys) | Where-Object { Test-Path $_ })
$shimFiles = @(Get-ShimFiles)
$dataFolders = @()
if (-not $KeepData) {
    $dataFolders = @(foreach ($name in $names) { (Join-Path $UserLocalAppData $name); (Join-Path $env:ProgramData $name) }) |
                   Where-Object { Test-Path $_ }
    $dataFolders = @($dataFolders)
}

$plan = @()
if ($helperService) { $plan += "service     WheelBriHelper (WheelBri Helper, $($helperService.Status))" }
$plan += $processes | ForEach-Object { "process     $($_.ProcessName) (pid $($_.Id))" }
$plan += $ourDevices | ForEach-Object { "device      $($_.InstanceId) ($($_.Status))" }
$plan += $filteredDevices | ForEach-Object { "on driver   $($_.InstanceId) ($($_.FriendlyName); $($onOurDriver[$_.InstanceId])): back to Windows' driver" }
$plan += $ourPackages | ForEach-Object { "driver      $($_.Driver) = $($_.Original)" }
$plan += $certificates | ForEach-Object { "certificate $($_.Certificate.Subject) in LocalMachine\$($_.Store)" }
$plan += $installFolders | ForEach-Object { "folder      $_" }
$plan += $shortcuts | ForEach-Object { "shortcut    $_" }
$plan += $tempCopies | ForEach-Object { "temp copy   $_" }
$plan += $keys | ForEach-Object { "registry    $_" }
$plan += $shimFiles | ForEach-Object { "game shim   $_" }
$plan += $dataFolders | ForEach-Object { "data        $_" }
if ($ThirdParty) {
    $plan += $thirdDevices | ForEach-Object { "3rd party   device $($_.InstanceId) ($($_.FriendlyName))" }
    $plan += $thirdPackages | ForEach-Object { "3rd party   driver $($_.Driver) = $($_.Original)" }
}

Write-Host ''
if ($plan.Count -eq 0) {
    Write-Host 'Nothing of WheelBri found: this PC is already clean.' -ForegroundColor Green
} else {
    Write-Host "Found ($($plan.Count)):"
    $plan | ForEach-Object { Write-Host "  $_" }
}
if (-not $ThirdParty -and ($thirdDevices.Count + $thirdPackages.Count) -gt 0) {
    Write-Host ''
    Write-Host 'Also found, from tools used before (kept: -KeepThirdParty):' -ForegroundColor Yellow
    $thirdPackages | ForEach-Object { Write-Host "  driver $($_.Driver) = $($_.Original)" -ForegroundColor Yellow }
    $thirdDevices | ForEach-Object { Write-Host "  device $($_.InstanceId) ($($_.FriendlyName))" -ForegroundColor Yellow }
}
if ($KeepData) { Write-Host "Kept (-KeepData): $UserLocalAppData\wheelbri, $env:ProgramData\wheelbri (and the old-named ones)" }

$exitCode = 1
$go = $plan.Count -gt 0
if ($plan.Count -eq 0) { $exitCode = 0 }
if ($go -and -not $Yes) {
    Write-Host ''
    $answer = Read-Host 'Remove all of the above? [y/N]'
    if ("$answer".Trim().ToLower() -notin @('y', 'yes')) { Write-Host 'Nothing was removed.'; $go = $false }
}
try {
    if ($go) {
        Write-Host ''

        Step '[1/8] closing WheelBri'
        # The helper service first: stopping it removes the virtual G920 it made and shows the rig again.
        if ($helperService) {
            if (Remove-HelperService) { Write-Host '    removed the WheelBri Helper service' }
            else { Warn 'the WheelBri Helper service could not be removed: sc.exe delete WheelBriHelper' }
        }
        foreach ($process in $processes) { if ($process.MainWindowHandle -ne 0) { [void]$process.CloseMainWindow() } }
        $deadline = (Get-Date).AddSeconds(30)   # a closing session removes its devices and shows the rig again itself
        while ((Get-Date) -lt $deadline -and @($processes | Where-Object { -not $_.HasExited }).Count -gt 0) {
            Start-Sleep -Milliseconds 250
        }
        foreach ($process in @($processes | Where-Object { -not $_.HasExited })) {
            Write-Host "    $($process.ProcessName) did not close within 30 s: ended"
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
        # A driver installer the app started at Start keeps running when the app closes: finished first, never ended.
        if (-not (Wait-DriverInstallers)) { Warn 'a driver installer WheelBri started is still running after 2 minutes: going on' }
        foreach ($name in $names) {
            Remove-ItemProperty -Path "HKLM:\SOFTWARE\$name\hide" -Name 'AllowedPid', 'AllowedPidStart' -ErrorAction SilentlyContinue
        }
        Write-Host '    done'

        Step '[2/8] removing the virtual devices'
        $devices = $ourDevices
        if ($ThirdParty) { $devices += $thirdDevices }
        foreach ($device in $devices) {
            pnputil /remove-device "$($device.InstanceId)" | Out-Null
            if ($LASTEXITCODE -in @(0, 3010)) {
                Write-Host "    removed $($device.InstanceId)"
            } else {
                Warn "could not remove $($device.InstanceId) (pnputil $LASTEXITCODE): Device Manager > View > Show hidden devices > Uninstall"
            }
        }
        if ($devices.Count -eq 0) { Write-Host '    none' }

        Step '[3/8] removing the drivers'
        $restart = $false
        $drivers = $ourPackages
        if ($ThirdParty) { $drivers += $thirdPackages }
        foreach ($package in $drivers) {
            pnputil /delete-driver $package.Driver /uninstall /force | Out-Null
            $code = $LASTEXITCODE
            if ($code -in @(0, 259, 3010)) {
                Write-Host "    removed $($package.Driver) ($($package.Original))"
                if ($code -eq 3010) { $restart = $true }
            } else {
                Warn "could not remove $($package.Driver) ($($package.Original)): pnputil exited with $code"
            }
        }
        if ($drivers.Count -eq 0) { Write-Host '    none' }
        $left = @(Get-DriverPackages | Where-Object { $ourInfs -contains $_.Original })
        if ($left.Count -gt 0) { Warn "still installed: $(($left | ForEach-Object { $_.Original }) -join ', ')" }
        # The devices our drivers were on: removed and found again, so Windows sets them up from scratch with its own
        # driver (and no filter left in their settings), now rather than after a replug.
        foreach ($device in $filteredDevices) {
            pnputil /remove-device "$($device.InstanceId)" | Out-Null
            $code = $LASTEXITCODE
            if ($code -in @(0, 3010)) {
                Write-Host "    $($device.InstanceId): back to Windows' driver"
                if ($code -eq 3010) { $restart = $true }
            } else {
                Warn "could not reset $($device.InstanceId) (pnputil $code): unplug it and plug it in again"
            }
        }
        pnputil /scan-devices | Out-Null
        if ($restart) { Warn 'Windows wants a restart to finish removing a driver' }

        Step '[4/8] removing the drivers'' certificate'
        foreach ($entry in $certificates) {
            $path = "Cert:\LocalMachine\$($entry.Store)\$($entry.Certificate.Thumbprint)"
            Remove-Item $path -Force -ErrorAction SilentlyContinue
            if (Test-Path $path) { Warn "could not remove $($entry.Certificate.Subject) from LocalMachine\$($entry.Store)" }
            else { Write-Host "    removed $($entry.Certificate.Subject) from LocalMachine\$($entry.Store)" }
        }
        if ($certificates.Count -eq 0) { Write-Host '    none' }

        Step '[5/8] removing the program, its shortcuts and the installer''s copies in %TEMP%'
        foreach ($path in $shortcuts + $installFolders + $tempCopies) {
            # An install folder: the files in its installed-files.txt and WheelBri's subfolders (all of it without that
            # list); never following a link (common.ps1).
            if ($installFolders -contains $path) { [void](Remove-InstallFolder $path) } else { Remove-Tree $path }
            if (Test-Path $path) { Warn "could not remove $path completely (a file in use, or not WheelBri's?): delete it after a restart" }
            else { Write-Host "    removed $path" }
        }
        if (($shortcuts.Count + $installFolders.Count + $tempCopies.Count) -eq 0) { Write-Host '    none' }

        Step '[6/8] removing registry entries (hide setting, Settings > Apps entry, UMDF registrations, DirectInput)'
        foreach ($key in $keys) {
            Remove-Item $key -Recurse -Force -ErrorAction SilentlyContinue
            if (Test-Path $key) { Warn "could not remove $key" } else { Write-Host "    removed $key" }
        }
        if ($keys.Count -eq 0) { Write-Host '    none' }

        Step '[7/8] taking the shim out of game folders'
        foreach ($file in $shimFiles) {
            Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue
            if (Test-Path -LiteralPath $file) { Warn "could not remove $file (is the game running?)" } else { Write-Host "    removed $file" }
        }
        if ($GameFolder.Count -eq 0) { Write-Host '    no game folder given (-GameFolder)' }
        elseif ($shimFiles.Count -eq 0) { Write-Host '    none' }

        Step '[8/8] removing the app''s data (rigs, configs, logs, telemetry)'
        if ($KeepData) {
            Write-Host '    kept (-KeepData)'
        } else {
            foreach ($folder in $dataFolders) {
                # Remove-Tree (common.ps1), not Remove-Item -Recurse: as Administrator that follows a link (junction)
                # inside the folder and empties whatever it points to; Remove-Tree takes the link away only.
                Remove-Tree $folder
                if (Test-Path -LiteralPath $folder) { Warn "could not remove $folder completely (a file in use?)" }
                else { Write-Host "    removed $folder" }
            }
            if ($dataFolders.Count -eq 0) { Write-Host '    none' }
        }
        $exitCode = 0
    }
} catch {
    Write-Host "Cleaning failed: $($_.Exception.Message)" -ForegroundColor Red
} finally {
    Write-Host ''
    if ($exitCode -eq 0 -and $plan.Count -gt 0) {
        Write-Host 'Done: WheelBri is gone from this PC.' -ForegroundColor Green
        if ($warnings.Count -gt 0) {
            Write-Host 'Things to look at:' -ForegroundColor Yellow
            $warnings | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }
        }
    }
    Write-Host "log: $log"
    try { Stop-Transcript | Out-Null } catch { }
}
exit $exitCode
