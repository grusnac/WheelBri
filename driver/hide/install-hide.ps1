# Installs wheelbri-hide, the filter that hides the R3 from games while wheelbri-service runs (build it first,
# see ..\README.md). Administrator; close Moza Pit House and games first (the R3 restarts once). Secure Boot and test
# mode stay as they are: the package is signed with a certificate made on this PC by the WDK build
# (WDKTestCert), which this script adds to the machine's trusted Root and TrustedPublisher stores.
#
#   install-hide.ps1                   Release build
#   install-hide.ps1 -Configuration Debug
#
# Exit codes (wheelbri-service runs this when Start finds the filter missing, and logs the code):
#   0 installed and on the R3 (or the R3 is unplugged: it attaches when the R3 is plugged in)
#   1 failed (the red line says why)
#   2 installed, but not running on the R3 yet: unplug and replug the R3 (or restart Windows), then run it again

param([string]$Configuration = 'Release')
# A 32-bit PowerShell (one started from a 32-bit prompt, e.g. some Developer PowerShell windows) sees SysWOW64,
# where pnputil and the 64-bit driver tools are missing: run this again in the 64-bit PowerShell.
if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
    & (Join-Path $env:SystemRoot 'Sysnative\WindowsPowerShell\v1.0\powershell.exe') -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -Configuration $Configuration
    exit $LASTEXITCODE
}
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common.ps1')
Assert-Admin

# Everything below also goes to C:\ProgramData\wheelbri\logs\wheelbri-hide-install-<date>.log (the app runs this
# without a window, so that file is where to look when Start says the R3 is not hidden).
$logFolder = 'C:\ProgramData\wheelbri\logs'
New-Item -ItemType Directory -Force -Path $logFolder | Out-Null
Start-Transcript -Append -Path (Join-Path $logFolder "wheelbri-hide-install-$(Get-Date -Format yyyy-MM-dd).log") | Out-Null
$exitCode = 1
$script:needsWindowsRestart = $false   # pnputil said the R3 waits for a restart of Windows
try {
    if ((Remove-LegacyPackages 'wheelbri-hide') -gt 0) {
        Invoke-Pnputil @('/scan-devices') 'rescanning devices' | Out-Null   # the R3 back on its own driver first
    }
    $inf = Find-BuiltPackage $PSScriptRoot 'wheelbri-hide' $Configuration
    Add-PackageCertificate $inf 'wheelbri-hide'
    Invoke-Pnputil @('/add-driver', $inf.FullName, '/install') 'installing wheelbri-hide' | Out-Null
    Write-Host ''

    # pnputil switches a device only to the best-ranked driver, and Windows ranks its own signed input.inf, and Moza's
    # signed driver ("MOZA Windows Driver", which Moza's software can put on the R3), above this test-signed package:
    # the R3 keeps that driver without our filter. So put ours on the R3's game-controller interface explicitly, as Device
    # Manager's "Have disk" does (UpdateDriverForPlugAndPlayDevices, INSTALLFLAG_FORCE).
    # Every list below is taken with @(...) at the call: a function returning one device hands back the device itself,
    # and Windows PowerShell 5.1 gives no .Count for a single CIM object, so a missing filter looked like "none
    # missing" and was never forced (the R3 stayed visible to games).
    $hardwareId = 'HID\VID_346E&PID_0005&MI_02'
    function Get-R3 { @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like "$hardwareId*" }) }
    function Test-Filter($device) {
        $filters = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_UpperFilters' -ErrorAction SilentlyContinue).Data
        return @($filters) -contains 'WUDFRd'
    }
    function Get-Missing { @(Get-R3 | Where-Object { -not (Test-Filter $_) }) }
    # Listed is not running: after a driver switch Windows may keep the old stack until the interface restarts
    # (something still has it open), and says so with DN_NEED_RESTART (0x100) in the device's status. Until then
    # the filter is not loaded and the R3 is not hidden, even though UpperFilters names it.
    function Test-Pending($device) {
        $status = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_DevNodeStatus' -ErrorAction SilentlyContinue).Data
        $problem = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_ProblemCode' -ErrorAction SilentlyContinue).Data
        return (($status -band 0x100) -ne 0) -or ($problem -eq 14)   # 14: CM_PROB_NEED_RESTART
    }
    function Get-Pending { @(Get-R3 | Where-Object { (Test-Filter $_) -and (Test-Pending $_) }) }

    $r3 = @(Get-R3)
    if ($r3.Count -gt 0 -and @(Get-Missing).Count -gt 0) {
        foreach ($device in $r3) {
            $current = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_DriverInfPath' -ErrorAction SilentlyContinue).Data
            $description = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_DriverDesc' -ErrorAction SilentlyContinue).Data
            Write-Host "the R3's game-controller interface uses $current ($description) now"
        }
        Add-Type -Namespace Wheelbri -Name NewDev -MemberDefinition @'
[DllImport("newdev.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern bool UpdateDriverForPlugAndPlayDevices(IntPtr parent, string hardwareId, string infPath,
                                                            uint flags, out bool rebootRequired);
'@
        $reboot = $false
        Write-Host "putting wheelbri-hide on $hardwareId (its current driver ranks higher, so ours is forced)..."
        if (-not [Wheelbri.NewDev]::UpdateDriverForPlugAndPlayDevices([IntPtr]::Zero, $hardwareId, $inf.FullName, 1, [ref]$reboot)) {
            $code = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
            Stop-WithError "could not put the filter on the R3: $((New-Object ComponentModel.Win32Exception $code).Message) ($code)"
        }
        if ($reboot) { Write-Host 'Windows wants the R3 restarted before the filter runs: restarting it below.' }
        # The R3's interface restarts with the filter: wait for it (up to 15 s) rather than a fixed pause.
        for ($waited = 0; $waited -lt 15 -and @(Get-Missing).Count -gt 0; $waited++) { Start-Sleep -Seconds 1 }
    }

    # Installed but not loaded: restart the R3's interface ourselves (what unplugging it does), instead of asking
    # for a restart of Windows. It fails while a program holds the R3 in a way Windows cannot interrupt.
    foreach ($device in @(Get-Pending)) {
        Write-Host "restarting $($device.InstanceId) so Windows loads the filter..."
        $said = @(& pnputil /restart-device "$($device.InstanceId)")
        $code = $LASTEXITCODE
        if ($code -eq 0) {
            Write-Host '    restarted'
        } elseif ($code -in @(50, 3010)) {
            # 3010: done at the next restart. 50 (ERROR_NOT_SUPPORTED): pnputil then prints "This command is not
            # supported on this OS product", which is misleading: Windows refuses because the R3 is waiting for a
            # restart of Windows ("pending system reboot"), not because the command is missing. Either way only a
            # restart of Windows helps here; unplugging the R3 does not.
            $script:needsWindowsRestart = $true
            Write-Host '    Windows will not restart the R3 now: it is waiting for a restart of Windows to finish switching'
            Write-Host "    its driver (pnputil exit code $code; unplugging the R3 does not help)."
        } else {
            $said | ForEach-Object { Write-Host "    pnputil: $_" }
            Write-Host "    pnputil exit code $code"
        }
    }
    for ($waited = 0; $waited -lt 15 -and @(Get-Pending).Count -gt 0; $waited++) { Start-Sleep -Seconds 1 }

    # Check what the R3's game-controller interface runs with now.
    $r3 = @(Get-R3)
    foreach ($device in $r3) {
        $filters = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_UpperFilters' -ErrorAction SilentlyContinue).Data
        $driver = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_DriverInfPath' -ErrorAction SilentlyContinue).Data
        $pending = if (Test-Pending $device) { ', waiting for a restart (filter not running yet)' } else { '' }
        Write-Host "$($device.InstanceId): status $($device.Status), driver $driver, upper filters: $($filters -join ', ')$pending"
    }
    Write-Host ''
    if ($r3.Count -eq 0) {
        Write-Host 'The R3 is not plugged in: the filter attaches when it is.' -ForegroundColor Yellow
        $exitCode = 0
    } elseif ($script:needsWindowsRestart -and (@(Get-Missing).Count -gt 0 -or @(Get-Pending).Count -gt 0)) {
        Write-Host ('The filter is installed but Windows starts it on the R3 only after a restart: restart Windows; ' +
                    'the next Start in the app then hides the R3.') -ForegroundColor Red
        $exitCode = 2
    } elseif (@(Get-Missing).Count -gt 0 -or @(Get-Pending).Count -gt 0) {
        Write-Host ('The filter is installed but not running on the R3 yet: unplug the R3 and plug it in again (or ' +
                    'restart Windows); the next Start in the app then hides it.') -ForegroundColor Red
        $exitCode = 2
    } else {
        Write-Host 'Installed. wheelbri-service --virtual-g920 now hides the R3 with it.' -ForegroundColor Green
        $exitCode = 0
    }
} finally {
    Stop-Transcript | Out-Null
}
exit $exitCode
