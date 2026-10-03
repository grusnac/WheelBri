# Removes everything WheelBri (internal name wheelbri; wheelemu before the rename) has put on this PC, so the next
# install starts from a clean PC. It lists what it found, asks once, then removes it. It asks for Administrator itself.
# Every name below is looked for under both names: $names = wheelbri and the old wheelemu.
#
#   clean-pc.ps1                          list, ask, remove
#   clean-pc.ps1 -Yes                     no question (and no "press Enter" at the end)
#   clean-pc.ps1 -KeepData                keep %LOCALAPPDATA%\wheelbri (rigs, configs, app.ini, logs, telemetry) and
#                                         C:\ProgramData\wheelbri (driver and install logs), and the old-named ones
#   clean-pc.ps1 -GameFolder "D:\Games\Need for Speed"
#                                         also take the shim out of a game folder (dinput8.dll / dinput8.dll.off when
#                                         they are ours, wheelbri-shim.ini, wheelemu-shim.ini); several folders:
#                                         comma-separated
#   clean-pc.ps1 -KeepThirdParty          keep HidHide and HIDMaestro (drivers and devices), which the project used
#                                         before it had its own drivers; by default they are removed too, so the PC
#                                         has nothing left from any earlier version (-ThirdParty, the old switch, is
#                                         still accepted and changes nothing)
#
# What it removes: running WheelBri processes; the virtual devices (SWD\wheelbri\..., SWD\wheelemu\...) and the
# HID devices under them, including ones Windows keeps half-removed; the driver packages under both names (the hide
# filter first, then xusb and g920); every device our drivers are on (found by their driver description, so also when
# the package is already gone: the rig's devices with the hide filter, whatever they are) is reinstalled on Windows'
# own driver; the drivers' UMDF registrations (WUDF\Services); the drivers' test certificate (WDKTestCert) from the machine's Root
# and TrustedPublisher stores; the install folder (%ProgramFiles%\WheelBri, an older %ProgramFiles%\wheelemu, or where
# Settings > Apps says), its shortcuts and its Settings > Apps entries; HKLM\SOFTWARE\wheelbri and \wheelemu (the hide
# setting); the installer's and uninstaller's copies in %TEMP% (wheelbri-setup-*, wheelbri-uninstall-*);
# DirectInput's entries for VID_046D&PID_C262 (the G920's name and force feedback driver, which the service writes;
# a real G920 gets them again when plugged in); the app's data folders unless -KeepData.
# It never touches this repository (build\, dist\) or the WDK certificate in your own store that signs builds.
#
# Exit codes: 0 clean (yellow lines say what to look at, e.g. a restart), 1 failed or cancelled.

param(
    [switch]$Yes,
    [switch]$KeepData,
    [switch]$ThirdParty,       # old switch: removing them is now the default
    [switch]$KeepThirdParty,
    [string[]]$GameFolder = @(),
    [string]$UserLocalAppData = ''   # the user's %LOCALAPPDATA%, passed on when the script elevates itself
)

if (-not $UserLocalAppData) { $UserLocalAppData = $env:LOCALAPPDATA }
$ThirdParty = -not $KeepThirdParty

function Forwarded {
    $list = @('-UserLocalAppData', $UserLocalAppData)
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
    $arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" " +
                 ((Forwarded | ForEach-Object { if ($_ -like '-*') { $_ } else { "`"$_`"" } }) -join ' ')
    Write-Host 'Cleaning the PC needs Administrator: answer the Windows prompt; it goes on in a new window.'
    try {
        $p = Start-Process powershell.exe -Verb RunAs -ArgumentList $arguments -Wait -PassThru
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
$log = Join-Path $env:TEMP "wheelbri-clean-pc-$(Get-Date -Format yyyy-MM-dd-HHmmss).log"
Start-Transcript -Path $log | Out-Null
$warnings = New-Object System.Collections.Generic.List[string]
function Warn([string]$text) { Write-Host "    $text" -ForegroundColor Yellow; $warnings.Add($text) }
function Step([string]$text) { Write-Host $text -ForegroundColor Cyan }

# --- What is there ----------------------------------------------------------------------------------------------

$names = @('wheelbri', 'wheelemu')   # the name, and the project's old name (before the rename)
# Removal order: the R3's filter first.
$ourInfs = @(foreach ($part in 'hide', 'xusb', 'g920') { foreach ($name in $names) { "$name-$part.inf" } })
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

$diKeys = @('HKCU:\System\CurrentControlSet\Control\MediaProperties\PrivateProperties\Joystick\OEM\VID_046D&PID_C262',
            'HKLM:\SYSTEM\CurrentControlSet\Control\MediaProperties\PrivateProperties\Joystick\OEM\VID_046D&PID_C262',
            'HKCU:\System\CurrentControlSet\Control\MediaProperties\PrivateProperties\DirectInput\VID_046D&PID_C262')
$otherKeys = @(foreach ($name in $names) {
    "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\$name"; "HKLM:\SOFTWARE\$name"
    # The UMDF host's registration of each driver (UmdfService in the INFs), left behind by some removals.
    foreach ($part in 'hide', 'g920', 'xusb') { "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\WUDF\Services\${name}_$part" }
})

# The installer's unpacked package and the uninstaller's copy (Setup.cmd, uninstall.ps1), left in %TEMP%.
function Get-TempCopies {
    $roots = @($env:TEMP, (Join-Path $UserLocalAppData 'Temp')) | Where-Object { $_ -and (Test-Path $_) } | Sort-Object -Unique
    return @(foreach ($root in $roots) {
        foreach ($name in $names) {
            Get-ChildItem $root -Directory -Filter "$name-setup-*" -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName }
            Get-ChildItem $root -Directory -Filter "$name-uninstall-*" -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName }
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
        if (-not (Test-Path $folder)) { Warn "game folder not found: $folder"; continue }
        foreach ($name in 'dinput8.dll', 'dinput8.dll.off') {
            $path = Join-Path $folder $name
            if ((Test-Path $path) -and (Test-OurShim $path)) { $found += $path }
        }
        foreach ($name in $names) {
            $ini = Join-Path $folder "$name-shim.ini"
            if (Test-Path $ini) { $found += $ini }
        }
    }
    return $found
}

Write-Host 'WheelBri: clean this PC for a fresh install' -ForegroundColor Green
Write-Host "log: $log"
Write-Host ''
Write-Host 'Looking for what WheelBri left on this PC...'

$processes = @(Get-Process -Name (@(foreach ($name in $names) { $name; "$name-service"; "$name-bridge" }) +
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
$certificates = @(foreach ($store in 'Root', 'TrustedPublisher') {
    Get-ChildItem "Cert:\LocalMachine\$store" -ErrorAction SilentlyContinue | Where-Object { $_.Subject -like '*WDKTestCert*' } |
        ForEach-Object { [pscustomobject]@{ Store = $store; Certificate = $_ } }
})
$installFolders = @(Get-InstallFolders)
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
        foreach ($process in $processes) { if ($process.MainWindowHandle -ne 0) { [void]$process.CloseMainWindow() } }
        $deadline = (Get-Date).AddSeconds(30)   # a closing session removes its devices and shows the R3 again itself
        while ((Get-Date) -lt $deadline -and @($processes | Where-Object { -not $_.HasExited }).Count -gt 0) {
            Start-Sleep -Milliseconds 250
        }
        foreach ($process in @($processes | Where-Object { -not $_.HasExited })) {
            Write-Host "    $($process.ProcessName) did not close within 30 s: ended"
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
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
            Remove-Item $path -Recurse -Force -ErrorAction SilentlyContinue
            if (Test-Path $path) { Warn "could not remove $path completely (a file in use?): delete it after a restart" }
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
            Remove-Item $file -Force -ErrorAction SilentlyContinue
            if (Test-Path $file) { Warn "could not remove $file (is the game running?)" } else { Write-Host "    removed $file" }
        }
        if ($GameFolder.Count -eq 0) { Write-Host '    no game folder given (-GameFolder)' }
        elseif ($shimFiles.Count -eq 0) { Write-Host '    none' }

        Step '[8/8] removing the app''s data (rigs, configs, logs, telemetry)'
        if ($KeepData) {
            Write-Host '    kept (-KeepData)'
        } else {
            foreach ($folder in $dataFolders) {
                Remove-Item $folder -Recurse -Force -ErrorAction SilentlyContinue
                if (Test-Path $folder) { Warn "could not remove $folder completely (a file in use?)" }
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
    Stop-Transcript | Out-Null
    if (-not $Yes) { Read-Host 'Press Enter to close' | Out-Null }
}
exit $exitCode
