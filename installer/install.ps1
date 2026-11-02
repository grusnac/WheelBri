# Installs WheelBri on a PC that has nothing of it yet: the app, its two drivers, shortcuts and an entry in
# Settings > Apps (to uninstall). Run it from the unpacked package (Setup.exe does that), as Administrator; it asks
# for Administrator itself when needed. Running it again upgrades in place and keeps an edited config.
#
# Windows 11 (build 22000+) gets everything below. Windows 7 SP1 to Windows 10 get the in-game shim way only: the
# virtual G920's driver needs Windows 11, so there are no drivers, no WheelBri Helper and no restart; the app and its
# shim need the Universal C Runtime there (Windows 10 has it; Windows 7 with update KB2999226), and this script
# PowerShell 5.1 (Windows 7: Windows Management Framework 5.1; Setup.cmd checks).
#
#   install.ps1                         install to C:\Program Files\WheelBri
#   install.ps1 -InstallDir D:\wheelbri
#   install.ps1 -NoDesktopShortcut
#   install.ps1 -StopBrowserGamepads    turn off Firefox's gamepad support for the signed-in user without asking (6b)
#   install.ps1 -Quiet                  no questions and no pause, even when it fails (for scripted installs); no
#                                       restart either (restart Windows yourself afterwards)
#
# Three questions at most: Windows' Administrator prompt; only when Firefox is installed, whether to turn off its
# gamepad support (6b, default no); and at the end of an install that worked, whether to restart Windows now (default
# yes). The restart makes sure every driver is in use: the hiding filter in particular often
# starts on a device only after one (when it was on another driver, e.g. the wheel maker's, Windows finishes the switch at
# the restart). When the install failed the window waits for Enter (so the reason can be read).
#
# What it changes on this PC (all undone by the uninstaller):
#   - files in the install folder (listed in its installed-files.txt: the uninstaller removes those, not the whole
#     folder; an -InstallDir that is not empty and holds no WheelBri install is refused); Start menu (and desktop)
#     shortcut "WheelBri"; an uninstall entry
#   - two driver packages (pnputil): the virtual wheel (wheelbri-wheel: G25, G27, G920, G923) and the
#     filter that hides the rig from games (wheelbri-hide). When the user's saved settings name a rig (a reinstall
#     that kept them), wheelbri-service --prepare-hide puts it on that rig's plugged-in devices here, before the
#     restart, so the first Start is instant; otherwise the app puts it on them at the first Start
#   - the WheelBri Helper, a Windows service (WheelBriHelper, LocalSystem, wheelbri-helper.exe in the install folder,
#     starts with Windows): it creates the virtual G920 and hides the rig for the app, which runs as a standard user
#   - the certificate the drivers are signed with, added to this PC's trusted Root and TrustedPublisher stores (the
#     drivers are signed by WheelBri's own test certificate, not by Microsoft); its thumbprint is recorded in
#     HKLM\SOFTWARE\wheelbri\SigningCertificates, so the uninstaller removes exactly that certificate. Only the
#     certificate the package names (driver\signing-certificate.txt), and only a code-signing one that is no
#     certificate authority (driver\common.ps1 Add-PackageCertificate). Each release has a certificate of its own, made
#     for that build, its private key deleted once the drivers are signed: nobody can sign anything else with it. The
#     earlier releases' certificates go once no installed driver uses them
#   - only when asked (6b): Firefox's dom.gamepad.enabled set to false (locked) by policy for the signed-in user, and
#     that user recorded in HKLM\SOFTWARE\wheelbri BrowserGamepadPolicy; the uninstaller takes out only that preference
#
# Exit codes: 0 installed (possibly with warnings, listed at the end), 1 failed (the red line says why).
# Everything is also written to %LOCALAPPDATA%\wheelbri\logs\wheelbri-install-<date>.log (with every other log).

param(
    # Default: the 64-bit Program Files. Not $env:ProgramFiles: in a 32-bit PowerShell (MSYS2 can start one) that is
    # "Program Files (x86)", and it was passed on to the 64-bit run below.
    [string]$InstallDir = '',
    [switch]$NoDesktopShortcut,
    [switch]$StopBrowserGamepads,
    [switch]$Quiet
)

if (-not $InstallDir) {
    $InstallDir = Join-Path $(if ($env:ProgramW6432) { $env:ProgramW6432 } else { $env:ProgramFiles }) 'WheelBri'
}
# A full path without a trailing backslash, before it is passed on or recorded: a relative path would be resolved
# against System32 once elevated, and "D:\WheelBri\" quoted on a command line escapes its own closing quote.
$InstallDir = [IO.Path]::GetFullPath($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($InstallDir))
if ($InstallDir.Length -gt 3) { $InstallDir = $InstallDir.TrimEnd('\') }

# A 32-bit PowerShell sees SysWOW64, where pnputil and the 64-bit driver tools are missing: run again in 64-bit.
if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath, '-InstallDir', $InstallDir)
    if ($NoDesktopShortcut) { $arguments += '-NoDesktopShortcut' }
    if ($StopBrowserGamepads) { $arguments += '-StopBrowserGamepads' }
    if ($Quiet) { $arguments += '-Quiet' }
    & (Join-Path $env:SystemRoot 'Sysnative\WindowsPowerShell\v1.0\powershell.exe') @arguments
    exit $LASTEXITCODE
}

# Not Administrator: ask for it (one UAC prompt) and wait for the elevated run.
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not ([Security.Principal.WindowsPrincipal]$identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    # A drive's root keeps its backslash: doubled, so it does not escape the closing quote.
    $arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -InstallDir `"$($InstallDir -replace '\\$', '\\')`""
    if ($NoDesktopShortcut) { $arguments += ' -NoDesktopShortcut' }
    if ($StopBrowserGamepads) { $arguments += ' -StopBrowserGamepads' }
    if ($Quiet) { $arguments += ' -Quiet' }
    try {
        $p = Start-Process powershell.exe -Verb RunAs -ArgumentList $arguments -Wait -PassThru
        exit $p.ExitCode
    } catch {
        Write-Host 'WheelBri needs Administrator to install its drivers, and that was refused.' -ForegroundColor Red
        exit 1
    }
}

$ErrorActionPreference = 'Stop'
$package = $PSScriptRoot
# The drivers (the virtual G920, the hiding filter) and the WheelBri Helper need Windows 11; before it, the shim only.
$withDrivers = [Environment]::OSVersion.Version.Build -ge 22000
$version = if (Test-Path -LiteralPath (Join-Path $package 'VERSION.txt')) { (Get-Content -LiteralPath (Join-Path $package 'VERSION.txt') -Raw).Trim() } else { '0.0.0' }
# Every log in one folder, the user's %LOCALAPPDATA%\wheelbri\logs (the app and both drivers write there too): the
# drivers run as LocalService, so it may write there, and HKLM\SOFTWARE\wheelbri LogFolder tells them where (the
# app does the same at every start, see ServiceLogging.hpp shareLogFolder).
# (Set up as the first thing in the try below, so a failure there is reported and the window waits.)
$logFolder = Join-Path $env:LOCALAPPDATA 'wheelbri\logs'
$transcript = $false

$warnings = New-Object System.Collections.Generic.List[string]
$step = 0
$stepCount = 7   # the numbered steps below, 1 to 7 (2b and 6b, which not every install has, are part of 2 and 6)
function Step([string]$text) {
    $script:step++
    Write-Host ''
    Write-Host "[$script:step/$script:stepCount] $text" -ForegroundColor Cyan
}
function Fail([string]$text) {
    Write-Host ''
    Write-Host "Installation failed: $text" -ForegroundColor Red
    Write-Host "Details: $logFolder" -ForegroundColor Red
    throw 'stop'
}
function Warn([string]$text) {
    Write-Host $text -ForegroundColor Yellow
    $warnings.Add($text)
}
# The SID of the user signed in to this Windows session (the owner of its Explorer), not the Administrator account
# this runs as when that is another one; this process's own user when there is no Explorer.
function Get-DesktopUserSid {
    $session = (Get-Process -Id $PID).SessionId
    $explorer = @(Get-CimInstance Win32_Process -Filter "Name='explorer.exe'" -ErrorAction SilentlyContinue |
                  Where-Object { $_.SessionId -eq $session }) | Select-Object -First 1
    if ($explorer) {
        $owner = Invoke-CimMethod -InputObject $explorer -MethodName GetOwnerSid -ErrorAction SilentlyContinue
        if ($owner -and $owner.Sid) { return $owner.Sid }
    }
    return [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
}
# Firefox's Preferences policy in $key (a policy key under HKEY_USERS) as an object, empty when not set; $null when
# it is set in a form this does not change (a Preferences subkey, Firefox's older form, or text that is not JSON).
function Get-FirefoxPreferences([string]$key) {
    if (Test-Path -LiteralPath "$key\Preferences") { return $null }
    $value = (Get-ItemProperty -LiteralPath $key -Name Preferences -ErrorAction SilentlyContinue).Preferences
    if ($null -eq $value) { return New-Object PSObject }
    try { $parsed = (@($value) -join "`n") | ConvertFrom-Json } catch { return $null }
    if ($parsed -isnot [System.Management.Automation.PSCustomObject]) { return $null }
    return $parsed
}
# Runs one of the package's PowerShell scripts in its own process (they end with `exit`); returns its exit code.
function Run-Script([string]$path, [string[]]$arguments = @()) {
    $powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    & $powershell -NoProfile -ExecutionPolicy Bypass -File $path @arguments | ForEach-Object { Write-Host "    $_" }
    return $LASTEXITCODE
}

$exitCode = 1
$helperWasThere = $false   # an earlier install's WheelBri Helper was stopped (started again on failure)
try {
    # The user's WheelBri folders must not be links (junctions): as Administrator, the permission grant below would
    # follow one and change the permissions of whatever it points to. The app refuses them too (userFoldersProblem).
    $logsLinked = @((Split-Path $logFolder), $logFolder | Where-Object {
        $item = Get-Item -LiteralPath $_ -Force -ErrorAction SilentlyContinue
        $item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)
    }).Count -gt 0
    if ($logsLinked) {
        $transcriptFolder = Join-Path $env:SystemRoot 'Temp'
    } else {
        New-Item -ItemType Directory -Force -Path $logFolder | Out-Null
        $transcriptFolder = $logFolder
    }
    Start-Transcript -Append -Path (Join-Path $transcriptFolder "wheelbri-install-$(Get-Date -Format yyyy-MM-dd).log") | Out-Null
    $transcript = $true
    if ($logsLinked) {
        Warn ("    $logFolder (or the folder above it) is a link to another folder: the drivers keep their logs in " +
              'C:\ProgramData\wheelbri\logs. Replace the link with a real folder to have every log in one place.')
    } else {
        & icacls $logFolder /grant '*S-1-5-19:(OI)(CI)M' | Out-Null
        if (-not (Test-Path 'HKLM:\SOFTWARE\wheelbri')) { New-Item -Path 'HKLM:\SOFTWARE\wheelbri' | Out-Null }
        Set-ItemProperty -Path 'HKLM:\SOFTWARE\wheelbri' -Name 'LogFolder' -Value $logFolder
    }

    Write-Host "WheelBri $version setup" -ForegroundColor Green
    Write-Host "package:      $package"
    Write-Host "install into: $InstallDir"

    # --- 1. this PC ---------------------------------------------------------------------------------------------
    Step 'checking this PC'
    $build = [Environment]::OSVersion.Version.Build
    if (-not [Environment]::Is64BitOperatingSystem) { Fail 'WheelBri needs 64-bit Windows.' }
    if ($build -lt 7601) { Fail "WheelBri needs Windows 7 SP1 (build 7601) or newer; this is build $build." }
    if ($withDrivers) {
        Write-Host "    Windows build $build, 64-bit: fine"
    } else {
        Write-Host ("    Windows build $build, 64-bit: the in-game shim way only (the virtual G920 needs " +
                    'Windows 11): no drivers, no WheelBri Helper')
        # The app and its shim are built against the Universal C Runtime (ucrtbase.dll): in Windows 10, an update for
        # Windows 7. Without it the app does not start and games cannot load the shim.
        if (-not (Test-Path -LiteralPath (Join-Path $env:SystemRoot 'System32\ucrtbase.dll'))) {
            Fail ('WheelBri needs the Universal C Runtime, which this Windows does not have yet: install update ' +
                  'KB2999226 (Windows Update, or "Update for Universal C Runtime in Windows" from Microsoft), then ' +
                  'run Setup again.')
        }
        Write-Host '    Universal C Runtime: there'
    }
    # The same list as scripts/make/package.ps1's $expected (keep them in step). The driver scripts: the installs below,
    # and the removals the uninstaller runs from the install folder.
    $requiredFiles = @('wheelbri.exe', 'wheelbri-service.exe', 'wheelbri-helper.exe',
                       'config\g25.ini', 'config\g27.ini', 'config\g920.ini', 'config\g923.ini',
                       'config\wheelbri-shim.ini', 'uninstall.ps1', 'Uninstall.cmd', 'driver\common.ps1',
                       'shim\dinput8.dll', 'shim\x86\dinput8.dll')
    if ($withDrivers) {
        $requiredFiles += @('driver\install-driver.ps1', 'driver\uninstall-driver.ps1',
                            'driver\hide\install-hide.ps1', 'driver\hide\uninstall-hide.ps1',
                            'driver\signing-certificate.txt')
    }
    foreach ($required in $requiredFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $package $required))) {
            Fail "the package is incomplete ($required is missing): unpack the whole package again."
        }
    }
    $driverFiles = @()
    if ($withDrivers) {
        $driverFiles = @('x64\Release\wheelbri-wheel\wheelbri-wheel', 'hide\x64\Release\wheelbri-hide\wheelbri-hide')
    }
    foreach ($driver in $driverFiles) {
        foreach ($extension in '.inf', '.cat', '.dll') {
            if (-not (Test-Path -LiteralPath (Join-Path $package "driver\$driver$extension"))) {
                Fail "the package is incomplete (driver\$driver$extension is missing): unpack the whole package again."
            }
        }
    }
    . (Join-Path $package 'driver\common.ps1')   # the install folder's helpers (Remove-Tree, Remove-InstallFolder)
    # The install folder: new, empty, or an earlier install (its program or its installed-files.txt is there).
    # Anything else is refused: the uninstaller removes what the installer put there, and other files do not belong
    # in it. wheelemu.exe: the program's name before the rename to WheelBri.
    if (Test-Path -LiteralPath $InstallDir) {
        $isFolder = Test-Path -LiteralPath $InstallDir -PathType Container
        $empty = $isFolder -and @(Get-ChildItem -LiteralPath $InstallDir -Force).Count -eq 0
        # An earlier install: one with its installed-files.txt, or the folder Settings > Apps records for WheelBri (or
        # wheelemu) with the program in it. Not any folder that happens to hold a wheelbri.exe (a build folder, a
        # portable copy): without a list, every file in it would count as WheelBri's and go at uninstall.
        $earlier = Test-Path -LiteralPath (Join-Path $InstallDir 'installed-files.txt') -PathType Leaf
        if (-not $earlier) {
            foreach ($name in 'wheelbri', 'wheelemu') {
                $recorded = (Get-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\$name" `
                                              -Name InstallLocation -ErrorAction SilentlyContinue).InstallLocation
                if ($recorded -and $recorded.TrimEnd('\') -ieq $InstallDir.TrimEnd('\') -and
                    (Test-Path -LiteralPath (Join-Path $InstallDir "$name.exe") -PathType Leaf)) { $earlier = $true }
            }
        }
        if (Test-Path -LiteralPath (Join-Path $InstallDir 'install.ps1')) {
            Fail "$InstallDir is a WheelBri package, not an install folder: choose another folder (-InstallDir)."
        }
        if (-not $isFolder -or (-not $empty -and -not $earlier)) {
            Fail "$InstallDir is not empty and holds no WheelBri installation: choose an empty or new folder (-InstallDir)."
        }
    }
    if ($withDrivers) {
        Write-Host ('    the hiding filter is only added now: the app puts it on the devices of your rig (the Rig ' +
                    'tab) the first time you press Start')
        Write-Host ('    the drivers are signed with WheelBri''s own certificate (not Microsoft''s): it is added to ' +
                    'the certificates this PC trusts (the uninstaller can remove it)')
    }

    # --- 2. a running copy ----------------------------------------------------------------------------------------
    Step 'closing a running WheelBri'
    # Also the same programs under the project's old name (wheelemu, before the rename to wheelbri).
    $running = @(Get-Process -Name 'wheelbri', 'wheelbri-service', 'wheelbri-bridge', 'wheelemu', 'wheelemu-service',
                                   'wheelemu-bridge' -ErrorAction SilentlyContinue)
    if ($running.Count -eq 0) {
        Write-Host '    none running'
    } else {
        foreach ($process in $running) {
            Write-Host "    closing $($process.ProcessName) ($($process.Id))"
            # The window first, so the app stops its session cleanly (the real wheel shown again).
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
        # An ended service could leave the wheel hidden: clear that (the hide filter also ignores a dead process).
        $oldService = Join-Path $InstallDir 'wheelbri-service.exe'
        if (Test-Path -LiteralPath $oldService) { & $oldService --unhide | Out-Null }
    }
    # The WheelBri Helper service of an earlier install: stopped while its program is replaced (it removes the virtual
    # G920 it made and shows the rig again), started again below.
    if (Stop-HelperService) {
        $helperWasThere = $true
        Write-Host '    stopped the WheelBri Helper service'
    }

    # --- 2b. a version from before the rename (wheelemu) ------------------------------------------------------------
    # Everything that version named wheelemu goes: its Settings > Apps entry (its folder is handled with the files
    # below), its hide setting and its virtual devices. Its drivers (wheelemu-*.inf) are removed by the driver install
    # scripts in step 4; its data (%LOCALAPPDATA%\wheelemu) moves to %LOCALAPPDATA%\wheelbri when the app first starts.
    $legacy = 'wheelemu'
    $legacyKey = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\$legacy"
    $legacyInstall = (Get-ItemProperty -Path $legacyKey -Name InstallLocation -ErrorAction SilentlyContinue).InstallLocation
    # Get-PnpDevice is Windows 8+ (that version needed Windows 11 anyway).
    $legacyDevices = @()
    if (Get-Command Get-PnpDevice -ErrorAction SilentlyContinue) {
        $legacyDevices = @(Get-PnpDevice -ErrorAction SilentlyContinue |
                           Where-Object { $_.InstanceId -like "SWD\$legacy\*" })
    }
    if ($legacyInstall -or $legacyDevices.Count -gt 0 -or (Test-Path "HKLM:\SOFTWARE\$legacy")) {
        Write-Host "    removing the version from before the rename ($legacy)"   # part of step 2: not numbered
        foreach ($device in $legacyDevices) {
            pnputil /remove-device "$($device.InstanceId)" | Out-Null
            $code = $LASTEXITCODE
            if ($script:PnputilOk -contains $code) {   # 0, 259, 3010 (common.ps1)
                Write-Host "    removed the old device $($device.InstanceId)"
                if ($code -eq 3010) { Warn '    Windows finishes removing it at the next restart' }
            } else {
                Warn ("    could not remove the old device $($device.InstanceId) (pnputil exit code $code): Device Manager > " +
                      'View > Show hidden devices > Uninstall')
            }
        }
        Remove-Item "HKLM:\SOFTWARE\$legacy" -Recurse -Force -ErrorAction SilentlyContinue
        Write-Host "    removed HKLM\SOFTWARE\$legacy"
    }

    # --- 3. files -------------------------------------------------------------------------------------------------
    Step "copying the program to $InstallDir"
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    # Outside Program Files a folder gets its drive's permissions, where any program of the user may change files;
    # the app runs as Administrator from here, so it gets Program Files' rules: only Administrators and SYSTEM
    # change it, users read and run it.
    $programFolders = @($env:ProgramW6432, $env:ProgramFiles, ${env:ProgramFiles(x86)}) | Where-Object { $_ } |
        ForEach-Object { $_.TrimEnd('\') + '\' }
    if (-not ($programFolders | Where-Object { ($InstallDir + '\').StartsWith($_, [StringComparison]::OrdinalIgnoreCase) })) {
        & icacls $InstallDir /inheritance:r /grant:r '*S-1-5-32-544:(OI)(CI)F' '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-545:(OI)(CI)RX' | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Fail "could not protect $InstallDir (icacls exit code $LASTEXITCODE): install into Program Files instead."
        }
        Write-Host "    $InstallDir protected like Program Files (only Administrators may change it)"
    }
    # An earlier install in another folder (C:\Program Files\wheelemu, or another folder picked then): its settings come
    # along, and the old folder goes once the new one works. Only a real install folder (the app's exe, no
    # install.ps1). Its entry in Settings > Apps is this version's (wheelbri) or the one from before the rename.
    $uninstallKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\wheelbri'
    $previous = (Get-ItemProperty -Path $uninstallKey -Name InstallLocation -ErrorAction SilentlyContinue).InstallLocation
    if (-not $previous) { $previous = $legacyInstall }
    $oldFolder = $null
    $isInstall = { param($folder) ((Test-Path -LiteralPath (Join-Path $folder 'wheelbri.exe')) -or
                                   (Test-Path -LiteralPath (Join-Path $folder "$legacy.exe"))) -and
                                  -not (Test-Path -LiteralPath (Join-Path $folder 'install.ps1')) }
    # The version from before the rename in this same folder: its program files go (their names changed, so the copy
    # below would leave them next to the new ones); its config stays.
    if ($previous -and ($previous.TrimEnd('\') -ieq $InstallDir.TrimEnd('\')) -and (Test-Path -LiteralPath (Join-Path $InstallDir "$legacy.exe"))) {
        Get-ChildItem -LiteralPath $InstallDir -Force | Where-Object { $_.Name -ne 'config' } |
            ForEach-Object { Remove-Tree $_.FullName }   # links removed as links, never followed
        Write-Host "    removed the old program files from $InstallDir (your config stays)"
    }
    if ($previous -and ($previous.TrimEnd('\') -ine $InstallDir.TrimEnd('\')) -and (& $isInstall $previous)) {
        $oldFolder = $previous
        New-Item -ItemType Directory -Force -Path (Join-Path $InstallDir 'config') | Out-Null
        foreach ($ini in Get-ChildItem -LiteralPath (Join-Path $previous 'config') -Filter '*.ini' -ErrorAction SilentlyContinue) {
            $target = Join-Path $InstallDir "config\$($ini.Name)"
            if (-not (Test-Path -LiteralPath $target)) { Copy-Item -LiteralPath $ini.FullName -Destination $target }
        }
        # Its wheelbri-shim.ini from shim\, where versions before 2026-10-17 put it (see below).
        $oldShim = Join-Path $previous 'shim\wheelbri-shim.ini'
        $target = Join-Path $InstallDir 'config\wheelbri-shim.ini'
        if ((Test-Path -LiteralPath $oldShim -PathType Leaf) -and -not (Test-Path -LiteralPath $target)) {
            Copy-Item -LiteralPath $oldShim -Destination $target
        }
        Write-Host "    an earlier install is in ${previous}: its settings are carried over, the folder is removed below"
    }
    # installed-files.txt: every file the installer put in the folder (relative paths), so that removing it deletes
    # those and nothing else (common.ps1 Remove-InstallFolder). Kept across upgrades: an older version's files stay
    # listed while they are there. An install from before the list: all of it is WheelBri's (checked in step 1).
    $manifest = Join-Path $InstallDir 'installed-files.txt'
    $listed = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    if (Test-Path -LiteralPath $manifest) {
        Get-Content -LiteralPath $manifest | ForEach-Object { if ($_.Trim()) { [void]$listed.Add($_.Trim()) } }
    } else {
        Get-ChildItem -LiteralPath $InstallDir -Recurse -File -Force |
            ForEach-Object { [void]$listed.Add($_.FullName.Substring($InstallDir.Length).TrimStart('\')) }
    }
    if ($oldFolder) {
        Get-ChildItem -LiteralPath (Join-Path $InstallDir 'config') -Filter '*.ini' -ErrorAction SilentlyContinue |
            ForEach-Object { [void]$listed.Add("config\$($_.Name)") }   # the carried-over settings
    }
    # wheelbri-shim.ini was in shim\ until 2026-10-17, where the app never looked (it reads config\wheelbri-shim.ini):
    # an earlier install's copy moves to config\ unless one is there, and is then kept like any edited config below.
    $oldShimSettings = Join-Path $InstallDir 'shim\wheelbri-shim.ini'
    $shimSettings = Join-Path $InstallDir 'config\wheelbri-shim.ini'
    if ((Test-Path -LiteralPath $oldShimSettings -PathType Leaf) -and -not (Test-Path -LiteralPath $shimSettings)) {
        New-Item -ItemType Directory -Force -Path (Join-Path $InstallDir 'config') | Out-Null
        Move-Item -LiteralPath $oldShimSettings -Destination $shimSettings
        Write-Host '    moved shim\wheelbri-shim.ini to config\, where the app reads it'
    }
    $sourceRoot = (Resolve-Path -LiteralPath $package).Path.TrimEnd('\')
    $copied = 0
    # default-configs.txt: the SHA-256 of each default config\*.ini of this version ("config\<name><TAB><hash>"),
    # so that the uninstaller tells the configs the user changed (kept, never deleted) from the defaults.
    $defaultConfigs = @()
    foreach ($file in Get-ChildItem -LiteralPath $sourceRoot -Recurse -File) {
        $relative = $file.FullName.Substring($sourceRoot.Length + 1)
        if ($relative -in @('Setup.cmd', 'install.ps1')) { continue }   # the installer stays in the package
        if ($relative -like 'win7\*') { continue }   # Windows 7's prerequisites, for before the install
        $target = Join-Path $InstallDir $relative
        New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
        if ($relative -like 'config\*.ini') {
            $defaultConfigs += "$relative`t$((Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash)"
        }
        # An edited config stays: the new default goes next to it as .new.
        if ($relative -like 'config\*.ini' -and (Test-Path -LiteralPath $target)) {
            [void]$listed.Add($relative)
            if ((Get-FileHash -LiteralPath $target).Hash -ne (Get-FileHash -LiteralPath $file.FullName).Hash) {
                Copy-Item -LiteralPath $file.FullName -Destination "$target.new" -Force
                [void]$listed.Add("$relative.new")
                Write-Host "    kept your $relative (the new default is $relative.new)"
            }
            continue
        }
        Copy-Item -LiteralPath $file.FullName -Destination $target -Force
        [void]$listed.Add($relative)
        $copied++
    }
    Write-Host "    $copied files copied"
    $defaultsRecord = Join-Path $InstallDir 'default-configs.txt'
    Set-Content -LiteralPath $defaultsRecord -Value $defaultConfigs -Encoding UTF8
    [void]$listed.Add('default-configs.txt')
    # The built-in configs' older names (for the rig since 2026-10-04, the G29 gone; one per mode until 2026-10-15): the
    # new ones (g920.ini, g25.ini, g27.ini, g923.ini) replace them. Your own mappings are in %LOCALAPPDATA%, never here.
    $olderConfigs = @('moza-r3-as-g920.ini', 'moza-r3-as-g29.ini', 'rig-as-g920.ini', 'rig-as-g29.ini') +
                    @(foreach ($t in 'g920', 'g25', 'g27') { "$t-virtual-device.ini"; "$t-in-game-shim.ini" })
    foreach ($old in $olderConfigs) {
        foreach ($file in (Join-Path $InstallDir "config\$old"), (Join-Path $InstallDir "config\$old.new")) {
            Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue
        }
    }
    # The list: only what is there now (the old configs above are gone), sorted; not itself.
    $lines = @($listed | Where-Object { ($_ -ine 'installed-files.txt') -and (Test-ManifestEntry $_) -and
                                        (Test-Path -LiteralPath (Join-Path $InstallDir $_) -PathType Leaf) } | Sort-Object)
    Set-Content -LiteralPath $manifest -Value $lines -Encoding UTF8
    Write-Host "    $($lines.Count) files listed in installed-files.txt (what the uninstaller removes)"

    # --- 4. drivers -----------------------------------------------------------------------------------------------
    Step 'installing the drivers'
    $hidePending = $false   # the hiding filter is installed but Windows starts it only after a restart
    $hideCode = $null       # install-hide.ps1's exit code (the package)
    if (-not $withDrivers) { Write-Host '    none on this Windows (the in-game shim way needs none)' }
    $drivers = if (-not $withDrivers) { @() } else { @(
        @{ Name = 'virtual wheel (wheelbri-wheel)'; Script = 'driver\install-driver.ps1'; Required = $true },
        @{ Name = 'hiding filter (wheelbri-hide)'; Script = 'driver\hide\install-hide.ps1'; Required = $false }
    ) }
    foreach ($driver in $drivers) {
        Write-Host "  $($driver.Name)"
        $code = Run-Script (Join-Path $InstallDir $driver.Script)
        if ($code -eq 0) {
            Write-Host "    installed" -ForegroundColor Green
        } elseif ($driver.Script -like '*install-hide.ps1' -and $code -eq 2) {
            $hidePending = $true
            Warn '    the hiding filter is installed but Windows starts it only after a restart: restart Windows'
        } elseif ($driver.Required) {
            Fail "the $($driver.Name) driver did not install (exit code $code); the lines above say why."
        } else {
            Warn "    the $($driver.Name) driver did not install (exit code $code); the app works without it (see above)"
        }
        if ($driver.Script -like '*install-hide.ps1') { $hideCode = $code }
    }
    # Every release is signed with a certificate of its own: the ones earlier versions trusted go once no installed
    # driver is signed with them any more (common.ps1).
    if ($withDrivers -and (Remove-UnusedSigningCertificates) -gt 0) {
        Warn '    an earlier certificate could not be removed (see above); the uninstaller or clean-pc can remove it later'
    }

    # The filter on the saved rig's devices now, after a reinstall that kept the user's settings, so the first Start
    # does not have to (it took ~30 s with the wheel reconnecting, and looked like a failed Start). The installed
    # service does it exactly as Start does (the rig's game controllers as DirectInput lists them; reading USB ids here
    # missed the Moza R3). No saved rig (a new PC), settings of another account (Administrator given by another
    # user), or the wheel not plugged in: the first Start does it, as before.
    # The WheelBri Helper: what lets the app run without Administrator (the virtual G920, hiding the rig). A service
    # running as LocalSystem from the install folder, started with Windows and restarted after a crash.
    # Not on Windows before 11: it only serves the virtual G920 and the hiding filter, which are not there.
    $helperProblem = $null
    if ($withDrivers) {
        Write-Host '  WheelBri Helper (service)'
        $helperProblem = Install-HelperService (Join-Path $InstallDir 'wheelbri-helper.exe')
    }
    if (-not $withDrivers) {
        # nothing to say: the line above says there are no drivers
    } elseif ($helperProblem) {
        Warn ("    the WheelBri Helper is not running ($helperProblem): until it does, the app shows HELPER NOT " +
              'INSTALLED and the virtual G920 works only when WheelBri is started as Administrator')
    } else {
        $helperWasThere = $false   # running: nothing to start again if a later step fails
        Write-Host '    running (starts with Windows)' -ForegroundColor Green
    }

    if ($hideCode -eq 0 -or $hideCode -eq 2) {
        Write-Host '  hiding filter on your saved rig''s devices'
        $service = Join-Path $InstallDir 'wheelbri-service.exe'
        # Its log lines come on stderr. Windows PowerShell 5.1 turns each into an error record, and with
        # $ErrorActionPreference = 'Stop' the first one ("logging to ...") ended the install, so: Continue here,
        # its result line (stdout) shown as is, and from its log (all of it is in wheelbri-service-<date>-<NN>.log)
        # only the warnings and errors, without the time and source prefix.
        $previous = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            & $service --prepare-hide 2>&1 | ForEach-Object {
                if ($_ -is [System.Management.Automation.ErrorRecord]) {
                    $line = $_.Exception.Message
                    if ($line -match '^\[[^\]]*\] \[(warn\w*|error|critical)\s*\] \[[^\]]*\] \[tid \d+\s*\] (.*?)(\s+\([^()]*:\d+\))?$') {
                        Write-Host "    $($Matches[2])" -ForegroundColor Yellow
                    }
                } elseif ("$_".Trim()) {
                    Write-Host "    $_"
                }
            }
            $code = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previous
        }
        if ($code -eq 0) {
            Write-Host '    done' -ForegroundColor Green
        } elseif ($code -eq 2) {
            $hidePending = $true
        } elseif ($code -ne 3) {
            Warn "    not done (exit code $code): the app sets it up at the first Start instead"
        }
    }

    if ($oldFolder) {
        # Its installed-files.txt if it has one (only those files and WheelBri's subfolders), else all of it.
        if (-not (Remove-InstallFolder $oldFolder)) { Warn "    the old folder $oldFolder could not be removed completely: delete it by hand" }
        else { Write-Host "    removed the old folder $oldFolder" }
    }

    # --- 5. shortcuts ---------------------------------------------------------------------------------------------
    Step 'shortcuts'
    $shell = New-Object -ComObject WScript.Shell
    $app = Join-Path $InstallDir 'wheelbri.exe'
    function New-Shortcut([string]$path, [string]$target, [string]$arguments, [string]$folder, [string]$description) {
        $link = $shell.CreateShortcut($path)
        $link.TargetPath = $target
        $link.Arguments = $arguments
        $link.WorkingDirectory = $folder
        $link.Description = $description
        $link.IconLocation = "$app,0"
        $link.Save()
        Write-Host "    $path"
    }
    # Start menu: a "WheelBri" folder with the app and its uninstaller. Older versions' shortcuts (named
    # wheel-emulator) go.
    $programs = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs'
    $desktop = [Environment]::GetFolderPath('CommonDesktopDirectory')
    Remove-Item (Join-Path $programs 'wheel-emulator.lnk'), (Join-Path $programs 'wheel-emulator'),
                (Join-Path $desktop 'wheel-emulator.lnk') -Recurse -Force -ErrorAction SilentlyContinue
    $menu = Join-Path $programs 'WheelBri'
    New-Item -ItemType Directory -Force -Path $menu | Out-Null
    New-Shortcut (Join-Path $menu 'WheelBri.lnk') $app '' $InstallDir 'WheelBri: your rig as a Logitech G920'
    $powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    New-Shortcut (Join-Path $menu 'Uninstall WheelBri.lnk') $powershell `
        "-NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $InstallDir 'uninstall.ps1')`"" $env:SystemRoot `
        'Removes WheelBri, its drivers and shortcuts'
    if (-not $NoDesktopShortcut) {
        New-Shortcut (Join-Path $desktop 'WheelBri.lnk') $app '' `
            $InstallDir 'WheelBri: your rig as a Logitech G920'
    }

    # --- 6. uninstall entry ---------------------------------------------------------------------------------------
    Step 'adding WheelBri to Settings > Apps'
    $key = $uninstallKey   # the same key every version, so an upgrade replaces the entry instead of adding one
    Remove-Item $legacyKey -Recurse -Force -ErrorAction SilentlyContinue   # the entry from before the rename
    New-Item -Path $key -Force | Out-Null
    $uninstall = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $InstallDir 'uninstall.ps1')`""
    $entries = @{
        DisplayName = 'WheelBri'
        DisplayVersion = $version
        Publisher = 'WheelBri'
        InstallLocation = $InstallDir
        DisplayIcon = Join-Path $InstallDir 'wheelbri.exe'
        UninstallString = $uninstall
        QuietUninstallString = "$uninstall -Quiet"
        InstallDate = (Get-Date -Format yyyyMMdd)
    }
    foreach ($name in $entries.Keys) { Set-ItemProperty -Path $key -Name $name -Value $entries[$name] }
    Set-ItemProperty -Path $key -Name NoModify -Value 1 -Type DWord
    Set-ItemProperty -Path $key -Name NoRepair -Value 1 -Type DWord
    $size = [int]((Get-ChildItem $InstallDir -Recurse -File | Measure-Object Length -Sum).Sum / 1KB)
    Set-ItemProperty -Path $key -Name EstimatedSize -Value $size -Type DWord
    Write-Host '    done'

    # --- 6b. Firefox and game controllers (only when asked) -------------------------------------------------------
    # Firefox opens every game controller for its gamepad support, and while a program has the wheel open Windows
    # will not restart it with the hiding filter, so Start fails (2026-11-02: until Firefox was closed). Asked only
    # when Firefox is installed and its gamepad support is still on (default no); -StopBrowserGamepads: yes without
    # asking; -Quiet alone: not asked. Yes sets Firefox's own policy dom.gamepad.enabled = false (locked) for the user
    # signed in to Windows here (HKEY_USERS\<user>\Software\Policies\Mozilla\Firefox Preferences, merged with the
    # preferences already set there) and records that user in HKLM\SOFTWARE\wheelbri BrowserGamepadPolicy, so the
    # uninstaller takes out exactly that preference. Firefox only.
    $desktopUser = Get-DesktopUserSid
    $firefoxKey = "Registry::HKEY_USERS\$desktopUser\Software\Policies\Mozilla\Firefox"
    $appPaths = 'Software\Microsoft\Windows\CurrentVersion\App Paths\firefox.exe'
    $firefoxInstalled = (Test-Path -LiteralPath "HKLM:\$appPaths") -or
                        (Test-Path -LiteralPath "Registry::HKEY_USERS\$desktopUser\$appPaths")
    if ($StopBrowserGamepads -or ($firefoxInstalled -and -not $Quiet)) {
        $preferences = Get-FirefoxPreferences $firefoxKey
        $setting = if ($preferences) { $preferences.'dom.gamepad.enabled' } else { $null }
        if ($null -eq $preferences) {
            Warn ("    Firefox: its preferences policy is set in a form WheelBri does not change; to stop it holding " +
                  'the wheel open, set dom.gamepad.enabled to false in about:config')
        } elseif ($setting -and $setting.Value -eq $false) {
            Write-Host '    Firefox: its gamepad support is off already'
        } else {
            $turnOff = [bool]$StopBrowserGamepads
            if (-not $turnOff) {
                Write-Host ''
                Write-Host ('Firefox opens game controllers for websites, and while it has your wheel open WheelBri ' +
                            'cannot hide it') -ForegroundColor Yellow
                Write-Host ('from games (Start then asks you to close Firefox). WheelBri can turn that off in Firefox ' +
                            'for your') -ForegroundColor Yellow
                Write-Host 'Windows account (websites then see no game controllers; the uninstaller turns it back on).' -ForegroundColor Yellow
                $answer = Read-Host 'Turn off game controllers in Firefox? [y/N]'
                $turnOff = $answer -and $answer.Trim().ToLower() -in @('y', 'yes')
            }
            if ($turnOff) {
                $preferences | Add-Member -NotePropertyName 'dom.gamepad.enabled' -Force `
                    -NotePropertyValue ([pscustomobject]@{ Value = $false; Status = 'locked' })
                if (-not (Test-Path -LiteralPath $firefoxKey)) { New-Item -Path $firefoxKey -Force | Out-Null }
                New-ItemProperty -LiteralPath $firefoxKey -Name Preferences -PropertyType MultiString -Force `
                    -Value @($preferences | ConvertTo-Json -Depth 8 -Compress) | Out-Null
                Set-ItemProperty -Path 'HKLM:\SOFTWARE\wheelbri' -Name BrowserGamepadPolicy -Value $desktopUser
                Write-Host '    Firefox: gamepad support turned off (applies from its next start)'
            } else {
                Write-Host '    Firefox: left as it is'
            }
        }
    }

    # --- 7. check -------------------------------------------------------------------------------------------------
    Step 'checking the installation'
    if ($withDrivers) {
        $installed = pnputil /enum-drivers | Out-String
        foreach ($inf in 'wheelbri-wheel.inf', 'wheelbri-hide.inf') {
            if ($installed -match [regex]::Escape($inf)) { Write-Host "    $inf installed" }
            else { Warn "    $inf is not listed by pnputil" }
        }
        $helper = Get-Service -Name 'WheelBriHelper' -ErrorAction SilentlyContinue
        if ($helper -and $helper.Status -eq 'Running') { Write-Host '    WheelBri Helper running' }
        else { Warn '    the WheelBri Helper service is not running (services.msc > WheelBri Helper > Start)' }
    } else {
        foreach ($file in 'wheelbri.exe', 'shim\dinput8.dll', 'shim\x86\dinput8.dll') {
            if (Test-Path -LiteralPath (Join-Path $InstallDir $file)) { Write-Host "    $file there" }
            else { Warn "    $file is missing" }
        }
    }
    $exitCode = 0
} catch {
    if ($_.Exception.Message -notin @('stop', 'cancelled')) {
        Write-Host "Installation failed: $($_.Exception.Message)" -ForegroundColor Red
        Write-Host $_.ScriptStackTrace
    }
} finally {
    # Failed after the earlier install's helper was stopped: started again, so that install keeps working.
    if ($exitCode -ne 0 -and $helperWasThere) { Start-Service -Name 'WheelBriHelper' -ErrorAction SilentlyContinue }
    Write-Host ''
    if ($exitCode -eq 0) {
        Write-Host "WheelBri $version is installed." -ForegroundColor Green
        if ($warnings.Count -gt 0) {
            Write-Host 'Things to look at:' -ForegroundColor Yellow
            $warnings | ForEach-Object { Write-Host "  $($_.Trim())" -ForegroundColor Yellow }
        }
        if ($withDrivers) {
            Write-Host 'Next: restart Windows (asked below), plug in your wheel, open "WheelBri" from the Start'
            Write-Host 'menu, set up your rig (Rig tab) and press Start. joy.cpl then lists the G920, not your rig.'
            Write-Host 'To remove it: Start menu > WheelBri > Uninstall WheelBri, or Settings > Apps > WheelBri.'
        } else {
            Write-Host 'Next: plug in your wheel, open "WheelBri" from the Start menu and set up your rig (Rig tab).'
            Write-Host 'On the Games tab pick your game, press Choose folder... (the folder with its .exe) and'
            Write-Host 'Use for Start, then press Start before the game. To remove it: Start menu > WheelBri >'
            Write-Host 'Uninstall WheelBri.'
        }
    }
    Write-Host "Log: $logFolder"
    # The restart question: only here, at the end of an install that worked (never in the app or the uninstaller).
    $restart = $false
    if ($exitCode -eq 0 -and -not $Quiet -and $withDrivers) {   # no drivers: nothing a restart would finish
        Write-Host ''
        if ($hidePending) {
            Write-Host 'A restart of Windows is needed: the filter that hides the rig from games starts only after it.' -ForegroundColor Yellow
        } else {
            Write-Host 'Restart Windows to make sure every driver is installed properly (the filter that hides the rig' -ForegroundColor Yellow
            Write-Host 'from games often starts only after a restart).' -ForegroundColor Yellow
        }
        $answer = Read-Host 'Restart Windows now? Save your work in other programs first. [Y/n]'
        $restart = -not $answer -or -not $answer.Trim() -or $answer.Trim().ToLower() -in @('y', 'yes')
        if ($restart) {
            Write-Host 'Windows restarts in 2 seconds.'
        } else {
            Write-Host 'Not restarting now: restart Windows before you use WheelBri.' -ForegroundColor Yellow
        }
    }
    if ($transcript) { Stop-Transcript | Out-Null }
    if ($restart) {
        # The user just said yes: 2 s, enough for this window to close, no countdown (the user's rule, 2026-10-06).
        & shutdown.exe /r /t 2 /c 'WheelBri: restarting so its drivers are installed properly.' /d p:4:2
    } elseif (-not $Quiet -and $exitCode -ne 0) {
        # Only a failure keeps the window open (Setup.exe's window would close before the red lines could be read).
        # A success just ends: everything it said is in the log too.
        Read-Host 'Press Enter to close' | Out-Null
    }
}
exit $exitCode
