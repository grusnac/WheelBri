#!/usr/bin/env bash
# Downloads, from Microsoft, everything WheelBri needs on a Windows 7 SP1 (64-bit) PC before its own setup can run,
# checks that each file is signed by Microsoft, and puts them with installer/win7/install-prerequisites.cmd into
# dist/windows/win7-prerequisites/. Copy that folder to the Windows 7 PC and run install-prerequisites.cmd there as
# Administrator, then restart and run WheelBri's Setup.exe. (Downloaded here, not on Windows 7: it often cannot reach
# Microsoft's download servers any more.)
#   - Windows6.1-KB2999226-x64.msu              the Universal C Runtime (the app and its shim are built on it)
#   - NDP452-KB2901907-x86-x64-AllOS-ENU.exe    .NET Framework 4.5.2 (Windows Management Framework 5.1 needs it)
#   - Win7AndW2K8R2-KB3191566-x64.msu           Windows Management Framework 5.1, from Microsoft's zip: PowerShell 5.1
#
#   scripts/make/win7-prerequisites.sh            download what is not there yet (or not validly signed)
#   scripts/make/win7-prerequisites.sh --force    download everything again

source "$(dirname "$0")/common.sh"

on_windows || fail "run it from the MSYS2 UCRT64 shell on Windows (it checks the signatures with PowerShell)"
need curl "pacman -S curl"

force=0
case "${1:-}" in
    --force) force=1 ;;
    "") ;;
    *) fail "unknown option $1 (--force)" ;;
esac

out="$root/dist/windows/win7-prerequisites"
mkdir -p "$out" || fail "cannot create $out"

# Microsoft's own links (their download pages: KB2999226 id 49093, .NET 4.5.2 id 42642, WMF 5.1 id 54616).
ucrt_url='https://download.microsoft.com/download/1/1/5/11565a9a-ea09-4f0a-a57e-520d5d138140/'
ucrt_url+='Windows6.1-KB2999226-x64.msu'
net_url='https://download.microsoft.com/download/e/2/1/e21644b5-2df2-47c2-91bd-63c560427900/'
net_url+='NDP452-KB2901907-x86-x64-AllOS-ENU.exe'
wmf_url='https://go.microsoft.com/fwlink/?linkid=839523'   # Win7AndW2K8R2-KB3191566-x64.zip

# Whether `file` carries a valid Authenticode signature from Microsoft (PowerShell's Get-AuthenticodeSignature).
signed_by_microsoft() {
    local file
    file=$(cygpath -w "$1")
    WE_FILE="$file" powershell.exe -NoProfile -Command '
        $s = Get-AuthenticodeSignature -LiteralPath $env:WE_FILE
        if ($s.Status -eq "Valid" -and $s.SignerCertificate.Subject -match "O=Microsoft Corporation") { exit 0 }
        Write-Host "    signature: $($s.Status) $($s.SignerCertificate.Subject)"
        exit 1'
}

# Downloads `url` to `file` (through a temporary name, so a broken download never looks finished) unless a validly
# signed copy is there already.
fetch() {
    local url=$1 file=$2
    if [ $force -eq 0 ] && [ -f "$file" ] && signed_by_microsoft "$file" >/dev/null 2>&1; then
        ok "already here: $(basename "$file")"
        return 0
    fi
    say "downloading $(basename "$file")..."
    curl -fL --retry 3 --progress-bar -o "$file.part" "$url" || { rm -f "$file.part"; fail "download failed: $url"; }
    mv -f "$file.part" "$file" || fail "cannot write $file"
}

check() {
    if ! signed_by_microsoft "$1"; then
        rm -f "$1"
        fail "$(basename "$1") is not validly signed by Microsoft: deleted (run this again, or --force)"
    fi
    ok "signed by Microsoft: $(basename "$1")"
}

fetch "$ucrt_url" "$out/Windows6.1-KB2999226-x64.msu"
check "$out/Windows6.1-KB2999226-x64.msu"
fetch "$net_url" "$out/NDP452-KB2901907-x86-x64-AllOS-ENU.exe"
check "$out/NDP452-KB2901907-x86-x64-AllOS-ENU.exe"

# WMF 5.1 comes as a zip (the .msu and Microsoft's Install-WMF5.1.ps1): only the .msu is kept; the .cmd installs it
# with wusa, as Install-WMF5.1.ps1 does.
wmf="$out/Win7AndW2K8R2-KB3191566-x64.msu"
if [ $force -eq 1 ] || [ ! -f "$wmf" ] || ! signed_by_microsoft "$wmf" >/dev/null 2>&1; then
    zip="$out/Win7AndW2K8R2-KB3191566-x64.zip"
    rm -f "$zip"
    fetch "$wmf_url" "$zip"
    unpacked="$out/wmf-unpacked"
    rm -rf "$unpacked"
    WE_ZIP=$(cygpath -w "$zip") WE_TO=$(cygpath -w "$unpacked") powershell.exe -NoProfile -Command \
        'Expand-Archive -LiteralPath $env:WE_ZIP -DestinationPath $env:WE_TO -Force' || fail "cannot unpack $zip"
    found=$(find "$unpacked" -iname '*KB3191566*x64*.msu' | head -1)
    [ -n "$found" ] || fail "no KB3191566 x64 .msu in $zip"
    mv -f "$found" "$wmf" || fail "cannot write $wmf"
    rm -rf "$unpacked" "$zip"
else
    ok "already here: $(basename "$wmf")"
fi
check "$wmf"

cp -f "$root/installer/win7/install-prerequisites.cmd" "$out/" || fail "cannot copy install-prerequisites.cmd"
ok "ready: $(cygpath -w "$out")"
say "Copy that folder to the Windows 7 PC, right-click install-prerequisites.cmd > Run as administrator, restart,"
say "then run WheelBri's Setup.exe."
