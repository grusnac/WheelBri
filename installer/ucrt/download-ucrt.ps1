# Downloads Microsoft's Universal C Runtime update (KB2999226) for 64-bit Windows 7 SP1, 8 and 8.1 into this folder,
# each checked to be validly signed by Microsoft; a copy already here that is signed is kept. scripts/make/package.ps1
# runs it and ships the three in the package's ucrt\, from which wheelbri-setup.exe installs the one for that Windows
# when the Universal C Runtime is missing (installer/setup/SetupSteps.cpp). The files are Microsoft's, never committed.
#
#   download-ucrt.ps1           download what is missing
#   download-ucrt.ps1 -Force    download all three again
#
# Exit codes: 0 all three here and signed by Microsoft, 1 something failed (the red line says what).

param([switch]$Force)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Definition

# Microsoft's own links (their Download Center pages: Windows 7 id 49093, Windows 8 id 49082, Windows 8.1 id 49081).
$updates = @(
    @{ Name = 'Windows6.1-KB2999226-x64.msu'
       Url = 'https://download.microsoft.com/download/1/1/5/11565a9a-ea09-4f0a-a57e-520d5d138140/Windows6.1-KB2999226-x64.msu' },
    @{ Name = 'Windows8-RT-KB2999226-x64.msu'
       Url = 'https://download.microsoft.com/download/a/c/1/ac15393f-a6e6-469b-b222-c44b3bb6eccc/Windows8-RT-KB2999226-x64.msu' },
    @{ Name = 'Windows8.1-KB2999226-x64.msu'
       Url = 'https://download.microsoft.com/download/9/6/f/96fd0525-3ddf-423d-8845-5f92f4a6883e/Windows8.1-KB2999226-x64.msu' }
)

function Fail([string]$message) {
    Write-Host "error: $message" -ForegroundColor Red
    exit 1
}

# TLS 1.2 (3072: older .NET has no name for it). Without it Microsoft's servers refuse the connection.
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor 3072

# Whether `file` carries a valid Authenticode signature from Microsoft.
function Test-Microsoft([string]$file) {
    if (-not (Test-Path -LiteralPath $file)) { return $false }
    $s = Get-AuthenticodeSignature -FilePath $file
    return $s.Status -eq 'Valid' -and $s.SignerCertificate -and $s.SignerCertificate.Subject -match 'O=Microsoft Corporation'
}

foreach ($update in $updates) {
    $file = Join-Path $here $update.Name
    if (-not $Force -and (Test-Microsoft $file)) {
        Write-Host "ok: already here: $($update.Name)" -ForegroundColor Green
        continue
    }
    Write-Host "downloading $($update.Name)..."
    # Through a temporary name, so a broken download never looks finished.
    $part = "$file.part"
    if (Test-Path -LiteralPath $part) { Remove-Item -LiteralPath $part -Force }
    $client = New-Object System.Net.WebClient
    try {
        $client.DownloadFile($update.Url, $part)
    } catch {
        if (Test-Path -LiteralPath $part) { Remove-Item -LiteralPath $part -Force }
        Fail "download failed: $($update.Url) ($($_.Exception.Message))"
    } finally {
        $client.Dispose()
    }
    if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file -Force }
    Move-Item -LiteralPath $part -Destination $file
    if (-not (Test-Microsoft $file)) {
        Remove-Item -LiteralPath $file -Force
        Fail "$($update.Name) is not validly signed by Microsoft: deleted"
    }
    Write-Host "ok: signed by Microsoft: $($update.Name)" -ForegroundColor Green
}
exit 0
