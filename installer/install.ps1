# Installs WheelBri on a PC that has nothing of it yet: the app, its three drivers, shortcuts and an entry in
# Settings > Apps (to uninstall). Run it from the unpacked package (Setup.exe does that), as Administrator; it asks
# for Administrator itself when needed. Running it again upgrades in place and keeps an edited config.
#
#   install.ps1                         install to C:\Program Files\WheelBri
#   install.ps1 -InstallDir D:\wheelemu
#   install.ps1 -NoDesktopShortcut
#   install.ps1 -Quiet                  no pause even when it fails (for scripted installs)
#
# It asks nothing: Windows' Administrator prompt is the only question. The window closes by itself after a few
# seconds when the install worked, and waits for Enter when it failed (so the reason can be read).
#
# What it changes on this PC (all undone by the uninstaller):
#   - files in the install folder; Start menu (and desktop) shortcut "WheelBri"; an uninstall entry
#   - three driver packages (pnputil): the virtual G920 (wheelemu-g920), the Xbox controller (wheelemu-xusb) and the
#     filter that hides the real wheel from games (wheelemu-hide, on the Moza R3's game-controller interface)
#   - the certificate the drivers are signed with, added to this PC's trusted Root and TrustedPublisher stores (the
#     drivers are signed by WheelBri's own test certificate, not by Microsoft)
#
# Exit codes: 0 installed (possibly with warnings, listed at the end), 1 failed (the red line says why).
# Everything is also written to C:\ProgramData\wheelemu\logs\wheelemu-install-<date>.log.

param(
    [string]$InstallDir = (Join-Path $env:ProgramFiles 'WheelBri'),
    [switch]$NoDesktopShortcut,
    [switch]$Quiet
)

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
    $arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -InstallDir `"$InstallDir`""
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
$logFolder = Join-Path $env:ProgramData 'wheelemu\logs'
New-Item -ItemType Directory -Force -Path $logFolder | Out-Null
Start-Transcript -Append -Path (Join-Path $logFolder "wheelemu-install-$(Get-Date -Format yyyy-MM-dd).log") | Out-Null

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
    foreach ($required in 'wheelemu.exe', 'wheelemu-service.exe', 'config\moza-r3-as-g920.ini', 'uninstall.ps1', 'Uninstall.cmd',
                          'driver\common.ps1', 'driver\install-driver.ps1', 'driver\hide\install-hide.ps1',
                          'driver\xusb\install-xusb.ps1') {
        if (-not (Test-Path (Join-Path $package $required))) {
            Fail "the package is incomplete ($required is missing): unpack the whole package again."
        }
    }
    foreach ($driver in 'x64\Release\wheelemu-g920\wheelemu-g920', 'hide\x64\Release\wheelemu-hide\wheelemu-hide',
                        'xusb\x64\Release\wheelemu-xusb\wheelemu-xusb') {
        foreach ($extension in '.inf', '.cat', '.dll') {
            if (-not (Test-Path (Join-Path $package "driver\$driver$extension"))) {
                Fail "the package is incomplete (driver\$driver$extension is missing): unpack the whole package again."
            }
        }
    }
    $moza = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'HID\VID_346E&PID_0005&MI_02*' })
    if ($moza.Count -gt 0) {
        Write-Host '    Moza R3 connected: the hiding filter goes on it now (it restarts once; close Moza Pit House and games)'
    } else {
        Write-Host '    Moza R3 not connected: fine, the app puts the hiding filter on it the first time you press Start'
    }

    Write-Host ('    the drivers are signed with WheelBri''s own certificate (not Microsoft''s): it is added to the ' +
                'certificates this PC trusts (the uninstaller can remove it)')

    # --- 2. a running copy ----------------------------------------------------------------------------------------
    Step 'closing a running WheelBri'
    $running = @(Get-Process -Name 'wheelemu', 'wheelemu-service', 'wheelemu-bridge' -ErrorAction SilentlyContinue)
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
        $oldService = Join-Path $InstallDir 'wheelemu-service.exe'
        if (Test-Path $oldService) { & $oldService --unhide | Out-Null }
    }

    # --- 3. files -------------------------------------------------------------------------------------------------
    Step "copying the program to $InstallDir"
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    # An earlier install in another folder (C:\Program Files\wheelemu before the rename): its settings come along, and
    # the old folder goes once the new one works. Only a real install folder (wheelemu.exe, no install.ps1).
    $uninstallKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\wheelemu'
    $previous = (Get-ItemProperty -Path $uninstallKey -Name InstallLocation -ErrorAction SilentlyContinue).InstallLocation
    $oldFolder = $null
    if ($previous -and ($previous.TrimEnd('\') -ine $InstallDir.TrimEnd('\')) -and
        (Test-Path (Join-Path $previous 'wheelemu.exe')) -and -not (Test-Path (Join-Path $previous 'install.ps1'))) {
        $oldFolder = $previous
        New-Item -ItemType Directory -Force -Path (Join-Path $InstallDir 'config') | Out-Null
        foreach ($ini in Get-ChildItem (Join-Path $previous 'config') -Filter '*.ini' -ErrorAction SilentlyContinue) {
            $target = Join-Path $InstallDir "config\$($ini.Name)"
            if (-not (Test-Path $target)) { Copy-Item $ini.FullName $target }
        }
        Write-Host "    an earlier install is in ${previous}: its settings are carried over, the folder is removed below"
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
            if ((Get-FileHash $target).Hash -ne (Get-FileHash $file.FullName).Hash) {
                Copy-Item $file.FullName "$target.new" -Force
                Write-Host "    kept your $relative (the new default is $relative.new)"
            }
            continue
        }
        Copy-Item $file.FullName $target -Force
        $copied++
    }
    Write-Host "    $copied files copied"

    # --- 4. drivers -----------------------------------------------------------------------------------------------
    Step 'installing the drivers'
    $drivers = @(
        @{ Name = 'virtual G920 (wheelemu-g920)'; Script = 'driver\install-driver.ps1'; Required = $true },
        @{ Name = 'Xbox controller (wheelemu-xusb)'; Script = 'driver\xusb\install-xusb.ps1'; Required = $false },
        @{ Name = 'hiding filter (wheelemu-hide)'; Script = 'driver\hide\install-hide.ps1'; Required = $false }
    )
    foreach ($driver in $drivers) {
        Write-Host "  $($driver.Name)"
        $code = Run-Script (Join-Path $InstallDir $driver.Script)
        if ($code -eq 0) {
            Write-Host "    installed" -ForegroundColor Green
        } elseif ($driver.Script -like '*install-hide.ps1' -and $code -eq 2) {
            Warn '    the hiding filter is installed but not running on the R3 yet: unplug the R3 and plug it in again (or restart Windows)'
        } elseif ($driver.Required) {
            Fail "the $($driver.Name) driver did not install (exit code $code); the lines above say why."
        } else {
            Warn "    the $($driver.Name) driver did not install (exit code $code); the app works without it (see above)"
        }
    }

    if ($oldFolder) {
        Remove-Item $oldFolder -Recurse -Force -ErrorAction SilentlyContinue
        if (Test-Path $oldFolder) { Warn "    the old folder $oldFolder could not be removed completely: delete it by hand" }
        else { Write-Host "    removed the old folder $oldFolder" }
    }

    # --- 5. shortcuts ---------------------------------------------------------------------------------------------
    Step 'shortcuts'
    $shell = New-Object -ComObject WScript.Shell
    $app = Join-Path $InstallDir 'wheelemu.exe'
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
    New-Shortcut (Join-Path $menu 'WheelBri.lnk') $app '' $InstallDir 'WheelBri: Moza R3 as a Logitech G920'
    $powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    New-Shortcut (Join-Path $menu 'Uninstall WheelBri.lnk') $powershell `
        "-NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $InstallDir 'uninstall.ps1')`"" $env:SystemRoot `
        'Removes WheelBri, its drivers and shortcuts'
    if (-not $NoDesktopShortcut) {
        New-Shortcut (Join-Path $desktop 'WheelBri.lnk') $app '' `
            $InstallDir 'WheelBri: Moza R3 as a Logitech G920'
    }

    # --- 6. uninstall entry ---------------------------------------------------------------------------------------
    Step 'adding WheelBri to Settings > Apps'
    $key = $uninstallKey   # the key keeps its old name, so an upgrade replaces the entry instead of adding one
    New-Item -Path $key -Force | Out-Null
    $uninstall = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $InstallDir 'uninstall.ps1')`""
    $entries = @{
        DisplayName = 'WheelBri'
        DisplayVersion = $version
        Publisher = 'WheelBri'
        InstallLocation = $InstallDir
        DisplayIcon = Join-Path $InstallDir 'wheelemu.exe'
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
    foreach ($inf in 'wheelemu-g920.inf', 'wheelemu-xusb.inf', 'wheelemu-hide.inf') {
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
        Write-Host 'Next: plug in the R3, open "WheelBri" from the Start menu and press Start. joy.cpl then lists the'
        Write-Host 'G920 and the Xbox controller, not the R3. To remove it: Start menu > WheelBri > Uninstall'
        Write-Host 'WheelBri, or Settings > Apps > WheelBri > Uninstall.'
    }
    Write-Host "Log: $logFolder"
    Stop-Transcript | Out-Null
    if (-not $Quiet) {
        if ($exitCode -eq 0) {
            Write-Host 'This window closes in 10 seconds.'
            Start-Sleep -Seconds 10
        } else {
            Read-Host 'Press Enter to close' | Out-Null
        }
    }
}
exit $exitCode
