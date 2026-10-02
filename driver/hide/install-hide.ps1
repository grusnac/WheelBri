# Installs wheelemu-hide, the filter that hides the R3 from games while wheelemu-service runs (build it first,
# see ..\README.md). Administrator; close Moza Pit House and games first (the R3 restarts once). Secure Boot and test
# mode stay as they are: the package is signed with a certificate made on this PC by the WDK build
# (WDKTestCert), which this script adds to the machine's trusted Root and TrustedPublisher stores.
#
#   install-hide.ps1                   Release build
#   install-hide.ps1 -Configuration Debug
#
# Exit codes (wheelemu-service runs this when Start finds the filter missing, and logs the code):
#   0 installed and on the R3 (or the R3 is unplugged: it attaches when the R3 is plugged in)
#   1 failed (the red line says why)
#   2 installed, but not on the R3 yet: unplug and replug the R3 (or restart Windows), then run it again

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

# Everything below also goes to C:\ProgramData\wheelemu\logs\wheelemu-hide-install-<date>.log (the app runs this
# without a window, so that file is where to look when Start says the R3 is not hidden).
$logFolder = 'C:\ProgramData\wheelemu\logs'
New-Item -ItemType Directory -Force -Path $logFolder | Out-Null
Start-Transcript -Append -Path (Join-Path $logFolder "wheelemu-hide-install-$(Get-Date -Format yyyy-MM-dd).log") | Out-Null
$exitCode = 1
try {
    $inf = Find-BuiltPackage $PSScriptRoot 'wheelemu-hide' $Configuration
    Add-PackageCertificate $inf 'wheelemu-hide'
    Invoke-Pnputil @('/add-driver', $inf.FullName, '/install') 'installing wheelemu-hide' | Out-Null
    Write-Host ''

    # pnputil switches a device only to the best-ranked driver, and Windows ranks its own signed input.inf above
    # this test-signed package: the R3 keeps input.inf without the filter. So put ours on the R3's game-controller
    # interface explicitly, as Device Manager's "Have disk" does (UpdateDriverForPlugAndPlayDevices, INSTALLFLAG_FORCE).
    $hardwareId = 'HID\VID_346E&PID_0005&MI_02'
    function Get-R3 { @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like "$hardwareId*" }) }
    function Test-Filter($device) {
        $filters = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_UpperFilters' -ErrorAction SilentlyContinue).Data
        return @($filters) -contains 'WUDFRd'
    }
    function Get-Missing { @(Get-R3 | Where-Object { -not (Test-Filter $_) }) }

    $r3 = Get-R3
    if ($r3.Count -gt 0 -and (Get-Missing).Count -gt 0) {
        Add-Type -Namespace Wheelemu -Name NewDev -MemberDefinition @'
[DllImport("newdev.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern bool UpdateDriverForPlugAndPlayDevices(IntPtr parent, string hardwareId, string infPath,
                                                            uint flags, out bool rebootRequired);
'@
        $reboot = $false
        Write-Host "putting wheelemu-hide on $hardwareId (Windows' own driver ranks higher, so it is forced)..."
        if (-not [Wheelemu.NewDev]::UpdateDriverForPlugAndPlayDevices([IntPtr]::Zero, $hardwareId, $inf.FullName, 1, [ref]$reboot)) {
            $code = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
            Stop-WithError "could not put the filter on the R3: $((New-Object ComponentModel.Win32Exception $code).Message) ($code)"
        }
        if ($reboot) { Write-Host 'Windows asks for a restart before the filter is active.' -ForegroundColor Yellow }
        # The R3's interface restarts with the filter: wait for it (up to 15 s) rather than a fixed pause.
        for ($waited = 0; $waited -lt 15 -and (Get-Missing).Count -gt 0; $waited++) { Start-Sleep -Seconds 1 }
    }

    # Check what the R3's game-controller interface runs with now.
    $r3 = Get-R3
    foreach ($device in $r3) {
        $filters = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_UpperFilters' -ErrorAction SilentlyContinue).Data
        $driver = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_DriverInfPath' -ErrorAction SilentlyContinue).Data
        Write-Host "$($device.InstanceId): status $($device.Status), driver $driver, upper filters: $($filters -join ', ')"
    }
    Write-Host ''
    if ($r3.Count -eq 0) {
        Write-Host 'The R3 is not plugged in: the filter attaches when it is.' -ForegroundColor Yellow
        $exitCode = 0
    } elseif ((Get-Missing).Count -gt 0) {
        Write-Host ('The filter is installed but not on the R3 yet: unplug the R3, plug it in again (or restart ' +
                    'Windows), and run this once more.') -ForegroundColor Red
        $exitCode = 2
    } else {
        Write-Host 'Installed. wheelemu-service --virtual-g920 now hides the R3 with it.' -ForegroundColor Green
        $exitCode = 0
    }
} finally {
    Stop-Transcript | Out-Null
}
exit $exitCode
