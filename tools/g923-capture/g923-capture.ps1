# Captures a real Logitech G923 (Xbox One and PC, 046D:C26E) with USBPcap, guided step by step, on the PC where
# WheelBri lives. Run it through scripts/make/g923-capture.sh (MSYS2), which passes -Tool and -Repo and handles the
# shim in the game folders. Plan: "Claude outputs/plans/g923-capture-automation-plan.md".
#
#   -Step check     S0: is the PC ready (WheelBri off, R3 out, the G923 in, tools), versions, registry export,
#                   where the wheel is (USB root hub and address). Changes nothing.
#   -Step check -WheelBri
#                   after restore: is WheelBri's side back (the R3 present, nothing of the capture left running)
#   -Step session -Session S1|S2|S3|S4|S5
#                   one guided capture: S1 plug-in + G HUB, S2 ACC with TrueForce, S3 ACC with G HUB closed,
#                   S4 DiRT Rally 2.0, S5 NFS Heat (DirectInput baseline)
#   -Step restore   registry compared with S0's export, G HUB's service back, USBPcap uninstall offered
#   -Step games     prints the game folders (exe folders) as name=path lines; no Administrator needed
#
# Capturing USB needs Administrator: the script starts itself again elevated, in its own window, and waits.
# Nothing here deletes a registry entry, a driver or a device. The only files it deletes are its own raw captures
# (other devices on the same USB hub, a keyboard among them, must not stay in a capture).

param(
    [ValidateSet('check', 'session', 'restore', 'games')]
    [string]$Step = 'check',
    [ValidateSet('', 'S1', 'S2', 'S3', 'S4', 'S5')]
    [string]$Session = '',
    [string]$Tool = '',              # wheelbri-g923cap.exe (Windows path), the decoder; the wrapper builds and passes it
    [string]$Repo = '',              # WheelBri's root
    [string]$Day = '',               # the captures folder for this day (default <Repo>\captures\g923\<date>)
    [switch]$WheelBri,
    [string]$Target = '',            # rehearsal with another wheel: VID[:PID] in hex, e.g. 346E (the MOZA R3)
    [string]$UserLocalAppData = '',
    [switch]$Elevated                # set when started again as Administrator
)

# Continue, not Stop: Windows PowerShell 5.1 turns a native program's stderr line into a terminating error under Stop.
$ErrorActionPreference = 'Continue'
if (-not $Repo) { $Repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
if (-not $Day) { $Day = Join-Path $Repo ("captures\g923\" + (Get-Date -Format 'yyyy-MM-dd')) }
if (-not $UserLocalAppData) { $UserLocalAppData = $env:LOCALAPPDATA }
$Kit = $PSScriptRoot
# Rehearsal: the decoder (wheelbri-g923cap) reads the same setting from the environment.
$TargetRegex = 'VID_046D&PID_C26E'
if ($Target) {
    $env:G923_TARGET = $Target
    $parts = $Target.Split(':')
    $TargetRegex = 'VID_' + $parts[0].ToUpper()
    if ($parts.Count -gt 1 -and $parts[1] -and $parts[1] -ne 'any') { $TargetRegex += '&PID_' + $parts[1].ToUpper() }
}

$AppIds = [ordered]@{ acc = 805550; dirt = 690790; nfsheat = 1222680 }
$GameExe = @{ acc = 'AC2-Win64-Shipping.exe'; dirt = 'dirtrally2.exe'; nfsheat = 'NeedForSpeedHeat.exe' }
$OemKey = 'System\CurrentControlSet\Control\MediaProperties\PrivateProperties\Joystick\OEM\VID_046D&PID_C26E'
$LogitechFfbClsid = '{62B43F0E-E7DB-4329-8C13-A966D84A289F}'

# --- Output --------------------------------------------------------------------------------------------------------

$script:Problems = New-Object System.Collections.Generic.List[string]
$script:Report = New-Object System.Collections.Generic.List[string]
function Say([string]$t) { Write-Host $t -ForegroundColor Cyan; $script:Report.Add($t) }
function Good([string]$t) { Write-Host "  ok   $t" -ForegroundColor Green; $script:Report.Add("- ok: $t") }
function Info([string]$t) { Write-Host "       $t"; $script:Report.Add("  - $t") }
function Warn([string]$t) { Write-Host "  warn $t" -ForegroundColor Yellow; $script:Report.Add("- warn: $t") }
function Bad([string]$t) { Write-Host "  FAIL $t" -ForegroundColor Red; $script:Report.Add("- **FAIL**: $t"); $script:Problems.Add($t) }

# --- Elevation -------------------------------------------------------------------------------------------------------

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    return ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
    & (Join-Path $env:SystemRoot 'Sysnative\WindowsPowerShell\v1.0\powershell.exe') -NoProfile -ExecutionPolicy Bypass `
        -File $PSCommandPath @PSBoundParameters
    exit $LASTEXITCODE
}

if ($Step -ne 'games' -and -not (Test-Admin)) {
    $quoted = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Step', $Step)
    if ($Session) { $quoted += @('-Session', $Session) }
    if ($Tool) { $quoted += @('-Tool', "`"$Tool`"") }
    $quoted += @('-Repo', "`"$($Repo.TrimEnd('\'))`"", '-Day', "`"$($Day.TrimEnd('\'))`"",
                 '-UserLocalAppData', "`"$($UserLocalAppData.TrimEnd('\'))`"", '-Elevated')
    if ($WheelBri) { $quoted += '-WheelBri' }
    if ($Target) { $quoted += @('-Target', $Target) }
    Write-Host 'USB capture needs Administrator: answer the Windows prompt. The steps go on in a new window.'
    try {
        $p = Start-Process powershell.exe -Verb RunAs -ArgumentList ($quoted -join ' ') -Wait -PassThru
        exit $p.ExitCode
    } catch {
        Write-Host 'Administrator was refused: nothing was done.' -ForegroundColor Red
        exit 1
    }
}

function Finish([int]$code) {
    if ($Elevated) {
        Write-Host ''
        Read-Host 'Press Enter to close this window' | Out-Null
    }
    exit $code
}

# --- Finding things ----------------------------------------------------------------------------------------------------

function Get-UsbPcapCmd {
    foreach ($p in @("$env:ProgramFiles\USBPcap\USBPcapCMD.exe", "$env:ProgramFiles\Wireshark\extcap\USBPcapCMD.exe",
                     "${env:ProgramFiles(x86)}\USBPcap\USBPcapCMD.exe")) {
        if ($p -and (Test-Path -LiteralPath $p)) { return $p }
    }
    return $null
}

function Get-UsbPcapHubs([string]$cmd) {
    # Piped through Out-String: USBPcapCMD is a GUI-subsystem program, and PowerShell only waits for one (and gets
    # its output) when its output goes into a pipeline.
    $text = & $cmd --extcap-interfaces 2>&1 | Out-String
    $hubs = @()
    foreach ($m in [regex]::Matches($text, 'interface \{value=([^}]+)\}')) { $hubs += $m.Groups[1].Value }
    return @($hubs)
}

function Get-SteamLibraries {
    $libs = @()
    $steam = $null
    try { $steam = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -ErrorAction Stop).SteamPath } catch { }
    if (-not $steam) {
        try { $steam = (Get-ItemProperty -Path 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam' -ErrorAction Stop).InstallPath } catch { }
    }
    if (-not $steam) { return @() }
    $steam = $steam -replace '/', '\'
    $libs += $steam
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path -LiteralPath $vdf) {
        foreach ($m in [regex]::Matches((Get-Content -LiteralPath $vdf -Raw), '"path"\s+"([^"]+)"')) {
            $libs += ($m.Groups[1].Value -replace '\\\\', '\')
        }
    }
    return @($libs | Select-Object -Unique)
}

# A game's install folder, the folder of its .exe and its Steam build id; $null when not installed.
function Find-Game([string]$key) {
    $id = $AppIds[$key]
    foreach ($lib in Get-SteamLibraries) {
        $manifest = Join-Path $lib "steamapps\appmanifest_$id.acf"
        if (-not (Test-Path -LiteralPath $manifest)) { continue }
        $text = Get-Content -LiteralPath $manifest -Raw
        if ($text -notmatch '"installdir"\s+"([^"]+)"') { continue }
        $install = Join-Path $lib ("steamapps\common\" + $Matches[1])
        $build = if ($text -match '"buildid"\s+"([^"]+)"') { $Matches[1] } else { '?' }
        $exe = Get-ChildItem -LiteralPath $install -Filter $GameExe[$key] -Recurse -File -ErrorAction SilentlyContinue |
            Select-Object -First 1
        return [pscustomobject]@{ Key = $key; Install = $install; ExeFolder = $(if ($exe) { $exe.DirectoryName } else { $null }); Build = $build }
    }
    return $null
}

function Get-FileVersionText([string]$path) {
    try {
        $item = Get-Item -LiteralPath $path -ErrorAction Stop
        $v = $item.VersionInfo
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.Substring(0, 16)
        return "file $($v.FileVersion), product $($v.ProductVersion), $($item.Length) bytes, sha256 $hash..."
    } catch { return 'unreadable' }
}

# G HUB itself (the app, its agent, its tray). Not lghub_updater: that is a service that runs all the time, also when
# G HUB is closed, so it must not count as "G HUB is running".
function Get-GHubProcesses {
    return @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -like 'lghub*' -and $_.ProcessName -ne 'lghub_updater' })
}
function Get-AllGHubProcesses { return @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -like 'lghub*' }) }

function Get-RealWheels {
    return @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object {
        $_.InstanceId -match 'VID_046D&PID_(C26E|C26D|C266|C267)' -and $_.InstanceId -notlike 'SWD\*'
    })
}

# The wheel being captured: the G923 (046D:C26E), or the rehearsal wheel (-Target).
function Get-TargetWheels {
    return @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object {
        $_.InstanceId -match $TargetRegex -and $_.InstanceId -notlike 'SWD\*'
    })
}

function Get-RigDevices { return @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match 'VID_346E' }) }

function Get-WheelSoftware {
    return @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
        $_.ProcessName -match 'pit.?house|moza' -or ($_.Path -and $_.Path -match '\\MOZA')
    })
}

# Starts a program as the signed-in user, not as Administrator (explorer.exe hands it over).
function Start-AsUser([string]$target) { Start-Process -FilePath explorer.exe -ArgumentList "`"$target`"" }

function Start-GHub {
    foreach ($svc in @(Get-Service -ErrorAction SilentlyContinue | Where-Object { $_.Name -like '*LGHUB*' })) {
        if ($svc.Status -ne 'Running') { try { Start-Service $svc.Name -ErrorAction Stop } catch { Warn "could not start $($svc.Name)" } }
    }
    foreach ($p in @("$env:ProgramFiles\LGHUB\system_tray\lghub_system_tray.exe", "$env:ProgramFiles\LGHUB\lghub.exe")) {
        if (Test-Path -LiteralPath $p) { Start-AsUser $p; return $true }
    }
    Warn 'G HUB not found under Program Files\LGHUB: start it yourself'
    return $false
}

function Stop-GHub {
    $stopped = @()
    foreach ($svc in @(Get-Service -ErrorAction SilentlyContinue | Where-Object { $_.Name -like '*LGHUB*' -and $_.Status -eq 'Running' })) {
        try { Stop-Service $svc.Name -Force -ErrorAction Stop; $stopped += $svc.Name } catch { Warn "could not stop $($svc.Name)" }
    }
    Get-AllGHubProcesses | ForEach-Object { try { Stop-Process -Id $_.Id -Force } catch { } }
    Start-Sleep -Seconds 2
    Get-AllGHubProcesses | ForEach-Object { try { Stop-Process -Id $_.Id -Force } catch { } }
    if ($stopped.Count -gt 0) { Set-State 'ghub_services_stopped' ($stopped -join ',') }
    return @(Get-AllGHubProcesses).Count -eq 0
}

# --- State (one file per day) ---------------------------------------------------------------------------------------

function Get-State([string]$key) {
    $f = Join-Path $Day 'state.txt'
    if (-not (Test-Path -LiteralPath $f)) { return $null }
    foreach ($line in Get-Content -LiteralPath $f) { if ($line -like "$key=*") { return $line.Substring($key.Length + 1) } }
    return $null
}

function Set-State([string]$key, [string]$value) {
    New-Item -ItemType Directory -Force -Path $Day | Out-Null
    $f = Join-Path $Day 'state.txt'
    $lines = @()
    if (Test-Path -LiteralPath $f) { $lines = @(Get-Content -LiteralPath $f | Where-Object { $_ -notlike "$key=*" }) }
    $lines += "$key=$value"
    Set-Content -LiteralPath $f -Value $lines -Encoding UTF8
}

# --- Capture -----------------------------------------------------------------------------------------------------------

function Start-Capture([string]$cmd, [string]$hub, [string]$file, [string[]]$filter) {
    $arguments = @('-d', $hub, '-o', "`"$file`"", '--inject-descriptors') + $filter
    $p = Start-Process -FilePath $cmd -ArgumentList ($arguments -join ' ') -WindowStyle Minimized -PassThru
    return $p
}

function Stop-Captures($processes) {
    foreach ($p in @($processes)) { if ($p -and -not $p.HasExited) { try { Stop-Process -Id $p.Id -Force } catch { } } }
    Start-Sleep -Milliseconds 800
}

function Invoke-Tool([string[]]$arguments) {
    if (-not $Tool -or -not (Test-Path -LiteralPath $Tool)) {
        Warn "no decoder given (-Tool): run the decode yourself: scripts/make/g923-capture.sh decode"
        return @()
    }
    $out = & $Tool @arguments 2>&1
    return @($out | ForEach-Object { "$_" })
}

# Where the wheel is: a 3 s capture of every root hub, read by the decoder. Hub and address back, or $null.
function Find-Wheel([string]$cmd) {
    $hubs = Get-UsbPcapHubs $cmd
    if ($hubs.Count -eq 0) { return $null }
    $tmp = Join-Path $env:TEMP ("g923-locate-" + (Get-Date -Format 'HHmmss'))
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $procs = @()
    $files = @{}
    foreach ($hub in $hubs) {
        $name = ($hub -replace '[\\\.]', '') + '.pcap'
        $file = Join-Path $tmp $name
        $files[$file] = $hub
        $procs += Start-Capture $cmd $hub $file @('-A')
    }
    Start-Sleep -Seconds 3
    Stop-Captures $procs
    # One line per wheel found: "found<TAB>file<TAB>bus<TAB>address<TAB>product".
    $lines = @(Invoke-Tool (@('locate') + @($files.Keys)))
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    $hit = $lines | Where-Object { $_ -like "found`t*" } | Select-Object -First 1
    if (-not $hit) { return $null }
    $fields = $hit -split "`t"
    if ($fields.Count -lt 5) { return $null }
    # The hub from the file's name (USBPcap1.pcap -> \\.\USBPcap1), whichever way the path's slashes go.
    $leaf = (($fields[1] -split '[\\/]')[-1]) -replace '\.pcap.*$', ''
    $hub = @($hubs | Where-Object { ($_ -replace '[\\\.]', '') -eq $leaf }) | Select-Object -First 1
    if (-not $hub) { return $null }
    return [pscustomobject]@{ Hub = $hub; Address = [int]$fields[3]; Bus = [int]$fields[2]; Product = $fields[4] }
}

# --- Session helpers --------------------------------------------------------------------------------------------------

$script:Notes = $null
function Note([string]$kind, [string]$text) {
    $ms = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    $line = "$ms`t$((Get-Date).ToString('yyyy-MM-ddTHH:mm:ss.fff'))`t$kind`t$text"
    Add-Content -LiteralPath $script:Notes -Value $line -Encoding UTF8
}

function Ask([string]$text) {
    Write-Host ''
    Write-Host ">> $text" -ForegroundColor White
    $answer = Read-Host '   Enter when done (s = skip)'
    $skipped = $answer -eq 's'
    Note $(if ($skipped) { 'SKIP' } else { 'STEP' }) $text
    return -not $skipped
}

function Countdown([int]$seconds, [string]$text) {
    for ($i = $seconds; $i -gt 0; $i--) {
        Write-Progress -Activity $text -Status "$i s" -PercentComplete (100 * ($seconds - $i) / $seconds)
        Start-Sleep -Seconds 1
    }
    Write-Progress -Activity $text -Completed
}

function Hold([string]$label, [int]$degrees, [string]$text) {
    Write-Host ''
    Write-Host ">> $text, and hold it still" -ForegroundColor White
    Read-Host '   Enter when the wheel is there (keep holding)' | Out-Null
    Note 'HOLD_START' "$label|$degrees"
    Countdown 5 "Hold: $text"
    Note 'HOLD_END' $label
    try { [console]::Beep(880, 200) } catch { }
    Write-Host '   done, let go' -ForegroundColor Green
}

# The level the shim starts with: the last [app] log_level in WheelBri's app.ini (warn when none).
function Get-ShimLevel {
    $ini = Join-Path $UserLocalAppData 'wheelbri\app.ini'
    if (-not (Test-Path -LiteralPath $ini)) { return 'warn' }
    $level = 'warn'
    foreach ($line in Get-Content -LiteralPath $ini) {
        if ($line -match '^\s*log_level\s*=\s*(\w+)') { $level = $Matches[1].ToLower() }
    }
    return $level
}

# One-run safety: stop and ask when something already looks wrong, rather than find out after the drive.
function Confirm-GoOn([string]$why) {
    Write-Host ''
    Write-Host "!! $why" -ForegroundColor Red
    $a = Read-Host '   Go on anyway? (y = go on, anything else = stop this session here)'
    Note 'ANSWER' "go on despite: $why -> $a"
    if ($a -ne 'y') { throw "stopped: $why" }
}

# Is the capture file growing? (USBPcap writes as it goes.)
function Test-CaptureGrowing([string]$folder) {
    $size = { (@(Get-ChildItem -LiteralPath $folder -Filter 'raw-*.pcap' -ErrorAction SilentlyContinue) | Measure-Object Length -Sum).Sum }
    # A wheel sends reports only when something changes (it moves, or the game sends force): have it move.
    Write-Host '   checking the capture: turn the wheel a little for 3 seconds now' -ForegroundColor White
    $a = & $size
    Start-Sleep -Seconds 3
    $b = & $size
    if (-not $b) { Confirm-GoOn 'no capture file is being written' }
    elseif ($b -le $a) { Confirm-GoOn "the capture did not grow while the wheel moved ($b bytes): is the wheel on that hub?" }
    else { Good ("capturing: {0:N1} MB so far" -f ($b / 1MB)) }
}

# Has the game loaded the shim with logi_log on? (A shim log newer than the session's start that says so.)
function Test-ShimLogging {
    $logs = Join-Path $UserLocalAppData 'wheelbri\logs'
    $new = @(Get-ChildItem -LiteralPath $logs -Filter 'wheelbri-shim-*.log' -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -ge $script:SessionStart })
    if ($new.Count -eq 0) { Confirm-GoOn 'the game wrote no shim log: the shim is not loaded, so its SDK calls are not logged'; return }
    $text = ($new | ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw -ErrorAction SilentlyContinue }) -join "`n"
    if ($text -match 'logi_log: on') { Good "shim loaded, logi_log on ($($new[0].Name))" }
    else { Confirm-GoOn "the shim log ($($new[0].Name)) does not say 'logi_log: on'" }
    if ($text -match 'logi: .*wrapped') { Good 'the game looked up Logitech''s SDK through the shim' }
    elseif (-not $Target) { Warn 'no Logitech SDK lookup logged yet (it may come when the session starts)' }
}

function New-SessionFolder([string]$name) {
    $folder = Join-Path $Day $name
    $n = 2
    while (Test-Path -LiteralPath $folder) { $folder = Join-Path $Day "$name-$n"; $n++ }
    New-Item -ItemType Directory -Force -Path $folder | Out-Null
    return $folder
}

function Copy-ShimLogs([string]$folder, [datetime]$since) {
    $logs = Join-Path $UserLocalAppData 'wheelbri\logs'
    $copied = 0
    foreach ($f in @(Get-ChildItem -LiteralPath $logs -Filter 'wheelbri-shim-*.log' -ErrorAction SilentlyContinue)) {
        if ($f.LastWriteTime -ge $since) { Copy-Item -LiteralPath $f.FullName -Destination $folder; $copied++ }
    }
    return $copied
}

function Test-Shim([string]$key) {
    $g = Find-Game $key
    if (-not $g -or -not $g.ExeFolder) { Warn "$key not found in Steam's libraries"; return $null }
    $ini = Join-Path $g.ExeFolder 'wheelbri-shim.ini'
    $dll = Join-Path $g.ExeFolder 'dinput8.dll'
    if (-not (Test-Path -LiteralPath $dll) -or -not (Test-Path -LiteralPath $ini)) {
        Warn "no WheelBri shim in $($g.ExeFolder): the SDK calls will not be logged (the wrapper puts it in)"
        return $g
    }
    $text = Get-Content -LiteralPath $ini -Raw
    $emulate = $text -match '(?m)^\s*emulate\s*=\s*false'
    $logi = $text -match '(?m)^\s*logi_log\s*=\s*true'
    if ($emulate -and $logi) { Good "shim in $key, logging only (emulate = false, logi_log = true)" }
    else { Bad "shim in $key but not set to emulate = false and logi_log = true" }
    $level = Get-ShimLevel
    if ($level -in @('debug', 'trace')) { Good "shim log level $level (app.ini)" }
    elseif ($level -eq 'info') { Warn 'shim log level info: the SDK calls are logged, debug lines are not' }
    else { Bad "shim log level '$level' (app.ini): the SDK calls would not be logged; run the session through g923-capture.sh" }
    return $g
}

# --- check (S0) --------------------------------------------------------------------------------------------------------

function Invoke-Check {
    $folder = Join-Path $Day 'S0-check'
    New-Item -ItemType Directory -Force -Path $folder | Out-Null
    Say "G923 capture: checking this PC ($(Get-Date -Format 'yyyy-MM-dd HH:mm'))"

    Say 'WheelBri out of the way'
    $wb = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -in @('wheelbri', 'wheelbri-service') })
    if ($wb.Count) { Bad "WheelBri is running ($(($wb | ForEach-Object ProcessName) -join ', ')): press Stop in the app, then quit it" }
    else { Good 'WheelBri app and service not running' }
    $virtual = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'SWD\WHEELBRI*' })
    if ($virtual.Count) { Bad "WheelBri's virtual devices are present: $(($virtual | ForEach-Object InstanceId) -join ', ')" }
    else { Good 'no virtual WheelBri device present' }

    Say 'The R3 and its own software'
    if ($Target) {
        Info "REHEARSAL with $Target`: the R3 and its own software may stay (not checked)"
        $rig = @()
        $pitSkip = $true
    } else {
        $rig = Get-RigDevices
        $pitSkip = $false
    }
    if ($pitSkip) { }
    elseif (@($rig).Count) { Bad "a MOZA device (VID 346E) is plugged in: unplug the R3 base (and its pedals if they are MOZA's USB)" }
    else { Good 'no MOZA device plugged in' }
    $pit = if ($pitSkip) { @() } else { Get-WheelSoftware }
    if ($pitSkip) { }
    elseif (@($pit).Count) { Bad "the R3's own software is running ($(($pit | ForEach-Object ProcessName) -join ', ')): quit it" }
    else { Good 'the R3''s own software not running' }

    Say $(if ($Target) { "The rehearsal wheel ($Target)" } else { 'The real G923' })
    $wheels = if ($Target) { Get-TargetWheels } else { Get-RealWheels }
    $c26e = @($wheels | Where-Object { $_.InstanceId -match $TargetRegex })
    if ($Target -and $c26e.Count -eq 0) {
        Bad "no device matching $TargetRegex is plugged in"
    } elseif ($Target) {
        Good "rehearsal wheel present ($($c26e.Count) device nodes)"
    } elseif ($c26e.Count -eq 0) {
        if ($wheels.Count) { Bad "a G923 is there but not as 046D:C26E (PC mode): $(($wheels | ForEach-Object InstanceId) -join ', ')" }
        else { Bad 'no G923 plugged in (046D:C26E)' }
    } else { Good "G923 present as 046D:C26E ($($c26e.Count) device nodes)" }
    foreach ($w in $wheels) {
        $compat = ''
        try { $compat = ((Get-PnpDeviceProperty -InstanceId $w.InstanceId -KeyName 'DEVPKEY_Device_CompatibleIds' -ErrorAction Stop).Data | Where-Object { $_ -like 'HID_DEVICE_UP*' }) -join ' ' } catch { }
        Info "$($w.InstanceId)  [$($w.Class)] $($w.FriendlyName) $compat"
    }
    if (-not $Target -and @($wheels | Where-Object { $_.InstanceId -match 'UP:FFFD|MI_02' }).Count -eq 0 -and $c26e.Count) {
        Info 'no separate TrueForce interface (MI_02) among the device nodes: it may be a collection of MI_00 or MI_01; S1 records the report descriptors and the probe finds it by usage page'
    }

    Say 'Registry (DirectInput entries for 046D:C26E), exported, not changed'
    foreach ($hive in @('HKCU', 'HKLM')) {
        $file = Join-Path $folder "registry-$hive-before.reg"
        $null = & reg.exe export "$hive\$OemKey" $file /y 2>&1
        $path = "Registry::$(if ($hive -eq 'HKCU') { 'HKEY_CURRENT_USER' } else { 'HKEY_LOCAL_MACHINE' })\$OemKey\OEMForceFeedback"
        if (Test-Path -LiteralPath $path) {
            $p = Get-ItemProperty -LiteralPath $path
            $owner = if ($null -ne $p.WheelBri) { "WheelBri's copy (WheelBri value)" } else { "Logitech's (no WheelBri value)" }
            $clsid = $p.CLSID
            $which = if ($clsid -eq $LogitechFfbClsid) { "Logitech's force feedback driver" } else { "CLSID $clsid" }
            Good "$hive OEMForceFeedback: $which, $owner"
        } else {
            Warn "$hive has no OEMForceFeedback for C26E: DirectInput games get Windows' PID driver, not Logitech's"
        }
    }
    Info "Do not switch WheelBri's trueforce off while the real wheel is plugged in: it removes that entry."

    Say 'Versions'
    $nt = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
    Info "Windows $($nt.DisplayVersion) build $($nt.CurrentBuild).$($nt.UBR)"
    $ghubExe = "$env:ProgramFiles\LGHUB\lghub.exe"
    if (Test-Path -LiteralPath $ghubExe) { Info "G HUB: $(Get-FileVersionText $ghubExe)" } else { Bad 'G HUB is not installed (Program Files\LGHUB)' }
    $ffbDll = $null
    try { $ffbDll = (Get-ItemProperty -LiteralPath "Registry::HKEY_CLASSES_ROOT\CLSID\$LogitechFfbClsid\InProcServer32").'(default)' } catch { }
    if ($ffbDll) { Info "Logitech force feedback driver: $ffbDll, $(Get-FileVersionText $ffbDll)" } else { Warn "Logitech's force feedback driver is not registered" }
    $sdkRoots = @("$env:ProgramFiles\LGHUB", "$env:ProgramData\LGHUB")
    foreach ($key in $AppIds.Keys) {
        $g = Find-Game $key
        if ($g) {
            Info "$key build $($g.Build): $($g.Install) (exe in $($g.ExeFolder))"
            $sdkRoots += $g.Install
        } else { Info "$key not installed" }
    }
    foreach ($root in $sdkRoots) {
        if (-not (Test-Path -LiteralPath $root)) { continue }
        foreach ($dll in @(Get-ChildItem -LiteralPath $root -Recurse -File -Filter 'trueforce_sdk*.dll' -ErrorAction SilentlyContinue)) {
            Info "$($dll.FullName): $(Get-FileVersionText $dll.FullName)"
        }
    }
    $dirt = Find-Game 'dirt'
    if ($dirt -and -not @(Get-ChildItem -LiteralPath $dirt.Install -Recurse -File -Filter 'trueforce_sdk*' -ErrorAction SilentlyContinue).Count) {
        Info 'DiRT Rally 2.0 has no trueforce_sdk* in its folder: S4 will skip itself'
    }

    Say 'Tools'
    if ($Tool -and (Test-Path -LiteralPath $Tool)) { Good "decoder: $Tool" } else { Bad 'no decoder (the wrapper builds wheelbri-g923cap and passes it)' }
    $tshark = "$env:ProgramFiles\Wireshark\tshark.exe"
    if (Test-Path -LiteralPath $tshark) { Good "Wireshark: $(Get-FileVersionText $tshark)" } else { Warn 'Wireshark not found (only needed to look at captures by hand)' }
    $cmd = Get-UsbPcapCmd
    if (-not $cmd) {
        Bad 'USBPcap is not installed: install Wireshark with USBPcap ticked, then restart Windows'
    } else {
        $hubs = Get-UsbPcapHubs $cmd
        if ($hubs.Count -eq 0) { Bad "USBPcap is installed ($cmd) but has no root hubs: restart Windows once after installing it" }
        else {
            Good "USBPcap: $cmd, root hubs $($hubs -join ', ')"
            if ($c26e.Count -and $Tool) {
                Info 'finding the wheel''s hub and address (3 s capture of each hub, deleted straight after)...'
                $w = Find-Wheel $cmd
                if ($w) {
                    Good "the wheel ($($w.Product)) is on $($w.Hub), address $($w.Address)"
                    Set-State 'hub' $w.Hub
                    Set-State 'address' "$($w.Address)"
                    Info 'In S1 plug it back into the same USB port.'
                } else { Bad 'the G923 was not found in a capture of any hub (is USBPcap working? try again after a restart)' }
            }
        }
    }
    $free = (Get-PSDrive -Name ($Day.Substring(0, 1))).Free / 1GB
    if ($free -lt 2) { Warn ("only {0:N1} GB free on the captures drive" -f $free) } else { Good ("{0:N0} GB free for captures" -f $free) }

    Say 'Shim logging'
    Info "WheelBri's log level now: $(Get-ShimLevel) (app.ini). Sessions S2-S4 set it to debug through g923-capture.sh; restore puts it back."
    $acc = Find-Game 'acc'
    if ($acc -and $acc.ExeFolder) {
        $dll = Join-Path $acc.ExeFolder 'dinput8.dll'
        if (Test-Path -LiteralPath $dll) { Info "ACC has a dinput8.dll now ($(Get-FileVersionText $dll)); S2 replaces it with this build's shim" }
        else { Info 'ACC has no shim now; S2 puts it in (logging only)' }
    }

    Say 'G HUB'
    if ((Get-GHubProcesses).Count) { Info 'G HUB is running (S1 closes it first and asks before doing so)' } else { Info 'G HUB is not running' }

    $out = Join-Path $folder 'check.md'
    Set-Content -LiteralPath $out -Value (@("# S0 check, $(Get-Date -Format 'yyyy-MM-dd HH:mm')", '') + $script:Report) -Encoding UTF8
    Write-Host ''
    if ($script:Problems.Count) {
        Write-Host "$($script:Problems.Count) thing(s) to fix before capturing (above, in red). Saved: $out" -ForegroundColor Red
        Finish 1
    }
    Write-Host "Ready to capture. Saved: $out" -ForegroundColor Green
    Finish 0
}

function Invoke-CheckWheelBri {
    Say 'WheelBri side after the capture'
    if ((Get-RigDevices).Count) { Good 'the R3 (MOZA) is plugged in' } else { Warn 'no MOZA device: plug the R3 in' }
    $cmd = Get-UsbPcapCmd
    if ($cmd) { Warn "USBPcap is still installed ($cmd): restore offers to remove it" } else { Good 'USBPcap removed' }
    $left = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -like 'USBPcapCMD*' })
    if ($left.Count) { Warn 'a USBPcapCMD is still running'; $left | ForEach-Object { Stop-Process -Id $_.Id -Force } }
    Info 'Start WheelBri and press Start: the R3 should drive the virtual wheel as before.'
    Finish 0
}

# --- Sessions ------------------------------------------------------------------------------------------------------------

function Invoke-Session {
    $names = @{ S1 = 'S1-plugin-ghub'; S2 = 'S2-acc-trueforce'; S3 = 'S3-acc-no-ghub'; S4 = 'S4-dirt-rally-2'; S5 = 'S5-nfs-heat' }
    $cmd = Get-UsbPcapCmd
    if (-not $cmd -or (Get-UsbPcapHubs $cmd).Count -eq 0) { Write-Host 'USBPcap is not ready: run check first.' -ForegroundColor Red; Finish 1 }
    if (@(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -in @('wheelbri', 'wheelbri-service') }).Count) {
        Write-Host 'WheelBri is running: Stop and quit it first (run check).' -ForegroundColor Red; Finish 1
    }
    if ($Session -eq 'S4') {
        $dirt = Find-Game 'dirt'
        if (-not $dirt -or -not @(Get-ChildItem -LiteralPath $dirt.Install -Recurse -File -Filter 'trueforce_sdk*' -ErrorAction SilentlyContinue).Count) {
            $folder = New-SessionFolder $names.S4
            Set-Content -LiteralPath (Join-Path $folder 'summary.md') -Value '# S4: skipped (DiRT Rally 2.0 has no trueforce_sdk* in its folder: it does not use the TrueForce SDK)' -Encoding UTF8
            [IO.File]::WriteAllText((Join-Path $Day 'last-session.txt'), $folder)   # no BOM: the bash wrapper reads it
            Write-Host 'S4 skipped: DiRT Rally 2.0 does not ship the TrueForce SDK (written to its summary.md).' -ForegroundColor Yellow
            Finish 0
        }
    }
    $folder = New-SessionFolder $names[$Session]
    $script:Notes = Join-Path $folder 'notes.txt'
    [IO.File]::WriteAllText((Join-Path $Day 'last-session.txt'), $folder)   # no BOM: the bash wrapper reads it
    try { Start-Transcript -Path (Join-Path $folder 'transcript.log') | Out-Null } catch { }
    $start = Get-Date
    $script:SessionStart = $start
    $script:Captures = @()
    Say "Session $Session -> $folder"
    Note 'NOTE' "session $Session started"
    $procs = @()
    try {
        switch ($Session) {
            'S1' { $procs = Invoke-S1 $cmd $folder }
            'S2' { $procs = Invoke-Game $cmd $folder 'acc' $true }
            'S3' { $procs = Invoke-S3 $cmd $folder }
            'S4' { $procs = Invoke-Game $cmd $folder 'dirt' $false }
            'S5' { $procs = Invoke-S5 $cmd $folder }
        }
    } catch {
        Write-Host "Session stopped: $($_.Exception.Message)" -ForegroundColor Red
        Note 'NOTE' "session stopped: $($_.Exception.Message)"
    } finally {
        # Every capture this session started, also when a step above stopped it early.
        Stop-Captures (@($procs) + @($script:Captures))
        Note 'NOTE' 'capture stopped'
    }
    $n = Copy-ShimLogs $folder $start
    if ($n) { Info "copied $n shim log(s)" }
    Say 'Keeping only the wheel''s frames (other devices on the hub are dropped) and decoding...'
    foreach ($line in Invoke-Tool @('keep', $folder)) { Info $line }
    $decode = Invoke-Tool @('decode', $folder)
    foreach ($line in $decode) {
        if ($line -match '^\s+FAIL') { Write-Host $line -ForegroundColor Red }
        elseif ($line -match '^\s+PASS') { Write-Host $line -ForegroundColor Green }
        else { Write-Host $line }
    }
    try { Stop-Transcript | Out-Null } catch { }
    Write-Host ''
    Write-Host "Summary: $(Join-Path $folder 'summary.md')"
    Finish 0
}

# Captures on the wheel's hub (only its address when known); every hub when it cannot be found.
function Start-WheelCapture([string]$cmd, [string]$folder, [switch]$AllDevices) {
    $procs = @()
    if (-not $AllDevices) {
        Info 'finding the wheel (3 s)...'
        $w = Find-Wheel $cmd
        if ($w) {
            Good "the wheel ($($w.Product)) is on $($w.Hub), address $($w.Address)"
            Set-State 'hub' $w.Hub
            $file = Join-Path $folder ('raw-' + ($w.Hub -replace '[\\\.]', '') + '.pcap')
            # Plus new devices: if the wheel enumerates again (a reset, G HUB starting) it gets a new address; keep drops
            # every other device afterwards.
            $p = Start-Capture $cmd $w.Hub $file @('--devices', "$($w.Address)", '--capture-from-new-devices')
            Start-Sleep -Seconds 2
            $script:Captures += $p
            if (-not $p.HasExited) {
                Note 'NOTE' "capture on $($w.Hub) address $($w.Address)"
                return @($p)
            }
            Warn "the capture on $($w.Hub) stopped at once (exit code $($p.ExitCode)): capturing every hub instead"
            # USBPcapCMD makes its file before it opens the hub, and never writes over one: clear the way.
            Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue
        } else {
            Warn 'the wheel was not found: capturing every hub (the other devices are dropped afterwards)'
        }
    }
    foreach ($hub in Get-UsbPcapHubs $cmd) {
        $file = Join-Path $folder ('raw-all-' + ($hub -replace '[\\\.]', '') + '.pcap')
        $procs += Start-Capture $cmd $hub $file @('-A')
    }
    $script:Captures += $procs
    Start-Sleep -Seconds 2
    $dead = @($procs | Where-Object { $_.HasExited })
    if ($dead.Count) { Warn "$($dead.Count) capture(s) stopped at once: USBPcap may need a restart of Windows" }
    Note 'NOTE' "capture on every hub ($(@($procs).Count))"
    return $procs
}

function Invoke-S1([string]$cmd, [string]$folder) {
    while ((Get-TargetWheels).Count) {
        Read-Host '>> Unplug the wheel''s USB cable now, then press Enter' | Out-Null
        Start-Sleep -Seconds 1
    }
    if ((Get-GHubProcesses).Count) {
        $a = Read-Host '>> G HUB is running and must start inside the capture. Close it now? (Y/n)'
        if ($a -ne 'n') { if (Stop-GHub) { Good 'G HUB closed' } else { Warn 'G HUB is still running: quit it from the tray' } }
    }
    $procs = Start-WheelCapture $cmd $folder -AllDevices
    Good 'capturing'
    Ask 'Plug the wheel into the same USB port as before, and wait for Windows'' sound' | Out-Null
    Countdown 10 'Letting Windows set the wheel up'
    if ($Target) {
        if (-not (Get-TargetWheels).Count) { Confirm-GoOn 'Windows does not show the wheel yet (plugged into the same port?)' }
    } elseif (-not (Get-TargetWheels).Count) {
        # Expected: without G HUB a G923 for Xbox starts in Xbox mode (another product ID); G HUB switches it to PC
        # mode (C26E) when it starts, below. The capture records both.
        $other = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match 'VID_046D&PID_C2' -and $_.InstanceId -notmatch 'PID_C26E' -and $_.InstanceId -notlike 'SWD\*' })
        Info "the wheel is not in PC mode yet (expected without G HUB; G HUB switches it next). Logitech devices now: $(($other | ForEach-Object { $_.InstanceId -replace '\\[^\\]*$', '' } | Select-Object -Unique) -join ', ')"
        Note 'NOTE' "after plug-in, before G HUB: $(($other | ForEach-Object InstanceId) -join ', ')"
    }
    Test-CaptureGrowing $folder
    if ($Target) {
        Ask 'REHEARSAL: turn the wheel and press each pedal once' | Out-Null
        Countdown 5 'A last few seconds'
        return $procs
    }
    Note 'NOTE' 'starting G HUB'
    Start-GHub | Out-Null
    Ask 'Wait until G HUB shows the G923 (its page open)' | Out-Null
    if (-not $Target -and -not (Get-TargetWheels).Count) { Confirm-GoOn 'the wheel is still not in PC mode (046D:C26E) after G HUB started' }
    elseif (-not $Target) { Good 'the wheel is in PC mode (046D:C26E)'; Note 'NOTE' 'PC mode (C26E) present' }
    Countdown 30 'G HUB settling'
    Ask 'In G HUB, set the Operating Range (rotation) to 540' | Out-Null
    Ask 'Set the Operating Range back to 900' | Out-Null
    Ask 'Set Sensitivity / force (Strength) to 50' | Out-Null
    Ask 'Set it back to 100' | Out-Null
    Ask 'If G HUB has a TRUEFORCE switch: turn it off' | Out-Null
    Ask 'Turn TRUEFORCE back on' | Out-Null
    Ask 'If G HUB has a TrueForce level / audio slider: move it to 50, then back where it was' | Out-Null
    Ask 'Centering Spring: on, then back off' | Out-Null
    Countdown 5 'A last few seconds'
    return $procs
}

function Invoke-Game([string]$cmd, [string]$folder, [string]$key, [bool]$holds) {
    if (-not $Target -and -not (Get-GHubProcesses).Count) {
        Info 'G HUB is not running: starting it'
        Start-GHub | Out-Null
        Countdown 20 'G HUB starting'
    }
    $before = $script:Problems.Count
    $g = Test-Shim $key
    if (-not $g) { Warn "$key is not installed"; return @() }
    if ($script:Problems.Count -gt $before) { Confirm-GoOn 'the shim is not set up for logging (above)' }
    $procs = Start-WheelCapture $cmd $folder
    Note 'NOTE' "starting $key (steam://rungameid/$($AppIds[$key]))"
    Start-AsUser "steam://rungameid/$($AppIds[$key])"
    if ($key -eq 'acc') {
        if ($Target) { Ask 'REHEARSAL: wait for ACC''s main menu (your usual wheel set-up)' | Out-Null }
        else { Ask 'Wait for ACC''s main menu. In Options > Controls make sure the G923 is chosen and TrueForce is on' | Out-Null }
        Test-CaptureGrowing $folder
        Test-ShimLogging
        Ask 'Start a Hot Lap (any car and track) and wait in the pits, hands off the wheel' | Out-Null
    } else {
        Ask 'Wait for the menu. Turn TrueForce on in the game''s wheel settings if it has such a setting' | Out-Null
        Test-CaptureGrowing $folder
        Test-ShimLogging
        Ask 'Start a short stage and wait at the start line, hands off the wheel' | Out-Null
    }
    if ($holds) {
        Hold 'centre' 0 'Wheel straight (centre)'
        Hold 'left90' -90 'Turn the wheel 90 degrees LEFT (a quarter turn)'
        Hold 'right90' 90 'Turn the wheel 90 degrees RIGHT'
        Hold 'left180' -180 'Turn the wheel 180 degrees LEFT (half a turn)'
        Write-Host '>> Press Enter, then sweep the wheel left and right quickly for 5 seconds' -ForegroundColor White
        Read-Host | Out-Null
        Note 'STEP' 'sweep start'
        Countdown 5 'Sweep left and right'
        Note 'STEP' 'sweep end'
    }
    Note 'STEP' 'driving start'
    Ask 'Drive for 1-2 minutes: kerbs, grass or gravel, one long fast corner. Then stop' | Out-Null
    Ask 'Quit to the main menu' | Out-Null
    Ask 'Exit the game' | Out-Null
    Countdown 3 'Last packets'
    return $procs
}

function Invoke-S3([string]$cmd, [string]$folder) {
    $before = $script:Problems.Count
    $g = Test-Shim 'acc'
    if (-not $g) { return @() }
    if ($script:Problems.Count -gt $before) { Confirm-GoOn 'the shim is not set up for logging (above)' }
    if ((Get-AllGHubProcesses).Count) {
        Info 'closing G HUB fully (its processes and its service)...'
        if (-not (Stop-GHub)) { Warn 'some G HUB process is still there: quit it from the tray, then go on' }
    }
    $left = @(Get-AllGHubProcesses)
    if ($left.Count) { Warn "still running: $(($left | ForEach-Object ProcessName) -join ', ')" } else { Good 'no lghub* process running (updater service included)' }
    Note 'NOTE' "G HUB processes running: $($left.Count)"
    $procs = Start-WheelCapture $cmd $folder
    Note 'NOTE' 'starting acc without G HUB'
    Start-AsUser "steam://rungameid/$($AppIds['acc'])"
    Ask 'Wait for ACC''s main menu (G HUB closed)' | Out-Null
    Test-CaptureGrowing $folder
    Test-ShimLogging
    Ask 'Start a Hot Lap, wait 30 s in the pits, then turn the wheel left and right a little' | Out-Null
    Ask 'Does the wheel have force (try to turn it)? Answer in the next line' | Out-Null
    $f = Read-Host '   force felt without G HUB? (y/n)'
    Note 'ANSWER' "force without G HUB: $f"
    Note 'NOTE' 'starting G HUB with ACC running'
    Start-GHub | Out-Null
    Countdown 30 'G HUB starting while ACC runs'
    Ask 'Turn the wheel left and right a little' | Out-Null
    $f = Read-Host '   force felt now? (y/n)'
    Note 'ANSWER' "force after G HUB started: $f"
    Ask 'Exit ACC' | Out-Null
    Countdown 3 'Last packets'
    return $procs
}

function Invoke-S5([string]$cmd, [string]$folder) {
    if (-not (Get-GHubProcesses).Count) { Start-GHub | Out-Null; Countdown 20 'G HUB starting' }
    $g = Find-Game 'nfsheat'
    if (-not $g) { Warn 'NFS Heat is not installed'; return @() }
    $procs = Start-WheelCapture $cmd $folder
    Start-AsUser "steam://rungameid/$($AppIds['nfsheat'])"
    Ask 'Wait for NFS Heat to load, and get into a car (free roam)' | Out-Null
    Test-CaptureGrowing $folder
    Note 'STEP' 'driving start'
    Ask 'Drive for 2 minutes: a few hard turns, a crash into a wall' | Out-Null
    Ask 'Exit the game' | Out-Null
    Countdown 3 'Last packets'
    return $procs
}

# --- restore -------------------------------------------------------------------------------------------------------

function Invoke-Restore {
    Say 'Restore after the capture'
    $before = Join-Path $Day 'S0-check'
    foreach ($hive in @('HKCU', 'HKLM')) {
        $old = Join-Path $before "registry-$hive-before.reg"
        $new = Join-Path $before "registry-$hive-after.reg"
        $null = & reg.exe export "$hive\$OemKey" $new /y 2>&1
        if (-not (Test-Path -LiteralPath $old)) { Warn "no $hive export from check"; continue }
        if (-not (Test-Path -LiteralPath $new)) { Warn "$hive key gone now (it was there before)"; continue }
        $a = Get-Content -LiteralPath $old
        $b = Get-Content -LiteralPath $new
        $diff = @(Compare-Object $a $b)
        if ($diff.Count -eq 0) { Good "$hive C26E entries unchanged since check" }
        else {
            Warn "$hive C26E entries changed since check ($($diff.Count) lines): compare $old and $new"
            $diff | Select-Object -First 10 | ForEach-Object { Info "$($_.SideIndicator) $($_.InputObject)" }
        }
    }
    $services = Get-State 'ghub_services_stopped'
    if ($services) {
        foreach ($s in $services -split ',') { if ($s) { try { Start-Service $s -ErrorAction Stop; Good "started $s again" } catch { Warn "could not start $s" } } }
        Set-State 'ghub_services_stopped' ''
    }
    Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -like 'USBPcapCMD*' } | ForEach-Object { Stop-Process -Id $_.Id -Force }
    $uninstall = "$env:ProgramFiles\USBPcap\Uninstall.exe"
    if ($Target) {
        Info 'REHEARSAL: USBPcap stays installed (you need it for the G923)'
    } elseif (Test-Path -LiteralPath $uninstall) {
        $a = Read-Host '>> Remove USBPcap now? It sits on every USB root hub, next to WheelBri''s hide filter (y/N)'
        if ($a -eq 'y') { Start-Process -FilePath $uninstall -Wait; Info 'restart Windows when it asks' }
        else { Info "keep it for now; remove later from Settings > Apps (USBPcap) or $uninstall" }
    }
    Info 'Plug the R3 back in, start WheelBri, press Start. Then: scripts/make/g923-capture.sh check --wheelbri'
    Finish 0
}

# --- games (for the wrapper) ----------------------------------------------------------------------------------------

function Invoke-Games {
    foreach ($key in $AppIds.Keys) {
        $g = Find-Game $key
        if ($g -and $g.ExeFolder) { Write-Output "$key=$($g.ExeFolder)" }
    }
    exit 0
}

switch ($Step) {
    'check' { if ($WheelBri) { Invoke-CheckWheelBri } else { Invoke-Check } }
    'session' { if (-not $Session) { Write-Host 'which session? -Session S1..S5' -ForegroundColor Red; Finish 1 }; Invoke-Session }
    'restore' { Invoke-Restore }
    'games' { Invoke-Games }
}
