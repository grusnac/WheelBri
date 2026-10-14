# Downloads, from Microsoft, everything WheelBri needs on a Windows 7 SP1 (64-bit) PC before its own setup can run,
# into the folder this script is in, and checks that each file is signed by Microsoft. Then install-prerequisites.cmd
# (same folder) installs them on the Windows 7 PC. Start it with download-prerequisites.cmd (double-click).
#   - Windows6.1-KB2999226-x64.msu              the Universal C Runtime (the app and its shim are built on it)
#   - NDP452-KB2901907-x86-x64-AllOS-ENU.exe    .NET Framework 4.5.2 (Windows Management Framework 5.1 needs it)
#   - Win7AndW2K8R2-KB3191566-x64.msu           Windows Management Framework 5.1, from Microsoft's zip: PowerShell 5.1
#
#   download-prerequisites.cmd            download what is not there yet (or not validly signed)
#   download-prerequisites.cmd -Force     download everything again
#
# Runs on any Windows from 7 SP1 on, in Windows PowerShell 2.0 or newer (the one Windows 7 ships), so it uses only
# what 2.0 has: System.Net.WebClient, Get-AuthenticodeSignature and the Explorer zip folder. Microsoft's servers need
# TLS 1.2, which Windows 7's PowerShell 2.0 often cannot use: if a download fails there, run this on a Windows 10 or
# 11 PC and copy the folder over.
param([switch]$Force)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Definition

# Microsoft's own links (their download pages: KB2999226 id 49093, .NET 4.5.2 id 42642, WMF 5.1 id 54616).
$ucrtUrl = 'https://download.microsoft.com/download/1/1/5/11565a9a-ea09-4f0a-a57e-520d5d138140/' +
    'Windows6.1-KB2999226-x64.msu'
$netUrl = 'https://download.microsoft.com/download/e/2/1/e21644b5-2df2-47c2-91bd-63c560427900/' +
    'NDP452-KB2901907-x86-x64-AllOS-ENU.exe'
$wmfUrl = 'https://go.microsoft.com/fwlink/?linkid=839523'   # Win7AndW2K8R2-KB3191566-x64.zip

function Fail([string]$message) {
    Write-Host "error: $message" -ForegroundColor Red
    exit 1
}

# TLS 1.2 (3072: the enum has no name for it before .NET 4.5). Without it Microsoft's servers refuse the connection.
try {
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor 3072
} catch {
    Write-Host "note: this PowerShell cannot turn on TLS 1.2; the downloads will probably fail here." `
        -ForegroundColor Yellow
    Write-Host "      Run this on a Windows 10 or 11 PC instead and copy the folder over." -ForegroundColor Yellow
}

# Whether `file` carries a valid Authenticode signature from Microsoft.
function Test-Microsoft([string]$file, [switch]$Quiet) {
    if (-not (Test-Path $file)) { return $false }
    $s = Get-AuthenticodeSignature -FilePath $file
    $subject = ''
    if ($s.SignerCertificate) { $subject = $s.SignerCertificate.Subject }
    if ($s.Status -eq 'Valid' -and $subject -match 'O=Microsoft Corporation') { return $true }
    if (-not $Quiet) { Write-Host "    signature: $($s.Status) $subject" }
    return $false
}

# Downloads `url` to `file` (through a temporary name, so a broken download never looks finished).
function Get-File([string]$url, [string]$file) {
    $name = Split-Path -Leaf $file
    Write-Host "downloading $name..."
    $part = "$file.part"
    if (Test-Path $part) { Remove-Item -Force $part }
    $client = New-Object System.Net.WebClient
    try {
        $client.DownloadFile($url, $part)
    } catch {
        if (Test-Path $part) { Remove-Item -Force $part }
        Fail "download failed: $url ($($_.Exception.Message))"
    } finally {
        $client.Dispose()
    }
    if (Test-Path $file) { Remove-Item -Force $file }
    Move-Item $part $file
}

# Fails, deleting the file, unless it is validly signed by Microsoft.
function Assert-Microsoft([string]$file) {
    $name = Split-Path -Leaf $file
    if (-not (Test-Microsoft $file)) {
        if (Test-Path $file) { Remove-Item -Force $file }
        Fail "$name is not validly signed by Microsoft: deleted (run this again, or with -Force)"
    }
    Write-Host "ok: signed by Microsoft: $name" -ForegroundColor Green
}

# Downloads and checks one file, unless a validly signed copy is there already.
function Get-Checked([string]$url, [string]$name) {
    $file = Join-Path $here $name
    if (-not $Force -and (Test-Microsoft $file -Quiet)) {
        Write-Host "ok: already here: $name" -ForegroundColor Green
        return
    }
    Get-File $url $file
    Assert-Microsoft $file
}

# Unpacks `zip` into `to` with the Explorer zip folder (Expand-Archive needs PowerShell 5).
function Expand-Zip([string]$zip, [string]$to) {
    if (Get-Command Expand-Archive -ErrorAction SilentlyContinue) {
        Expand-Archive -LiteralPath $zip -DestinationPath $to -Force
        return
    }
    $shell = New-Object -ComObject Shell.Application
    $source = $shell.NameSpace($zip)
    $target = $shell.NameSpace($to)
    if ($source -eq $null -or $target -eq $null) { Fail "cannot open $zip" }
    $count = @($source.Items()).Count
    $target.CopyHere($source.Items(), 0x14)   # 0x4 no progress window, 0x10 yes to all
    # CopyHere can return before it has finished: wait (up to two minutes) for every item.
    for ($i = 0; $i -lt 240 -and @(Get-ChildItem $to).Count -lt $count; $i++) { Start-Sleep -Milliseconds 500 }
}

Write-Host "WheelBri: Windows 7 prerequisites, into $here"
Write-Host ""

Get-Checked $ucrtUrl 'Windows6.1-KB2999226-x64.msu'
Get-Checked $netUrl 'NDP452-KB2901907-x86-x64-AllOS-ENU.exe'

# WMF 5.1 comes as a zip (the .msu and Microsoft's Install-WMF5.1.ps1): only the .msu is kept; install-
# prerequisites.cmd installs it with wusa, as Install-WMF5.1.ps1 does.
$wmfName = 'Win7AndW2K8R2-KB3191566-x64.msu'
$wmf = Join-Path $here $wmfName
if (-not $Force -and (Test-Microsoft $wmf -Quiet)) {
    Write-Host "ok: already here: $wmfName" -ForegroundColor Green
} else {
    $zip = Join-Path $here 'Win7AndW2K8R2-KB3191566-x64.zip'
    $unpacked = Join-Path $here 'wmf-unpacked'
    if (Test-Path $unpacked) { Remove-Item -Recurse -Force $unpacked }
    Get-File $wmfUrl $zip
    New-Item -ItemType Directory -Path $unpacked | Out-Null
    Expand-Zip $zip $unpacked
    $found = Get-ChildItem $unpacked -Recurse | Where-Object { $_.Name -like '*KB3191566*x64*.msu' } |
        Select-Object -First 1
    if ($found -eq $null) { Fail "no KB3191566 x64 .msu in $zip" }
    if (Test-Path $wmf) { Remove-Item -Force $wmf }
    Move-Item $found.FullName $wmf
    Remove-Item -Recurse -Force $unpacked
    Remove-Item -Force $zip
    Assert-Microsoft $wmf
}

Write-Host ""
Write-Host "ready: $here" -ForegroundColor Green
Write-Host "Copy this folder to the Windows 7 PC, right-click install-prerequisites.cmd > Run as administrator,"
Write-Host "restart, then run WheelBri's Setup.exe."
exit 0
