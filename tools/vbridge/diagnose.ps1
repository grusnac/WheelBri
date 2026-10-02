# Collects what decides whether the virtual G920 shows up in joy.cpl, into
# %LOCALAPPDATA%\wheelemu\logs\diagnose-<time>.txt. Run it while wheelemu-bridge is running.
# Read-only: it changes nothing.

$out = Join-Path $env:LOCALAPPDATA ("wheelemu\logs\diagnose-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + ".txt")
function Section($title) { "`n===== $title =====" }
function Prop($id, $key) { try { (Get-PnpDeviceProperty -InstanceId $id -KeyName $key -ErrorAction Stop).Data } catch { $null } }
function Describe($id, $depth) {
    $d = Get-PnpDevice -InstanceId $id -ErrorAction SilentlyContinue
    $pad = '  ' * $depth
    if (-not $d) { "$pad$id  (not found)"; return }
    "$pad$id"
    "$pad  name=$($d.FriendlyName)  status=$($d.Status)  class=$($d.Class)  problem=$($d.Problem)  present=$($d.Present)"
    "$pad  service=$(Prop $id 'DEVPKEY_Device_Service')  driver=$(Prop $id 'DEVPKEY_Device_DriverInfPath')"
    "$pad  hardwareIds=$((Prop $id 'DEVPKEY_Device_HardwareIds') -join ' | ')"
    "$pad  upperFilters=$((Prop $id 'DEVPKEY_Device_UpperFilters') -join ',')  lowerFilters=$((Prop $id 'DEVPKEY_Device_LowerFilters') -join ',')"
    "$pad  problemStatus=$(Prop $id 'DEVPKEY_Device_ProblemStatus')"
    foreach ($child in @(Prop $id 'DEVPKEY_Device_Children')) { if ($child) { Describe $child ($depth + 1) } }
}

& {
    "diagnose $(Get-Date -Format o)"
    Section 'processes'
    Get-Process wheelemu-bridge, wheelemu-service, HidHideClient -ErrorAction SilentlyContinue |
        Format-Table Id, ProcessName, StartTime -AutoSize | Out-String

    Section 'HIDMaestro devices and their children'
    $roots = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'ROOT\HIDCLASS\HM_*' -or $_.InstanceId -like '*HIDMAESTRO*' }
    if (-not $roots) { 'none' }
    foreach ($r in $roots) { Describe $r.InstanceId 0 }

    Section 'every device with VID 046D (present or not)'
    Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like '*VID_046D*' -or $_.InstanceId -like '*VID&0002046D*' } |
        Format-Table Status, Present, Problem, Class, FriendlyName, InstanceId -AutoSize | Out-String -Width 400

    Section 'game controllers Windows knows (HID with joystick/gamepad usage)'
    Get-PnpDevice -PresentOnly -Class HIDClass -ErrorAction SilentlyContinue | Where-Object { $_.FriendlyName -like '*game controller*' } |
        Format-Table Status, FriendlyName, InstanceId -AutoSize | Out-String -Width 400

    Section 'HIDClass class filters (HidHide installs one here)'
    Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{745a17a0-74d3-11d0-b6fe-00a0c90f57da}' -ErrorAction SilentlyContinue |
        Select-Object UpperFilters, LowerFilters | Format-List | Out-String

    Section 'HidHide'
    $cli = @("$env:ProgramFiles\Nefarius Software Solutions\HidHide\x64\HidHideCLI.exe",
             "$env:ProgramFiles\Nefarius Software Solutions\HidHide\HidHideCLI.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($cli) {
        "cli: $cli"
        & $cli --cloak-state
        'hidden devices:'; & $cli --dev-list
        'allowed apps:'; & $cli --app-list
    } else { 'HidHideCLI not found' }

    Section 'DirectInput OEM entries for VID_046D&PID_C262'
    foreach ($hive in 'HKCU:', 'HKLM:') {
        $key = "$hive\System\CurrentControlSet\Control\MediaProperties\PrivateProperties\Joystick\OEM\VID_046D&PID_C262"
        "--- $key"
        if (Test-Path $key) {
            Get-ItemProperty $key | Format-List | Out-String
            Get-ChildItem $key -Recurse -ErrorAction SilentlyContinue | ForEach-Object { $_.Name; Get-ItemProperty $_.PSPath | Format-List | Out-String }
        } else { 'missing' }
    }
    $di = 'HKCU:\System\CurrentControlSet\Control\MediaProperties\PrivateProperties\DirectInput'
    "--- $di (VID_046D entries)"
    Get-ChildItem $di -ErrorAction SilentlyContinue | Where-Object { $_.PSChildName -like '*046D*' } |
        ForEach-Object { $_.Name; Get-ChildItem $_.PSPath -Recurse -ErrorAction SilentlyContinue | ForEach-Object { '  ' + $_.Name } }

    Section 'Plug and Play events, last 60 minutes'
    try {
        Get-WinEvent -FilterHashtable @{ LogName = 'Microsoft-Windows-Kernel-PnP/Configuration'; StartTime = (Get-Date).AddMinutes(-60) } -ErrorAction Stop |
            Where-Object { $_.Message -match 'HM_|046D|HIDMaestro|HidHide' } | Select-Object -First 40 |
            ForEach-Object { "$($_.TimeCreated.ToString('HH:mm:ss')) id=$($_.Id) $($_.Message -replace '\s+', ' ')" }
    } catch { "no events: $($_.Exception.Message)" }
    try {
        Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = (Get-Date).AddMinutes(-60) } -ErrorAction Stop |
            Where-Object { $_.Message -match 'HM_|046D|HIDMaestro|HidHide|WUDF' } | Select-Object -First 40 |
            ForEach-Object { "$($_.TimeCreated.ToString('HH:mm:ss')) $($_.ProviderName) id=$($_.Id) $($_.Message -replace '\s+', ' ')" }
    } catch { "no system events: $($_.Exception.Message)" }
} *>&1 | Out-File -FilePath $out -Encoding utf8 -Width 400

"written: $out"
