# Dumps DirectInput's registry entries for the G920 ids (046D:C262, force feedback refused) and the test ids
# (1209:0001, force feedback works), side by side. Read-only. Output: %LOCALAPPDATA%\wheelemu\logs\di-keys-<time>.txt
$out = Join-Path $env:LOCALAPPDATA ("wheelemu\logs\di-keys-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + ".txt")
$base = 'System\CurrentControlSet\Control\MediaProperties\PrivateProperties'
& {
    foreach ($id in 'VID_046D&PID_C262', 'VID_1209&PID_0001') {
        foreach ($hive in 'HKLM', 'HKCU') {
            foreach ($kind in 'DirectInput', 'Joystick\OEM') {
                "`n===== $hive\$base\$kind\$id ====="
                reg query "$hive\$base\$kind\$id" /s 2>&1
            }
        }
        "`n===== HKLM\SYSTEM\CurrentControlSet\Control\DeviceOverrides\root#$id ====="
        reg query "HKLM\SYSTEM\CurrentControlSet\Control\DeviceOverrides\root#$id" /s 2>&1
    }
    "`n===== GameInput devices ====="
    reg query 'HKLM\SYSTEM\CurrentControlSet\Control\GameInput\Devices' /s 2>&1 | Select-String -Pattern '046DC262|12090001' -Context 0,12
} *>&1 | Out-File -FilePath $out -Encoding utf8 -Width 400
"written: $out"
