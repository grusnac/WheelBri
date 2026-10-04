# Installs WheelBri on a PC that has nothing of it yet: the app, its two drivers, shortcuts and an entry in
# Settings > Apps (to uninstall). Run it from the unpacked package (Setup.exe does that), as Administrator; it asks
# for Administrator itself when needed. Running it again upgrades in place and keeps an edited config.
#
#   install.ps1                         install to C:\Program Files\WheelBri
#   install.ps1 -InstallDir D:\wheelbri
#   install.ps1 -NoDesktopShortcut
#   install.ps1 -Quiet                  no questions and no pause, even when it fails (for scripted installs); no
#                                       restart either (restart Windows yourself afterwards)
#
# Two questions at most: Windows' Administrator prompt, and at the end of an install that worked, whether to restart
# Windows now (default yes). The restart makes sure every driver is in use: the hiding filter in particular often
# starts on a device only after one (when it was on another driver, e.g. the wheel maker's, Windows finishes the switch at
# the restart). When the install failed the window waits for Enter (so the reason can be read).
#
# What it changes on this PC (all undone by the uninstaller):
#   - files in the install folder (listed in its installed-files.txt: the uninstaller removes those, not the whole
#     folder; an -InstallDir that is not empty and holds no WheelBri install is refused); Start menu (and desktop)
#     shortcut "WheelBri"; an uninstall entry
#   - two driver packages (pnputil): the virtual G920 (wheelbri-g920) and the
#     filter that hides the rig from games (wheelbri-hide; only added here: the app puts it on the devices of the
#     rig set up in Wheel setup, whatever they are, when Start is pressed)
#   - the certificate the drivers are signed with, added to this PC's trusted Root and TrustedPublisher stores (the
#     drivers are signed by WheelBri's own test certificate, not by Microsoft); its thumbprint is recorded in
#     HKLM\SOFTWARE\wheelbri\SigningCertificates, so the uninstaller removes exactly that certificate
#
# Exit codes: 0 installed (possibly with warnings, listed at the end), 1 failed (the red line says why).
# Everything is also written to %LOCALAPPDATA%\wheelbri\logs\wheelbri-install-<date>.log (with every other log).

param(
    # Default: the 64-bit Program Files. Not $env:ProgramFiles: in a 32-bit PowerShell (MSYS2 can start one) that is
    # "Program Files (x86)", and it was passed on to the 64-bit run below.
    [string]$InstallDir = '',
    [switch]$NoDesktopShortcut,
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
$version = if (Test-Path (Join-Path $package 'VERSION.txt')) { (Get-Content (Join-Path $package 'VERSION.txt') -Raw).Trim() } else { '0.0.0' }
# Every log in one folder, the user's %LOCALAPPDATA%\wheelbri\logs (the app and both drivers write there too): the
# drivers run as LocalService, so it may write there, and HKLM\SOFTWARE\wheelbri LogFolder tells them where (the
# app does the same at every start, see ServiceLogging.hpp shareLogFolder).
# (Set up as the first thing in the try below, so a failure there is reported and the window waits.)
$logFolder = Join-Path $env:LOCALAPPDATA 'wheelbri\logs'
$transcript = $false

$warnings = New-Object System.Collections.Generic.List[string]
$step = 0
function Step([string]$text) {
    $script:step++
    Write-Host ''
    Write-Host "[$script:step/7] $text" -ForegroundColor Cyan
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
# Runs one of the package's PowerShell scripts in its own process (they end with `exit`); returns its exit code.
function Run-Script([string]$path, [string[]]$arguments = @()) {
    $powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    & $powershell -NoProfile -ExecutionPolicy Bypass -File $path @arguments | ForEach-Object { Write-Host "    $_" }
    return $LASTEXITCODE
}

$exitCode = 1
try {
    New-Item -ItemType Directory -Force -Path $logFolder | Out-Null
    Start-Transcript -Append -Path (Join-Path $logFolder "wheelbri-install-$(Get-Date -Format yyyy-MM-dd).log") | Out-Null
    $transcript = $true
    & icacls $logFolder /grant '*S-1-5-19:(OI)(CI)M' | Out-Null
    if (-not (Test-Path 'HKLM:\SOFTWARE\wheelbri')) { New-Item -Path 'HKLM:\SOFTWARE\wheelbri' | Out-Null }
    Set-ItemProperty -Path 'HKLM:\SOFTWARE\wheelbri' -Name 'LogFolder' -Value $logFolder

    Write-Host "WheelBri $version setup" -ForegroundColor Green
    Write-Host "package:      $package"
    Write-Host "install into: $InstallDir"

    # --- 1. this PC ---------------------------------------------------------------------------------------------
    Step 'checking this PC'
    $build = [Environment]::OSVersion.Version.Build
    if (-not [Environment]::Is64BitOperatingSystem) { Fail 'WheelBri needs 64-bit Windows.' }
    if ($build -lt 22000) {
        Fail "WheelBri needs Windows 11 (build 22000 or newer) for its virtual G920 driver; this is build $build."
    }
    Write-Host "    Windows build $build, 64-bit: fine"
    foreach ($required in 'wheelbri.exe', 'wheelbri-service.exe', 'config\g920-virtual-device.ini', 'config\g920-in-game-shim.ini', 'uninstall.ps1', 'Uninstall.cmd',
                          'driver\common.ps1', 'driver\install-driver.ps1', 'driver\hide\install-hide.ps1') {
        if (-not (Test-Path (Join-Path $package $required))) {
            Fail "the package is incomplete ($required is missing): unpack the whole package again."
        }
    }
    foreach ($driver in 'x64\Release\wheelbri-g920\wheelbri-g920', 'hide\x64\Release\wheelbri-hide\wheelbri-hide') {
        foreach ($extension in '.inf', '.cat', '.dll') {
            if (-not (Test-Path (Join-Path $package "driver\$driver$extension"))) {
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
        $earlier = $false
        foreach ($marker in 'wheelbri.exe', 'wheelemu.exe', 'installed-files.txt') {
            if (Test-Path -LiteralPath (Join-Path $InstallDir $marker) -PathType Leaf) { $earlier = $true }
        }
        if (Test-Path -LiteralPath (Join-Path $InstallDir 'install.ps1')) {
            Fail "$InstallDir is a WheelBri package, not an install folder: choose another folder (-InstallDir)."
        }
        if (-not $isFolder -or (-not $empty -and -not $earlier)) {
            Fail "$InstallDir is not empty and holds no WheelBri installation: choose an empty or new folder (-InstallDir)."
        }
    }
    Write-Host ('    the hiding filter is only added now: the app puts it on the devices of your rig (Wheel setup) ' +
                'the first time you press Start')

    Write-Host ('    the drivers are signed with WheelBri''s own certificate (not Microsoft''s): it is added to the ' +
                'certificates this PC trusts (the uninstaller can remove it)')

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
        if (Test-Path $oldService) { & $oldService --unhide | Out-Null }
    }

    # --- 2b. a version from before the rename (wheelemu) ------------------------------------------------------------
    # Everything that version named wheelemu goes: its Settings > Apps entry (its folder is handled with the files
    # below), its hide setting and its virtual devices. Its drivers (wheelemu-*.inf) are removed by the driver install
    # scripts in step 4; its data (%LOCALAPPDATA%\wheelemu) moves to %LOCALAPPDATA%\wheelbri when the app first starts.
    $legacy = 'wheelemu'
    $legacyKey = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\$legacy"
    $legacyInstall = (Get-ItemProperty -Path $legacyKey -Name InstallLocation -ErrorAction SilentlyContinue).InstallLocation
    $legacyDevices = @(Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like "SWD\$legacy\*" })
    if ($legacyInstall -or $legacyDevices.Count -gt 0 -or (Test-Path "HKLM:\SOFTWARE\$legacy")) {
        Step "removing the version from before the rename ($legacy)"
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
    # An earlier install in another folder (C:\Program Files\wheelemu, or another folder picked then): its settings come
    # along, and the old folder goes once the new one works. Only a real install folder (the app's exe, no
    # install.ps1). Its entry in Settings > Apps is this version's (wheelbri) or the one from before the rename.
    $uninstallKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\wheelbri'
    $previous = (Get-ItemProperty -Path $uninstallKey -Name InstallLocation -ErrorAction SilentlyContinue).InstallLocation
    if (-not $previous) { $previous = $legacyInstall }
    $oldFolder = $null
    $isInstall = { param($folder) ((Test-Path (Join-Path $folder 'wheelbri.exe')) -or
                                   (Test-Path (Join-Path $folder "$legacy.exe"))) -and
                                  -not (Test-Path (Join-Path $folder 'install.ps1')) }
    # The version from before the rename in this same folder: its program files go (their names changed, so the copy
    # below would leave them next to the new ones); its config stays.
    if ($previous -and ($previous.TrimEnd('\') -ieq $InstallDir.TrimEnd('\')) -and (Test-Path (Join-Path $InstallDir "$legacy.exe"))) {
        Get-ChildItem -LiteralPath $InstallDir -Force | Where-Object { $_.Name -ne 'config' } |
            ForEach-Object { Remove-Tree $_.FullName }   # links removed as links, never followed
        Write-Host "    removed the old program files from $InstallDir (your config stays)"
    }
    if ($previous -and ($previous.TrimEnd('\') -ine $InstallDir.TrimEnd('\')) -and (& $isInstall $previous)) {
        $oldFolder = $previous
        New-Item -ItemType Directory -Force -Path (Join-Path $InstallDir 'config') | Out-Null
        foreach ($ini in Get-ChildItem (Join-Path $previous 'config') -Filter '*.ini' -ErrorAction SilentlyContinue) {
            $target = Join-Path $InstallDir "config\$($ini.Name)"
            if (-not (Test-Path $target)) { Copy-Item $ini.FullName $target }
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
        Get-ChildItem (Join-Path $InstallDir 'config') -Filter '*.ini' -ErrorAction SilentlyContinue |
            ForEach-Object { [void]$listed.Add("config\$($_.Name)") }   # the carried-over settings
    }
    $sourceRoot = (Resolve-Path $package).Path.TrimEnd('\')
    $copied = 0
    foreach ($file in Get-ChildItem $sourceRoot -Recurse -File) {
        $relative = $file.FullName.Substring($sourceRoot.Length + 1)
        if ($relative -in @('Setup.cmd', 'install.ps1')) { continue }   # the installer stays in the package
        $target = Join-Path $InstallDir $relative
        New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
        # An edited config stays: the new default goes next to it as .new.
        if ($relative -like 'config\*.ini' -and (Test-Path $target)) {
            [void]$listed.Add($relative)
            if ((Get-FileHash $target).Hash -ne (Get-FileHash $file.FullName).Hash) {
                Copy-Item $file.FullName "$target.new" -Force
                [void]$listed.Add("$relative.new")
                Write-Host "    kept your $relative (the new default is $relative.new)"
            }
            continue
        }
        Copy-Item $file.FullName $target -Force
        [void]$listed.Add($relative)
        $copied++
    }
    Write-Host "    $copied files copied"
    # The built-in configs' older names (one per target since 2026-10-04): the new ones replace them.
    foreach ($old in 'moza-r3-as-g920.ini', 'moza-r3-as-g29.ini', 'rig-as-g920.ini', 'rig-as-g29.ini') {   # (the G29 is gone)
        foreach ($file in (Join-Path $InstallDir "config\$old"), (Join-Path $InstallDir "config\$old.new")) {
            Remove-Item $file -Force -ErrorAction SilentlyContinue
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
    $drivers = @(
        @{ Name = 'virtual G920 (wheelbri-g920)'; Script = 'driver\install-driver.ps1'; Required = $true },
        @{ Name = 'hiding filter (wheelbri-hide)'; Script = 'driver\hide\install-hide.ps1'; Required = $false }
    )
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

    # --- 7. check -------------------------------------------------------------------------------------------------
    Step 'checking the installation'
    $installed = pnputil /enum-drivers | Out-String
    foreach ($inf in 'wheelbri-g920.inf', 'wheelbri-hide.inf') {
        if ($installed -match [regex]::Escape($inf)) { Write-Host "    $inf installed" } else { Warn "    $inf is not listed by pnputil" }
    }
    $exitCode = 0
} catch {
    if ($_.Exception.Message -notin @('stop', 'cancelled')) {
        Write-Host "Installation failed: $($_.Exception.Message)" -ForegroundColor Red
        Write-Host $_.ScriptStackTrace
    }
} finally {
    Write-Host ''
    if ($exitCode -eq 0) {
        Write-Host "WheelBri $version is installed." -ForegroundColor Green
        if ($warnings.Count -gt 0) {
            Write-Host 'Things to look at:' -ForegroundColor Yellow
            $warnings | ForEach-Object { Write-Host "  $($_.Trim())" -ForegroundColor Yellow }
        }
        Write-Host 'Next: restart Windows (asked below), plug in your wheel, open "WheelBri" from the Start menu, set up'
        Write-Host 'your rig (Rig tab) and press Start. joy.cpl then lists the G920, not your rig. To remove it: Start menu >'
        Write-Host 'WheelBri > Uninstall WheelBri, or Settings > Apps > WheelBri > Uninstall.'
    }
    Write-Host "Log: $logFolder"
    # The restart question: only here, at the end of an install that worked (never in the app or the uninstaller).
    $restart = $false
    if ($exitCode -eq 0 -and -not $Quiet) {
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
            Write-Host 'Windows restarts in 15 seconds (to cancel: shutdown /a in a command prompt).'
        } else {
            Write-Host 'Not restarting now: restart Windows before you use WheelBri.' -ForegroundColor Yellow
        }
    }
    if ($transcript) { Stop-Transcript | Out-Null }
    if ($restart) {
        & shutdown.exe /r /t 15 /c 'WheelBri: restarting so its drivers are installed properly.' /d p:4:2
        Start-Sleep -Seconds 3
    } elseif (-not $Quiet) {
        if ($exitCode -eq 0) {
            Write-Host 'This window closes in 10 seconds.'
            Start-Sleep -Seconds 10
        } else {
            Read-Host 'Press Enter to close' | Out-Null
        }
    }
}
exit $exitCode
