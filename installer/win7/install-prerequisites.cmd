@echo off
rem Installs what WheelBri needs on Windows 7 SP1 (64-bit) before its own setup can run, from the files next to this
rem script (download-prerequisites.cmd, in this folder, downloads them from Microsoft; run it on a Windows 10 or 11 PC
rem if Windows 7 cannot reach Microsoft's servers). In this order, each one skipped when it is already there:
rem   1. Windows6.1-KB2999226-x64.msu              the Universal C Runtime (the app and its shim are built on it)
rem   2. NDP452-KB2901907-x86-x64-AllOS-ENU.exe    .NET Framework 4.5.2 (Windows Management Framework 5.1 needs it)
rem   3. Win7AndW2K8R2-KB3191566-x64.msu           Windows Management Framework 5.1: PowerShell 5.1 (WheelBri's setup)
rem Then restart Windows and run WheelBri's Setup.exe. Run this as Administrator (right-click > Run as administrator).
rem Only cmd and Windows' own tools (ver, wusa, reg, wmic): it runs on a Windows 7 that has nothing else yet.
rem
rem Exit codes: 0 everything installed or already there (restart when it says so), 1 something failed (it says what).
setlocal EnableDelayedExpansion
set "HERE=%~dp0"
set "RESTART=0"
set "FAILED=0"

echo WheelBri: Windows 7 prerequisites
echo.

rem --- this PC ---------------------------------------------------------------------------------------------------
net session >nul 2>&1
if errorlevel 1 (
    echo Run this as Administrator: right-click install-prerequisites.cmd ^> Run as administrator.
    goto :end_failed
)
set "ARCH=%PROCESSOR_ARCHITECTURE%"
if defined PROCESSOR_ARCHITEW6432 set "ARCH=%PROCESSOR_ARCHITEW6432%"
if /i not "%ARCH%"=="AMD64" (
    echo WheelBri needs 64-bit Windows; this is %ARCH%.
    goto :end_failed
)
rem "Microsoft Windows [Version 6.1.7601]": the part in brackets, then its second word.
set "VERSION=unknown"
for /f "tokens=2 delims=[]" %%v in ('ver') do for /f "tokens=2" %%w in ("%%v") do set "VERSION=%%w"
if not "%VERSION:~0,4%"=="6.1." (
    echo This is for Windows 7; this Windows is version %VERSION%. Windows 10 and 11 need none of it.
    goto :end_failed
)
if "%VERSION%"=="6.1.7600" (
    echo Windows 7 needs Service Pack 1 first ^(Windows Update, or KB976932 from Microsoft^).
    goto :end_failed
)
echo Windows %VERSION%, 64-bit: fine

rem Updates signed only with SHA-2 need KB4474419 on Windows 7. The three below are older and may not, but if one
rem fails to install with a signature error, that update (and the servicing stack update KB4490628) comes first.
wmic qfe get HotFixID 2>nul | find /i "KB4474419" >nul
if errorlevel 1 echo note: SHA-2 code signing support ^(KB4474419^) is not installed; see the end if something fails.
echo.

rem --- 1. Universal C Runtime --------------------------------------------------------------------------------------
echo [1/3] Universal C Runtime ^(KB2999226^)
if exist "%SystemRoot%\System32\ucrtbase.dll" (
    echo     already there
) else (
    call :install_msu "Windows6.1-KB2999226-x64.msu"
)
echo.

rem --- 2. .NET Framework 4.5.2 or newer ----------------------------------------------------------------------------
echo [2/3] .NET Framework 4.5.2 or newer
set "RELEASE=0"
set "NETKEY=HKLM\SOFTWARE\Microsoft\NET Framework Setup\NDP\v4\Full"
for /f "tokens=3" %%r in ('reg query "%NETKEY%" /v Release 2^>nul ^| find "Release"') do set /a "RELEASE=%%r"
rem 379893 = 4.5.2 (Microsoft's release numbers); anything newer is fine too.
if !RELEASE! GEQ 379893 (
    echo     already there ^(release !RELEASE!^)
) else (
    set "NET=%HERE%NDP452-KB2901907-x86-x64-AllOS-ENU.exe"
    if not exist "!NET!" (
        echo     missing next to this script: NDP452-KB2901907-x86-x64-AllOS-ENU.exe
        set "FAILED=1"
    ) else (
        echo     installing ^(a few minutes, no window^)...
        "!NET!" /q /norestart
        set "CODE=!ERRORLEVEL!"
        if "!CODE!"=="0" (
            echo     installed
        ) else if "!CODE!"=="3010" (
            echo     installed; Windows needs a restart
            set "RESTART=1"
        ) else if "!CODE!"=="1641" (
            echo     installed; Windows needs a restart
            set "RESTART=1"
        ) else (
            echo     failed ^(exit code !CODE!^)
            set "FAILED=1"
        )
    )
)
echo.

rem --- 3. Windows Management Framework 5.1 --------------------------------------------------------------------------
echo [3/3] Windows Management Framework 5.1 ^(PowerShell 5.1, KB3191566^)
set "PSVERSION=none"
for /f %%p in ('powershell -NoProfile -Command "$PSVersionTable.PSVersion.ToString()" 2^>nul') do set "PSVERSION=%%p"
if "!PSVERSION:~0,3!"=="5.1" (
    echo     already there ^(PowerShell !PSVERSION!^)
) else (
    if "!PSVERSION:~0,2!"=="3." (
        echo     note: WMF 3.0 is installed; Microsoft says to uninstall it first ^(Programs and Features ^>
        echo     installed updates ^> KB2506143^), or PSModulePath may need to be set again afterwards.
    )
    call :install_msu "Win7AndW2K8R2-KB3191566-x64.msu"
)
echo.

if "%FAILED%"=="1" goto :end_failed
if "%RESTART%"=="1" (
    echo Done. Restart Windows now, then run WheelBri's Setup.exe.
) else (
    echo Done. Run WheelBri's Setup.exe.
)
pause
exit /b 0

:end_failed
echo.
echo Not everything was installed ^(see above^). If an update failed with a signature error, install KB4474419 ^(SHA-2
echo code signing support^) and KB4490628 ^(servicing stack^) from the Microsoft Update Catalog first, restart, and
echo run this again.
pause
exit /b 1

rem Installs one .msu from next to this script with wusa, quietly, and notes a needed restart or a failure.
:install_msu
set "MSU=%HERE%%~1"
if not exist "%MSU%" (
    echo     missing next to this script: %~1
    set "FAILED=1"
    goto :eof
)
echo     installing %~1 ^(this can take several minutes, no window^)...
wusa "%MSU%" /quiet /norestart
set "CODE=%ERRORLEVEL%"
rem 0 installed, 3010 installed (restart needed), 2359302 already installed, -2145124329 not for this Windows.
if "%CODE%"=="0" (
    echo     installed
) else if "%CODE%"=="3010" (
    echo     installed; Windows needs a restart
    set "RESTART=1"
) else if "%CODE%"=="2359302" (
    echo     already installed
) else if "%CODE%"=="-2145124329" (
    echo     not for this Windows ^(wusa: not applicable^)
    set "FAILED=1"
) else (
    echo     failed ^(wusa exit code %CODE%^)
    set "FAILED=1"
)
goto :eof
