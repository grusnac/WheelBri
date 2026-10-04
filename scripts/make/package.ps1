# Assembles the Windows installer package from what is already built, checks it, zips it and wraps it in
# Setup.exe (IExpress, part of Windows: no third-party installer tools). package.sh builds everything first.
#
#   package.ps1                            dist\windows\WheelBri-<version>\, WheelBri-<version>.zip and
#                                          WheelBri-<version>-setup.exe
#   package.ps1 -BuildDir build-rel        take the programs from another CMake build tree
#   package.ps1 -NoSetupExe                only the folder and the zip
#
# Needs: build\service\wheelbri.exe, wheelbri-service.exe, build\shim\dinput8.dll (scripts/make/build.sh) and the
# two driver packages (scripts/make/drivers.sh), all Release.
#
# Exit codes: 0 built, 1 something is missing or wrong (the red line says what).

param(
    [string]$BuildDir = 'build',
    [string]$OutDir = 'dist\windows',
    [switch]$NoSetupExe
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
function Fail([string]$text) { Write-Host $text -ForegroundColor Red; exit 1 }
function Say([string]$text) { Write-Host $text -ForegroundColor Cyan }
function Under-Root([string]$path) { if ([IO.Path]::IsPathRooted($path)) { $path } else { Join-Path $root $path } }

# Version: project(... VERSION x.y.z) in the top CMakeLists.txt.
$cmakeLists = Get-Content (Join-Path $root 'CMakeLists.txt') -Raw
if ($cmakeLists -notmatch '(?s)project\s*\(.*?VERSION\s+(\d+\.\d+\.\d+)') { Fail 'No VERSION in CMakeLists.txt project().' }
$version = $Matches[1]
$build = Under-Root $BuildDir
$out = Under-Root $OutDir
$name = "WheelBri-$version"   # the product name, as the repository folder
$stage = Join-Path $out $name
Say "packaging WheelBri $version into $stage"

# 1. The built pieces, each checked before anything is copied.
$programs = @(
    @{ From = "$build\service\wheelbri.exe";         To = 'wheelbri.exe' },
    @{ From = "$build\service\wheelbri-service.exe"; To = 'wheelbri-service.exe' },
    @{ From = "$build\shim\dinput8.dll";             To = 'shim\dinput8.dll' }
)
foreach ($program in $programs) {
    if (-not (Test-Path $program.From)) { Fail "missing $($program.From): run scripts/make/build.sh first." }
}
# The programs must start on a PC without MSYS2: no MinGW runtime DLL may be imported (they link it statically).
foreach ($program in $programs) {
    $text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($program.From))
    foreach ($dll in 'libstdc++-6.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll') {
        if ($text.IndexOf($dll, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            Fail "$($program.From) needs $dll at run time; it would not start on a PC without MSYS2 (rebuild: scripts/make/rebuild.sh)."
        }
    }
}

$drivers = @(
    @{ Name = 'wheelbri-g920'; Folder = 'driver' },
    @{ Name = 'wheelbri-hide'; Folder = 'driver\hide' }
)
$signers = @{}
foreach ($driver in $drivers) {
    $release = Join-Path $root "$($driver.Folder)\x64\Release"
    $inf = Get-ChildItem $release -Recurse -Filter "$($driver.Name).inf" -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.DirectoryName "$($driver.Name).cat") } | Select-Object -First 1
    if (-not $inf) { Fail "no built $($driver.Name) package under $release (.inf + .cat): run scripts/make/drivers.sh first." }
    if (-not (Test-Path (Join-Path $inf.DirectoryName "$($driver.Name).dll"))) { Fail "$($inf.DirectoryName) has no $($driver.Name).dll." }
    $signature = Get-AuthenticodeSignature (Join-Path $inf.DirectoryName "$($driver.Name).cat")
    if (-not $signature.SignerCertificate) { Fail "$($driver.Name).cat is not signed." }
    $driver.Package = $inf.DirectoryName
    $driver.Relative = "$($driver.Folder)\x64\Release\$($driver.Name)"   # where install.ps1 and the app look
    $signers[$signature.SignerCertificate.Thumbprint] = $signature.SignerCertificate.Subject
    Write-Host "  $($driver.Name): $($driver.Package)"
}
# One certificate for both: the installer trusts it once, and the uninstaller removes it once.
if ($signers.Count -ne 1) {
    Fail ("the drivers are signed with different certificates ($($signers.Values -join '; ')): rebuild them on one PC " +
          '(scripts/make/drivers.sh -Clean).')
}
Write-Host "  signed by: $(@($signers.Values)[0])"

# 2. The package folder, laid out like the repo so the driver scripts and the app find everything.
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
function Put([string]$from, [string]$to) {
    $target = Join-Path $stage $to
    New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
    Copy-Item $from $target -Force
}
# Text with Windows line ends (cmd can misread a .cmd with LF ones). A .ps1 gets a UTF-8 BOM, without which Windows
# PowerShell 5.1 reads it as ANSI; a .cmd must not have one (cmd would run the BOM as a command).
function Put-Text([string]$from, [string]$to) {
    $target = Join-Path $stage $to
    New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
    $text = [IO.File]::ReadAllText($from) -replace "`r`n", "`n" -replace "`n", "`r`n"
    [IO.File]::WriteAllText($target, $text, (New-Object Text.UTF8Encoding($target.EndsWith('.ps1'))))
}

foreach ($program in $programs) { Put $program.From $program.To }
Put-Text (Join-Path $root 'config\wheelbri-shim.ini') 'shim\wheelbri-shim.ini'
foreach ($ini in Get-ChildItem (Join-Path $root 'config') -Filter '*.ini') {
    if ($ini.Name -ne 'wheelbri-shim.ini') { Put-Text $ini.FullName "config\$($ini.Name)" }
}
foreach ($script in 'driver\common.ps1', 'driver\install-driver.ps1', 'driver\uninstall-driver.ps1',
                    'driver\hide\install-hide.ps1', 'driver\hide\uninstall-hide.ps1') {
    Put-Text (Join-Path $root $script) $script
}
foreach ($driver in $drivers) {
    foreach ($file in Get-ChildItem $driver.Package -File) {
        if ($file.Extension -in '.inf', '.cat', '.dll') { Put $file.FullName (Join-Path $driver.Relative $file.Name) }
    }
}
foreach ($file in 'install.ps1', 'uninstall.ps1', 'Setup.cmd', 'Uninstall.cmd', 'README.txt') {
    Put-Text (Join-Path $root "installer\$file") $file
}
Set-Content -Path (Join-Path $stage 'VERSION.txt') -Value $version -Encoding ASCII

# 3. What install.ps1 checks for must be there (the same list), so a package never fails on the user's PC.
$expected = @('wheelbri.exe', 'wheelbri-service.exe', 'config\g920-virtual-device.ini', 'config\g920-in-game-shim.ini', 'install.ps1', 'uninstall.ps1',
              'Setup.cmd', 'Uninstall.cmd', 'README.txt', 'VERSION.txt', 'shim\dinput8.dll') +
            ($drivers | ForEach-Object { $d = $_; '.inf', '.cat', '.dll' | ForEach-Object { "$($d.Relative)\$($d.Name)$_" } })
foreach ($file in $expected) {
    if (-not (Test-Path (Join-Path $stage $file))) { Fail "the package lacks $file (packaging bug)." }
}
foreach ($script in Get-ChildItem $stage -Recurse -Filter '*.ps1') {
    $errors = $null
    [void][Management.Automation.Language.Parser]::ParseFile($script.FullName, [ref]$null, [ref]$errors)
    if ($errors.Count -gt 0) { Fail "$($script.FullName) does not parse: $($errors[0].Message)" }
}
$size = (Get-ChildItem $stage -Recurse -File | Measure-Object Length -Sum).Sum
Write-Host ("  {0} files, {1:N1} MB" -f @(Get-ChildItem $stage -Recurse -File).Count, ($size / 1MB))

# 4. The zip (the package folder's contents at its top).
$zip = Join-Path $out "$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal
Write-Host "  zip: $zip" -ForegroundColor Green

# 5. Setup.exe: IExpress unpacks Setup.cmd and the zip to a temporary folder and runs Setup.cmd. IExpress keeps no
#    folders, hence the zip.
if ($NoSetupExe) { Write-Host "packaged (no Setup.exe): $stage" -ForegroundColor Green; exit 0 }
$iexpress = Join-Path $env:SystemRoot 'System32\iexpress.exe'
if (-not (Test-Path $iexpress)) { Fail "IExpress not found ($iexpress); -NoSetupExe builds the zip only." }
$work = Join-Path $out 'iexpress'
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
New-Item -ItemType Directory -Force -Path $work | Out-Null
Copy-Item (Join-Path $stage 'Setup.cmd') $work
Copy-Item $zip (Join-Path $work 'wheelbri.zip')
$setup = Join-Path $out "$name-setup.exe"
if (Test-Path $setup) { Remove-Item $setup -Force }
$sed = @"
[Version]
Class=IEXPRESS
SEDVersion=3
[Options]
PackagePurpose=InstallApp
ShowInstallProgramWindow=0
HideExtractAnimation=1
UseLongFileName=1
InsideCompressed=0
CAB_FixedSize=0
CAB_ResvCodeSigning=0
RebootMode=N
InstallPrompt=%InstallPrompt%
DisplayLicense=%DisplayLicense%
FinishMessage=%FinishMessage%
TargetName=%TargetName%
FriendlyName=%FriendlyName%
AppLaunched=%AppLaunched%
PostInstallCmd=%PostInstallCmd%
AdminQuietInstCmd=%AdminQuietInstCmd%
UserQuietInstCmd=%UserQuietInstCmd%
SourceFiles=SourceFiles
[Strings]
InstallPrompt=
DisplayLicense=
FinishMessage=
TargetName=$setup
FriendlyName=WheelBri $version setup
AppLaunched=cmd /c Setup.cmd
PostInstallCmd=<None>
AdminQuietInstCmd=cmd /c Setup.cmd -Quiet
UserQuietInstCmd=cmd /c Setup.cmd -Quiet
FILE0="Setup.cmd"
FILE1="wheelbri.zip"
[SourceFiles]
SourceFiles0=$work\
[SourceFiles0]
%FILE0%=
%FILE1%=
"@
# IExpress reads the SED with the INI functions, which need Windows line ends; this script's own may be LF, and so
# is the here-string above.
$sed = $sed -replace "`r?`n", "`r`n"
$sedPath = Join-Path $work 'wheelbri.sed'
Set-Content -Path $sedPath -Value $sed -Encoding ASCII
Say 'building Setup.exe with IExpress (a makecab window opens for a moment)...'
# IExpress takes the SED path literally: quotes become part of the name ("Error opening the IExpress Self Extraction
# Directive file"), so the path goes unquoted, as its 8.3 short form when it has spaces. Not /Q, so that IExpress can
# still show an error. Its exit code is not trusted: the new Setup.exe existing is the test (the old one was deleted).
$sedArgument = $sedPath
if ($sedArgument.Contains(' ')) { $sedArgument = (New-Object -ComObject Scripting.FileSystemObject).GetFile($sedPath).ShortPath }
$p = Start-Process $iexpress -ArgumentList "/N $sedArgument" -Wait -PassThru
if (-not (Test-Path $setup)) {
    Fail ("IExpress built no Setup.exe (exit $($p.ExitCode)). $sedPath is kept: run 'iexpress /N $sedArgument' to " +
          'see its message.')
}
if ($p.ExitCode -ne 0) { Write-Host "  IExpress exited $($p.ExitCode) but built $setup" -ForegroundColor Yellow }
Remove-Item $work -Recurse -Force
Write-Host ''
Write-Host "packaged WheelBri $version" -ForegroundColor Green
Write-Host "  installer: $setup"
Write-Host "  folder:    $stage (Setup.cmd installs from it too)"
Write-Host "  zip:       $zip"
