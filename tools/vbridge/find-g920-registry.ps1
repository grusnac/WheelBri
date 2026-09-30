# Lists every registry key and value that mentions the G920's USB ids (046D:C262), to find what makes
# DirectInput refuse force feedback for them. Read-only. Output: %LOCALAPPDATA%\wheelemu\logs\g920-registry-<time>.txt
$out = Join-Path $env:LOCALAPPDATA ("wheelemu\logs\g920-registry-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + ".txt")
$roots = 'HKLM\SYSTEM\CurrentControlSet\Control', 'HKCU\System\CurrentControlSet\Control', 'HKLM\SOFTWARE', 'HKCU\SOFTWARE', 'HKLM\SYSTEM\CurrentControlSet\Services', 'HKCR\CLSID'
& {
    "g920 registry search $(Get-Date -Format o)"
    foreach ($root in $roots) {
        "`n===== $root ====="
        reg query $root /s /f 'C262' 2>&1 | Select-Object -First 400
    }
    "`n===== PID force feedback driver class ====="
    reg query 'HKCR\CLSID\{EEC6993A-B3FD-11D2-A916-00C04FB98638}' /s 2>&1
} *>&1 | Out-File -FilePath $out -Encoding utf8 -Width 400
"written: $out"
