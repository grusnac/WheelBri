# Puts wheelbri-hide, the filter that hides the rig from games while wheelbri-service runs, on the rig's devices
# (build it first, see ..\README.md). Administrator; close the rig's own software (the wheel maker's tool) and games
# first: each device restarts once. Secure Boot and test mode stay as they are: the package is signed with a
# certificate made on this PC by the WDK build (WDKTestCert, or WheelBri's own from ..\new-signing-cert.ps1), which this
# script adds to the machine's trusted Root and TrustedPublisher stores (thumbprint recorded, see ..\common.ps1).
#
# Which devices: only the ones given. The app gives the exact interfaces: the HID instance ids of the active rig's
# game controllers as DirectInput lists them. By hand, USB ids work too (their game-controller interfaces, by
# compatible ids). Nothing in this script or the INF names a particular wheel: the INF's only hardware id,
# WHEELBRI_HIDE_FILTER, belongs to no device, so Windows never picks it by itself; this script takes it from the
# INF's models for the device's class (SPDIT_CLASSDRIVER) and puts it on each interface given, the way Device
# Manager's "Have disk" does without "Show compatible hardware" (DiInstallDevice). Without -Devices (the installer)
# it replaces any older copy, which takes it off every device; the app puts it back at the next Start.
#
#   install-hide.ps1                                     adds the package to Windows only (the installer: no rig yet)
#   install-hide.ps1 -Devices 346e:0004,30b7:1001        ...and puts the filter on those devices' game-controller
#                                                        interfaces (all of them: a device may have several)
#   install-hide.ps1 -Devices "HID\VID_346E&PID_0005&MI_02\8&8052F69&0&0000"   ...on exactly that interface
#   install-hide.ps1 -Devices 346e:0004 -Configuration Debug
#   install-hide.ps1 -Devices ... -LogFolder <folder>    its log in that folder (the WheelBri Helper, which runs as
#                                                        SYSTEM, gives the user's: %LOCALAPPDATA% would be SYSTEM's)
#   install-hide.ps1 -Devices ... -NoRestart             the app: does not restart the devices itself (exit 2 when
#                                                        one waits for it); the app restarts them at once, which
#                                                        works where pnputil /restart-device refuses
#
# Exit codes (wheelbri-service runs this at Start when a rig device has no filter, and logs the code):
#   0 done: the filter runs on every given device that is plugged in (unplugged ones are listed; they get it at a
#     later Start)
#   1 failed (the red line says why)
#   2 put on, but not running on every device yet: restart Windows (when Windows is not waiting for a restart,
#     unplugging the device and plugging it in again is enough), then press Start again

param(
    [string]$Configuration = 'Release',
    [string[]]$Devices = @(),
    [string]$LogFolder = '',
    [switch]$NoRestart
)
# A 32-bit PowerShell (one started from a 32-bit prompt, e.g. some Developer PowerShell windows) sees SysWOW64,
# where pnputil and the 64-bit driver tools are missing: run this again in the 64-bit PowerShell.
if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
    $forward = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath, '-Configuration', $Configuration)
    if ($Devices.Count -gt 0) { $forward += @('-Devices', ($Devices -join ',')) }
    if ($LogFolder) { $forward += @('-LogFolder', $LogFolder) }
    if ($NoRestart) { $forward += '-NoRestart' }
    & (Join-Path $env:SystemRoot 'Sysnative\WindowsPowerShell\v1.0\powershell.exe') @forward
    exit $LASTEXITCODE
}
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common.ps1')

# Every line this script writes carries the time, as the other logs ("[2026-10-06 21:55:07.123] text"; the app's Logs
# tab reads it that way): on 2026-10-06 it took 36 s and its log could not say where.
function Write-Host {
    param([Parameter(Position = 0)] $Object = '', [ConsoleColor]$ForegroundColor = [ConsoleColor]::Gray)
    $text = "$Object"
    if ($text) { $text = "[$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff')] $text" }
    Microsoft.PowerShell.Utility\Write-Host $text -ForegroundColor $ForegroundColor
}
Assert-Admin

# Everything below also goes to %LOCALAPPDATA%\wheelbri\logs\wheelbri-hide-install-<date>.log (the app runs this
# without a window, so that file is where to look when Start says a device is not hidden).
# Every log in one folder, the user's %LOCALAPPDATA%\wheelbri\logs (the app and both drivers write there too): the
# drivers run as LocalService, so it may write there, and HKLM\SOFTWARE\wheelbri LogFolder tells them where (the
# app does the same at every start, see ServiceLogging.hpp shareLogFolder).
# All of it inside the try: a failure there still ends in the finally below (and the red line says why).
$logFolder = if ($LogFolder) { $LogFolder } else { Join-Path $env:LOCALAPPDATA 'wheelbri\logs' }
$exitCode = 1
$transcript = $false
$script:needsWindowsRestart = $false   # pnputil said a device waits for a restart of Windows
try {
    # Not through a link (junction) in the user's folders: as Administrator the grant below would change the
    # permissions of whatever it points to (the app refuses such folders too).
    $logsLinked = @((Split-Path $logFolder), $logFolder | Where-Object {
        $item = Get-Item -LiteralPath $_ -Force -ErrorAction SilentlyContinue
        $item -and (Test-Link $item)
    }).Count -gt 0
    if ($logsLinked) {
        Start-Transcript -Append -Path (Join-Path $env:SystemRoot "Temp\wheelbri-hide-install-$(Get-Date -Format yyyy-MM-dd).log") | Out-Null
        $transcript = $true
        Write-Host "$logFolder is a link to another folder: the drivers keep their logs in C:\ProgramData\wheelbri\logs" -ForegroundColor Yellow
    } else {
        New-Item -ItemType Directory -Force -Path $logFolder | Out-Null
        Start-Transcript -Append -Path (Join-Path $logFolder "wheelbri-hide-install-$(Get-Date -Format yyyy-MM-dd).log") | Out-Null
        $transcript = $true
        & icacls $logFolder /grant '*S-1-5-19:(OI)(CI)M' | Out-Null
        if (-not (Test-Path 'HKLM:\SOFTWARE\wheelbri')) { New-Item -Path 'HKLM:\SOFTWARE\wheelbri' | Out-Null }
        Set-ItemProperty -Path 'HKLM:\SOFTWARE\wheelbri' -Name 'LogFolder' -Value $logFolder
    }

    # "346e:0004,30b7:1001" (one argument from the app) or several arguments: the USB ids, checked.
    # The virtual wheels games are meant to see (046D and these products; kVirtualWheels in core/src/HidWheelDevice.cpp:
    # G25, G27, G920, G923) never get the filter.
    $virtualWheels = @('C299', 'C29B', 'C262', 'C26E')
    $wanted = @()
    foreach ($text in @($Devices | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })) {
        # The app gives the exact interfaces (HID instance ids of the game controllers DirectInput lists).
        if ($text -match '^HID\\VID_([0-9A-Fa-f]{4})&PID_([0-9A-Fa-f]{4})') {
            if ($Matches[1] -eq '046D' -and $Matches[2] -in $virtualWheels) {
                Write-Host "skipping ${text}: that is a virtual wheel games are meant to see" -ForegroundColor Yellow
                continue
            }
            $wanted += [pscustomobject]@{ Vendor = $Matches[1].ToUpper(); Product = $Matches[2].ToUpper(); Text = $text; Instance = $text }
            continue
        }
        if ($text -notmatch '^([0-9A-Fa-f]{4}):([0-9A-Fa-f]{4})$') {
            Stop-WithError "-Devices: '$text' is not a USB id like 346e:0004 or a HID instance id like HID\VID_346E&PID_0004&MI_02\..."
        }
        $id = [pscustomobject]@{ Vendor = $Matches[1].ToUpper(); Product = $Matches[2].ToUpper(); Text = $text.ToLower(); Instance = '' }
        if ($id.Vendor -eq '046D' -and $id.Product -in $virtualWheels) {
            Write-Host "skipping $($id.Text): that is a virtual wheel games are meant to see" -ForegroundColor Yellow
            continue
        }
        $wanted += $id
    }

    # The new package first: without a build, nothing is taken off the devices (the script stops here).
    $inf = Find-BuiltPackage $PSScriptRoot 'wheelbri-hide' $Configuration
    Add-PackageCertificate $inf 'wheelbri-hide'
    $removed = Remove-LegacyPackages 'wheelbri-hide'
    if ($wanted.Count -eq 0) {
        # The installer: a fresh copy, off every device an older copy is on (an older one could even have been put
        # on a virtual wheel by Windows itself). The app puts it back on the rig's devices at the next Start.
        $removed += Remove-InstalledPackages 'wheelbri-hide'
    }
    if ($removed -gt 0) {
        Invoke-Pnputil @('/scan-devices') 'rescanning devices' | Out-Null   # the devices back on their own driver first
    }
    # Added to Windows' driver store only (no /install): no device has its hardware id, so it never goes on by itself.
    $published = Add-Package $inf 'wheelbri-hide' $false 'adding wheelbri-hide to Windows'
    Write-Host ''
    if ($wanted.Count -eq 0) {
        Write-Host 'Package added. The app puts the filter on the rig''s devices (Wheel setup) when you press Start.' -ForegroundColor Green
        $exitCode = 0
    } else {

        # A device's game-controller interfaces: its HID collections whose compatible IDs say joystick, gamepad or
        # multi-axis controller (usage page 1, usages 4, 5, 8; many wheels are multi-axis and have no
        # HID_DEVICE_SYSTEM_GAME) or simulation control (page 2): what DirectInput and games open. A vendor tool keeps
        # its other interfaces. Same rule as isGameController in core/src/HidHideCli.cpp. Every list is taken with
        # @(...): Windows PowerShell 5.1 gives no .Count for a single CIM object.
        function Test-GameController($compatible) {
            foreach ($c in @($compatible)) {
                if ($c -in @('HID_DEVICE_SYSTEM_GAME', 'HID_DEVICE_UP:0001_U:0004', 'HID_DEVICE_UP:0001_U:0005',
                             'HID_DEVICE_UP:0001_U:0008') -or $c -like 'HID_DEVICE_UP:0002_*') { return $true }
            }
            return $false
        }
        function Get-Interfaces($id) {
            if ($id.Instance) { return @(Get-PnpDevice -InstanceId $id.Instance -PresentOnly -ErrorAction SilentlyContinue) }
            @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
                Where-Object { $_.InstanceId -like "HID\VID_$($id.Vendor)&PID_$($id.Product)*" } |
                Where-Object {
                    $compatible = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName 'DEVPKEY_Device_CompatibleIds' -ErrorAction SilentlyContinue).Data
                    Test-GameController $compatible
                })
        }
        # Ours: the device's driver is one of our installed copies (not just any user-mode filter, which also shows
        # as WUDFRd), and the reflector is in its upper filters.
        $ours = @(@(Get-InstalledPackages 'wheelbri-hide') | ForEach-Object { $_.ToLower() })
        function Test-Filter($device) {
            $filters = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_UpperFilters' -ErrorAction SilentlyContinue).Data
            $driverInf = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_DriverInfPath' -ErrorAction SilentlyContinue).Data
            return (@($filters) -contains 'WUDFRd') -and $driverInf -and ($ours -contains $driverInf.ToLower())
        }
        # Listed is not running: after a driver switch Windows may keep the old stack until the interface restarts
        # (something still has it open), and says so with DN_NEED_RESTART (0x100) in the device's status. Until then
        # the filter is not loaded and the device is not hidden, even though UpperFilters names it.
        function Test-Pending($device) {
            $status = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_DevNodeStatus' -ErrorAction SilentlyContinue).Data
            $problem = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_ProblemCode' -ErrorAction SilentlyContinue).Data
            return (($status -band 0x100) -ne 0) -or ($problem -eq 14)   # 14: CM_PROB_NEED_RESTART
        }
        function Get-All { @($wanted | ForEach-Object { Get-Interfaces $_ }) }
        function Get-Missing { @(Get-All | Where-Object { -not (Test-Filter $_) }) }
        function Get-Pending { @(Get-All | Where-Object { (Test-Filter $_) -and (Test-Pending $_) }) }

        # The filter on one interface, chosen from our INF only, whatever ranks higher (input.inf, a vendor's driver).
        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace Wheelbri {
public static class HideInstall {
    const uint DI_ENUMSINGLEINF = 0x00010000;
    const uint DI_FLAGSEX_ALLOWEXCLUDEDDRVS = 0x00000800;
    const uint SPDIT_CLASSDRIVER = 1;   // every model of the INF for the device's class, whatever ids it matches

    [StructLayout(LayoutKind.Sequential)]
    struct SP_DEVINFO_DATA { public uint cbSize; public Guid ClassGuid; public uint DevInst; public IntPtr Reserved; }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct SP_DEVINSTALL_PARAMS {
        public uint cbSize; public uint Flags; public uint FlagsEx; public IntPtr hwndParent;
        public IntPtr InstallMsgHandler; public IntPtr InstallMsgHandlerContext; public IntPtr FileQueue;
        public UIntPtr ClassInstallReserved; public uint Reserved;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string DriverPath;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct SP_DRVINFO_DATA {
        public uint cbSize; public uint DriverType; public UIntPtr Reserved;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string Description;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string MfgName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string ProviderName;
        public long DriverDate; public ulong DriverVersion;
    }

    [DllImport("setupapi.dll", SetLastError = true)]
    static extern IntPtr SetupDiCreateDeviceInfoList(IntPtr classGuid, IntPtr hwnd);
    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool SetupDiOpenDeviceInfoW(IntPtr set, string instanceId, IntPtr hwnd, uint flags, ref SP_DEVINFO_DATA data);
    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool SetupDiGetDeviceInstallParamsW(IntPtr set, ref SP_DEVINFO_DATA data, ref SP_DEVINSTALL_PARAMS p);
    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool SetupDiSetDeviceInstallParamsW(IntPtr set, ref SP_DEVINFO_DATA data, ref SP_DEVINSTALL_PARAMS p);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiBuildDriverInfoList(IntPtr set, ref SP_DEVINFO_DATA data, uint type);
    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool SetupDiEnumDriverInfoW(IntPtr set, ref SP_DEVINFO_DATA data, uint type, uint index, ref SP_DRVINFO_DATA info);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiDestroyDriverInfoList(IntPtr set, ref SP_DEVINFO_DATA data, uint type);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
    [DllImport("newdev.dll", SetLastError = true)]
    static extern bool DiInstallDevice(IntPtr hwnd, IntPtr set, ref SP_DEVINFO_DATA data, ref SP_DRVINFO_DATA info, uint flags, out bool reboot);

    static string Failed(string what) {
        int code = Marshal.GetLastWin32Error();
        return what + " failed: " + new Win32Exception(code).Message + " (0x" + code.ToString("X8") + ")";
    }

    // Puts the driver of `infPath` on the device `instanceId`. Empty on success, else why not.
    public static string Install(string instanceId, string infPath, out bool reboot) {
        reboot = false;
        IntPtr set = SetupDiCreateDeviceInfoList(IntPtr.Zero, IntPtr.Zero);
        if (set == new IntPtr(-1)) return Failed("SetupDiCreateDeviceInfoList");
        try {
            SP_DEVINFO_DATA device = new SP_DEVINFO_DATA();
            device.cbSize = (uint)Marshal.SizeOf(typeof(SP_DEVINFO_DATA));
            if (!SetupDiOpenDeviceInfoW(set, instanceId, IntPtr.Zero, 0, ref device)) return Failed("SetupDiOpenDeviceInfo");
            SP_DEVINSTALL_PARAMS parameters = new SP_DEVINSTALL_PARAMS();
            parameters.cbSize = (uint)Marshal.SizeOf(typeof(SP_DEVINSTALL_PARAMS));
            if (!SetupDiGetDeviceInstallParamsW(set, ref device, ref parameters)) return Failed("SetupDiGetDeviceInstallParams");
            parameters.DriverPath = infPath;   // only this INF
            parameters.Flags |= DI_ENUMSINGLEINF;
            parameters.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
            if (!SetupDiSetDeviceInstallParamsW(set, ref device, ref parameters)) return Failed("SetupDiSetDeviceInstallParams");
            if (!SetupDiBuildDriverInfoList(set, ref device, SPDIT_CLASSDRIVER)) return Failed("SetupDiBuildDriverInfoList");
            try {
                SP_DRVINFO_DATA driver = new SP_DRVINFO_DATA();
                driver.cbSize = (uint)Marshal.SizeOf(typeof(SP_DRVINFO_DATA));
                if (!SetupDiEnumDriverInfoW(set, ref device, SPDIT_CLASSDRIVER, 0, ref driver)) {
                    return "the package does not fit this device (not a HID device?): " + Failed("SetupDiEnumDriverInfo");
                }
                if (!DiInstallDevice(IntPtr.Zero, set, ref device, ref driver, 0, out reboot)) return Failed("DiInstallDevice");
                return "";
            } finally {
                SetupDiDestroyDriverInfoList(set, ref device, SPDIT_CLASSDRIVER);
            }
        } finally {
            SetupDiDestroyDeviceInfoList(set);
        }
    }
}
}
'@

        $notConnected = @()
        foreach ($id in $wanted) {
            $interfaces = @(Get-Interfaces $id)
            if ($interfaces.Count -eq 0) {
                Write-Host "$($id.Text): not plugged in (or no game-controller interface): it gets the filter at a later Start" -ForegroundColor Yellow
                $notConnected += $id.Text
                continue
            }
            foreach ($device in $interfaces) {
                if (Test-Filter $device) {
                    Write-Host "$($id.Text): $($device.InstanceId) already has the filter"
                    continue
                }
                $current = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_DriverInfPath' -ErrorAction SilentlyContinue).Data
                $description = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_DriverDesc' -ErrorAction SilentlyContinue).Data
                Write-Host "$($id.Text): $($device.InstanceId) ($($device.FriendlyName)) uses $current ($description) now"
                Write-Host "    putting wheelbri-hide on it..."
                $reboot = $false
                $problem = [Wheelbri.HideInstall]::Install($device.InstanceId, $inf.FullName, [ref]$reboot)
                if ($problem) {
                    Stop-WithError "could not put the filter on $($device.InstanceId): $problem"
                }
                if ($reboot) { Write-Host '    Windows wants the device restarted before the filter runs: restarting it below.' }
            }
        }
        # The interfaces restart with the filter: wait for them (up to 15 s) rather than a fixed pause.
        for ($waited = 0; $waited -lt 15 -and @(Get-Missing).Count -gt 0; $waited++) { Start-Sleep -Seconds 1 }

        # Installed but not loaded: restart the interface ourselves (what unplugging it does), instead of asking for a
        # restart of Windows. It fails while a program holds the device in a way Windows cannot interrupt.
        # -NoRestart (the app): left to the caller, which restarts it at once with CM_Query_And_Remove_SubTree +
        # CM_Setup_DevNode; pnputil refused (exit 50) where that worked in 0.25 s, and waiting 15 s for it cost the
        # first Start ~20 s (2026-10-06).
        if ($NoRestart -and @(Get-Pending).Count -gt 0) {
            Write-Host 'the filter is on and waits for a restart of the device: the app restarts it now'
        }
        foreach ($device in @(if ($NoRestart) { @() } else { Get-Pending })) {
            Write-Host "restarting $($device.InstanceId) so Windows loads the filter..."
            $said = @(& pnputil /restart-device "$($device.InstanceId)")
            $code = $LASTEXITCODE
            if ($code -eq 0) {
                Write-Host '    restarted'
            } elseif ($code -in @(50, 3010)) {
                # 3010: done at the next restart. 50 (ERROR_NOT_SUPPORTED): pnputil then prints "This command is not
                # supported on this OS product", which is misleading: Windows refuses because the device is waiting for a
                # restart of Windows ("pending system reboot"), not because the command is missing. Either way only a
                # restart of Windows helps here; unplugging the device does not.
                $script:needsWindowsRestart = $true
                Write-Host '    Windows will not restart the device now: it is waiting for a restart of Windows to finish'
                Write-Host "    switching its driver (pnputil exit code $code; unplugging the device does not help)."
            } else {
                $said | ForEach-Object { Write-Host "    pnputil: $_" }
                Write-Host "    pnputil exit code $code"
            }
        }
        if (-not $NoRestart) {
            for ($waited = 0; $waited -lt 15 -and @(Get-Pending).Count -gt 0; $waited++) { Start-Sleep -Seconds 1 }
        }

        # What each device's game-controller interfaces run with now.
        foreach ($device in @(Get-All)) {
            $filters = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_UpperFilters' -ErrorAction SilentlyContinue).Data
            $driver = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_DriverInfPath' -ErrorAction SilentlyContinue).Data
            $pending = if (Test-Pending $device) { ', waiting for a restart (filter not running yet)' } else { '' }
            Write-Host "$($device.InstanceId): status $($device.Status), driver $driver, upper filters: $($filters -join ', ')$pending"
        }
        Write-Host ''
        $left = @(Get-Missing) + @(Get-Pending)
        if ($left.Count -gt 0 -and $NoRestart) {
            Write-Host 'The filter is on; the app restarts the devices that wait for it.'
            $exitCode = 2
        } elseif ($left.Count -gt 0 -and $script:needsWindowsRestart) {
            Write-Host ('The filter is on, but Windows starts it only after a restart: restart Windows; the next Start in ' +
                        'the app then hides the rig.') -ForegroundColor Red
            $exitCode = 2
        } elseif ($left.Count -gt 0) {
            Write-Host ('The filter is on, but not running on every device yet: unplug those devices and plug them in ' +
                        'again (or restart Windows); the next Start in the app then hides the rig.') -ForegroundColor Red
            $exitCode = 2
        } else {
            $done = @($wanted | Where-Object { $notConnected -notcontains $_.Text } | ForEach-Object { $_.Text })
            Write-Host "Done: the filter runs on $($done -join ', ')$(if ($notConnected.Count) { "; not plugged in: $($notConnected -join ', ')" })." -ForegroundColor Green
            $exitCode = 0
        }
    }
    # The earlier builds' copies: one a device still runs is refused and stays (the next install takes it).
    Remove-OlderCopies 'wheelbri-hide' $published | Out-Null
} catch {
    Write-Host "install-hide.ps1 failed: $($_.Exception.Message)" -ForegroundColor Red
} finally {
    if ($transcript) { Stop-Transcript | Out-Null }
}
exit $exitCode
