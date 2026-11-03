# Shared by the driver install/uninstall scripts (dot-sourced: . "$PSScriptRoot\..\common.ps1"). Each step either
# works or stops the script with a red line saying what failed; nothing reports success after a failed step.

# pnputil exit codes that mean "done": 0, 259 (ERROR_NO_MORE_ITEMS: added, but no device to put it on now) and
# 3010 (ERROR_SUCCESS_REBOOT_REQUIRED: done, active after a restart).
$script:PnputilOk = @(0, 259, 3010)

function Stop-WithError([string]$message, [int]$code = 1) {
    Write-Host $message -ForegroundColor Red
    exit $code
}

function Assert-Admin {
    $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $admin) { Stop-WithError 'Run this from an Administrator PowerShell.' }
}

# Runs pnputil and stops the script unless it ended with one of the codes above. Returns the exit code.
function Invoke-Pnputil([string[]]$arguments, [string]$what) {
    & pnputil @arguments | ForEach-Object { Write-Host $_ }   # shown, not returned
    $code = $LASTEXITCODE
    if ($script:PnputilOk -notcontains $code) {
        Stop-WithError "$what failed: pnputil $($arguments -join ' ') exited with $code"
    }
    if ($code -eq 3010) { Write-Host "$what needs a restart of Windows to take effect." -ForegroundColor Yellow }
    return $code
}

# Adds the built package <inf> (pnputil /add-driver, with /install when <install>) and stops the script unless pnputil
# ended well. Returns its published name in Windows' driver store (oemNN.inf): the copy of <name>.inf that is new
# there, or, when the very same package was there already, the one pnputil names. Empty when neither says.
function Add-Package($inf, [string]$name, [bool]$install, [string]$what) {
    $before = @(Get-InstalledPackages $name | ForEach-Object { $_.ToLower() })
    $arguments = @('/add-driver', $inf.FullName)
    if ($install) { $arguments += '/install' }
    $lines = @(& pnputil @arguments)
    $code = $LASTEXITCODE
    $lines | ForEach-Object { Write-Host $_ }
    if ($script:PnputilOk -notcontains $code) {
        Stop-WithError "$what failed: pnputil $($arguments -join ' ') exited with $code"
    }
    if ($code -eq 3010) { Write-Host "$what needs a restart of Windows to take effect." -ForegroundColor Yellow }
    $after = @(Get-InstalledPackages $name | ForEach-Object { $_.ToLower() })
    $new = @($after | Where-Object { $before -notcontains $_ })
    if ($new.Count -eq 1) { return $new[0] }
    foreach ($line in $lines) {
        if ($line -match '(oem\d+\.inf)' -and ($after -contains $Matches[1].ToLower())) { return $Matches[1].ToLower() }
    }
    return ''
}

# Every installed copy of <name>.inf but <keep>: each build is a new package and pnputil keeps them all, so earlier
# installs pile up in Windows' driver store. Plain /delete-driver: pnputil refuses a copy a device still uses, and
# that one stays (said; a later install takes it). Nothing when <keep> is not known. Returns how many went.
function Remove-OlderCopies([string]$name, [string]$keep) {
    if (-not $keep) {
        Write-Host "older copies of $name.inf left as they are (which one is new is not known)" -ForegroundColor Yellow
        return 0
    }
    $removed = 0
    foreach ($package in @(Get-InstalledPackages $name)) {
        if ($package.ToLower() -eq $keep.ToLower()) { continue }
        $lines = @(& pnputil /delete-driver $package)
        if ($LASTEXITCODE -eq 0) {
            $removed++
            Write-Host "removed $package (an older copy of $name.inf)"
        } else {
            $lines | ForEach-Object { Write-Host $_ }
            Write-Host "kept $package (an older copy of $name.inf, still in use?): pnputil exited with $LASTEXITCODE" `
                -ForegroundColor Yellow
        }
    }
    return $removed
}

# The built package <name>.inf + <name>.cat under <folder>\x64\<Configuration>, or stop.
function Find-BuiltPackage([string]$folder, [string]$name, [string]$configuration) {
    $out = Join-Path $folder "x64\$configuration"
    $inf = Get-ChildItem $out -Recurse -Filter "$name.inf" -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.DirectoryName "$name.cat") } | Select-Object -First 1
    if (-not $inf) { Stop-WithError "No built driver package under $out ($name.inf + .cat): build it first." }
    Write-Host "package: $($inf.DirectoryName)"
    return $inf
}

# Where the install records the thumbprint of each certificate it trusted (one value per thumbprint, the subject as
# its data), so that removing "our" certificate removes exactly those and never another one that happens to have a
# similar name (every WDK test certificate is called WDKTestCert).
$script:SigningCertificatesKey = 'HKLM:\SOFTWARE\wheelbri\SigningCertificates'

# A package names the one certificate its drivers are signed with (driver\signing-certificate.txt, written by
# scripts/make/package.ps1); a build from this repository has none.
$script:PinnedCertificateFile = Join-Path $PSScriptRoot 'signing-certificate.txt'

# What a driver signing certificate may be used for: code signing (and Microsoft's lifetime signing, which only limits
# it further). The certificate goes into the machine's trusted roots, so one that could also vouch for web sites,
# e-mail or logons, or for other certificates (a certificate authority), is never trusted.
$script:CodeSigningUse = '1.3.6.1.5.5.7.3.3'
$script:AllowedSigningUses = @($script:CodeSigningUse, '1.3.6.1.4.1.311.10.3.13')

# Why <certificate> must not be trusted as a driver signer, or '' when it may.
function Get-SigningUseProblem($certificate) {
    $usage = @($certificate.Extensions | Where-Object { $_ -is [Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension] })
    if ($usage.Count -eq 0) { return 'it is not limited to code signing (it could vouch for anything)' }
    $uses = @($usage | ForEach-Object { $_.EnhancedKeyUsages } | ForEach-Object { $_.Value })
    if ($uses -notcontains $script:CodeSigningUse) { return 'it is not a code-signing certificate' }
    $others = @($uses | Where-Object { $script:AllowedSigningUses -notcontains $_ })
    if ($others.Count -gt 0) { return "it may also be used for other things ($($others -join ', '))" }
    $constraints = @($certificate.Extensions | Where-Object { $_ -is [Security.Cryptography.X509Certificates.X509BasicConstraintsExtension] })
    if (@($constraints | Where-Object { $_.CertificateAuthority }).Count -gt 0) {
        return 'it is a certificate authority (it could vouch for other certificates)'
    }
    return ''
}

# The thumbprint the package names (upper case), '' when there is no such file (a build from this repository).
function Get-PinnedThumbprint {
    if (-not (Test-Path -LiteralPath $script:PinnedCertificateFile -PathType Leaf)) { return '' }
    $text = (Get-Content -LiteralPath $script:PinnedCertificateFile -Raw).Trim().ToUpper()
    if ($text -notmatch '^[0-9A-F]{40}$') { Stop-WithError "$($script:PinnedCertificateFile) holds no certificate thumbprint: the package is damaged, download it again." }
    return $text
}

# Whether the certificate <thumbprint> was made on this PC: its private key is in one of this PC's stores (WheelBri's
# own from new-signing-cert.ps1 in CurrentUser\My, the WDK's WDKTestCert in CurrentUser\PrivateCertStore).
function Test-MadeOnThisPc([string]$thumbprint) {
    foreach ($store in 'Cert:\CurrentUser\My', 'Cert:\CurrentUser\PrivateCertStore', 'Cert:\LocalMachine\My') {
        $certificate = Get-Item -Path "$store\$thumbprint" -ErrorAction SilentlyContinue
        if ($certificate -and $certificate.HasPrivateKey) { return $true }
    }
    return $false
}

# Trusts the certificate that signed the package's catalog (Root and TrustedPublisher of this PC) and records its
# thumbprint, only when all of these hold:
#   - the catalog's signature is intact (only its root may still be unknown to this PC);
#   - the certificate is the one the package names (driver\signing-certificate.txt), or, for a build from this
#     repository, one made on this PC;
#   - it is a code-signing certificate and nothing more (Get-SigningUseProblem);
#   - once trusted, the catalog checks out as Valid; if not, what was added is taken out again.
# Added straight from memory: no .cer file in %TEMP%, which another process of this user could replace between
# writing and importing it.
function Add-PackageCertificate($inf, [string]$name) {
    $cat = Join-Path $inf.DirectoryName "$name.cat"
    $signature = Get-AuthenticodeSignature -LiteralPath $cat
    if (-not $signature.SignerCertificate) { Stop-WithError "The catalog is not signed: $cat" }
    $certificate = $signature.SignerCertificate
    $thumbprint = $certificate.Thumbprint.ToUpper()
    $who = "$($certificate.Subject) ($thumbprint)"
    if (@('Valid', 'UnknownError', 'NotTrusted') -notcontains "$($signature.Status)") {
        Stop-WithError ("The catalog's signature does not hold ($($signature.Status)): $cat. The package was changed " +
                        'after it was built: download or build it again.')
    }
    $pinned = Get-PinnedThumbprint
    if ($pinned) {
        if ($thumbprint -ne $pinned) {
            Stop-WithError "$name.cat is signed by $who, not by the certificate this package names ($pinned): not trusting it."
        }
    } elseif (-not (Test-MadeOnThisPc $thumbprint)) {
        Stop-WithError ("$name.cat is signed by $who, which was not made on this PC. A build from the repository is " +
                        'installed only with a certificate made here (driver\new-signing-cert.ps1); install a package ' +
                        '(Setup.exe) instead, which names its certificate.')
    }
    $problem = Get-SigningUseProblem $certificate
    if ($problem) { Stop-WithError "Not trusting $who`: $problem." }

    $added = @()
    foreach ($storeName in 'Root', 'TrustedPublisher') {
        if (Test-Path -Path "Cert:\LocalMachine\$storeName\$thumbprint") { continue }
        try {
            $store = New-Object Security.Cryptography.X509Certificates.X509Store($storeName, 'LocalMachine')
            $store.Open('ReadWrite')
            try { $store.Add($certificate) } finally { $store.Close() }
            $added += $storeName
        } catch {
            Stop-WithError "Cannot trust the signing certificate in $storeName`: $($_.Exception.Message)"
        }
    }
    $status = (Get-AuthenticodeSignature -LiteralPath $cat).Status
    if ("$status" -ne 'Valid') {
        foreach ($storeName in $added) {
            Remove-Item -Path "Cert:\LocalMachine\$storeName\$thumbprint" -Force -ErrorAction SilentlyContinue
        }
        Stop-WithError "$name.cat does not check out even with its certificate trusted ($status): the certificate was taken out again."
    }
    try {
        if (-not (Test-Path $script:SigningCertificatesKey)) { New-Item -Path $script:SigningCertificatesKey -Force | Out-Null }
        Set-ItemProperty -Path $script:SigningCertificatesKey -Name $thumbprint -Value $certificate.Subject
    } catch {
        Write-Host ("could not record the certificate's thumbprint ($($_.Exception.Message)); removing it later finds " +
                    'it from the signed packages instead') -ForegroundColor Yellow
    }
    Write-Host "trusted on this PC: $who"
}

# The certificates an earlier install trusted (recorded in SigningCertificates) that no installed driver package is
# signed with any more: removed, so trust does not pile up across upgrades (every release is signed with a
# certificate of its own). Any third-party package counts, not only ours: a certificate one of them still needs stays.
# When the installed packages cannot be read, nothing is removed. Returns how many could not be removed.
function Remove-UnusedSigningCertificates {
    $recorded = Get-Item -Path $script:SigningCertificatesKey -ErrorAction SilentlyContinue
    if (-not $recorded) { return 0 }
    $candidates = @($recorded.GetValueNames() | Where-Object { $_ -match '^[0-9A-Fa-f]{40}$' } | ForEach-Object { $_.ToUpper() })
    if ($candidates.Count -eq 0) { return 0 }
    try {
        $packages = @(Get-WindowsDriver -Online -ErrorAction Stop)   # the third-party packages
    } catch {
        Write-Host "could not list the installed drivers ($($_.Exception.Message)): earlier certificates are kept" -ForegroundColor Yellow
        return 0
    }
    $inUse = @{}
    foreach ($package in $packages) {
        if (-not $package.OriginalFileName) { continue }
        foreach ($catalog in @(Get-ChildItem -LiteralPath (Split-Path $package.OriginalFileName) -Filter '*.cat' -ErrorAction SilentlyContinue)) {
            $signer = (Get-AuthenticodeSignature -LiteralPath $catalog.FullName).SignerCertificate
            if ($signer) { $inUse[$signer.Thumbprint.ToUpper()] = $true }
        }
    }
    $unused = @($candidates | Where-Object { -not $inUse.ContainsKey($_) })
    if ($unused.Count -eq 0) { return 0 }
    Write-Host "no installed driver is signed with $($unused.Count) earlier certificate(s) any more: removing them"
    return (Remove-SigningCertificates $unused)
}

# The thumbprints of the certificates that signed our driver packages <names> (e.g. 'wheelbri-wheel'): the ones
# recorded at install (above), the signers of the installed packages' catalogs (in Windows' driver store, so look
# before removing the packages) and the signers of the catalogs under <folders> (a package's or install folder's
# driver\, the repository's). Never a certificate found by its name: other drivers on this PC may be test-signed too.
function Get-SigningThumbprints([string[]]$names, [string[]]$folders = @()) {
    $thumbprints = @()
    $recorded = Get-Item -Path $script:SigningCertificatesKey -ErrorAction SilentlyContinue
    if ($recorded) { $thumbprints += @($recorded.GetValueNames()) }
    $catalogs = @()
    try {
        $catalogs += @(Get-WindowsDriver -Online -ErrorAction Stop |
            Where-Object { $_.OriginalFileName -and ($names -contains [IO.Path]::GetFileNameWithoutExtension($_.OriginalFileName)) } |
            ForEach-Object { Get-ChildItem (Split-Path $_.OriginalFileName) -Filter '*.cat' -ErrorAction SilentlyContinue })
    } catch {
        Write-Host "Get-WindowsDriver is not available ($($_.Exception.Message)); looking only at the recorded and packaged certificates" -ForegroundColor Yellow
    }
    foreach ($folder in @($folders | Where-Object { $_ -and (Test-Path $_) })) {
        $catalogs += @(Get-ChildItem $folder -Recurse -Filter '*.cat' -ErrorAction SilentlyContinue |
                       Where-Object { $names -contains $_.BaseName })
    }
    foreach ($catalog in $catalogs) {
        $signer = (Get-AuthenticodeSignature $catalog.FullName).SignerCertificate
        if ($signer) { $thumbprints += $signer.Thumbprint }
    }
    return @($thumbprints | Where-Object { $_ -match '^[0-9A-Fa-f]{40}$' } | ForEach-Object { $_.ToUpper() } | Sort-Object -Unique)
}

# Removes the certificates <thumbprints> from LocalMachine Root and TrustedPublisher, and their record. Returns how
# many could not be removed.
function Remove-SigningCertificates([string[]]$thumbprints) {
    $failed = 0
    foreach ($thumbprint in $thumbprints) {
        foreach ($store in 'Root', 'TrustedPublisher') {
            $path = "Cert:\LocalMachine\$store\$thumbprint"
            $certificate = Get-Item -Path $path -ErrorAction SilentlyContinue
            if (-not $certificate) { continue }
            Remove-Item -Path $path -Force -ErrorAction SilentlyContinue
            if (Test-Path $path) {
                Write-Host "could not remove certificate $($certificate.Subject) ($thumbprint) from $store" -ForegroundColor Yellow
                $failed++
            } else {
                Write-Host "removed certificate $($certificate.Subject) ($thumbprint) from $store"
            }
        }
        if (-not (Test-Path "Cert:\LocalMachine\Root\$thumbprint") -and -not (Test-Path "Cert:\LocalMachine\TrustedPublisher\$thumbprint")) {
            Remove-ItemProperty -Path $script:SigningCertificatesKey -Name $thumbprint -ErrorAction SilentlyContinue
        }
    }
    return $failed
}

# The published names (oemNN.inf) of the installed packages whose original file is <name>.inf. Uses DISM's view,
# which does not depend on the language of pnputil's output; falls back to pnputil's English labels.
function Get-InstalledPackages([string]$name) {
    try {
        return @(Get-WindowsDriver -Online -ErrorAction Stop |
            Where-Object { (Split-Path $_.OriginalFileName -Leaf) -ieq "$name.inf" } | ForEach-Object { $_.Driver })
    } catch {
        Write-Host "Get-WindowsDriver is not available ($($_.Exception.Message)); reading pnputil instead" -ForegroundColor Yellow
    }
    $found = @()
    $published = $null
    foreach ($line in (pnputil /enum-drivers)) {
        if ($line -match '(oem\d+\.inf)') { $published = $Matches[1] }
        if ($line -match [regex]::Escape("$name.inf") -and $published) { $found += $published; $published = $null }
    }
    return $found
}

# The driver's older names: wheelemu-<x> (the project before the rename to wheelbri on 2026-10-03), and for the virtual
# wheel wheelbri-g920 / wheelemu-g920 (it was named after its first wheel until 2026-10-19, wheelbri-wheel since).
function Get-LegacyNames([string]$name) {
    $names = @($name -replace '^wheelbri-', 'wheelemu-')
    if ($name -eq 'wheelbri-wheel') { $names += @('wheelbri-g920', 'wheelemu-g920') }
    return @($names | Where-Object { $_ -ne $name } | Select-Object -Unique)
}

# The same driver under an older name (Get-LegacyNames): removed before the new one goes on, so two hiding filters
# never sit on a rig device (the old one would also count as "the filter is there" while reading a setting nothing
# writes any more, so the device would stay visible), and two packages never claim the same virtual wheel. Returns how
# many.
function Remove-LegacyPackages([string]$name) {
    $count = 0
    foreach ($legacy in (Get-LegacyNames $name)) {
        $packages = @(Get-InstalledPackages $legacy)
        foreach ($package in $packages) {
            Write-Host "removing $package ($legacy.inf, the old name of $name)"
            Invoke-Pnputil @('/delete-driver', $package, '/uninstall', '/force') "removing $package" | Out-Null
        }
        $count += $packages.Count
    }
    return $count
}

# The devices (HIDClass, present) whose driver is one of the installed copies of <name>.inf (or of an older name,
# Get-LegacyNames): for wheelbri-hide, the devices that have the filter, whatever they are.
function Get-DevicesUsing([string]$name) {
    $all = @(Get-InstalledPackages $name)
    foreach ($legacy in (Get-LegacyNames $name)) { $all += @(Get-InstalledPackages $legacy) }
    $published = @($all | ForEach-Object { $_.ToLower() })
    if ($published.Count -eq 0) { return @() }
    # One property query for all of them (one per device is slow).
    $present = @(Get-PnpDevice -PresentOnly -Class HIDClass -ErrorAction SilentlyContinue)
    if ($present.Count -eq 0) { return @() }
    $using = @(Get-PnpDeviceProperty -InstanceId @($present | ForEach-Object { $_.InstanceId }) `
                   -KeyName 'DEVPKEY_Device_DriverInfPath' -ErrorAction SilentlyContinue |
               Where-Object { $_.Data -and ($published -contains ([string]$_.Data).ToLower()) } |
               ForEach-Object { $_.InstanceId })
    return @($present | Where-Object { $using -contains $_.InstanceId })
}

# The virtual wheels the app creates (SWD\wheelbri\G25_<n>, G27_<n>, G920_<n>, G923_<n>, the classic G25L_<n>, G27L_<n>; SWD\wheelemu\... from before
# the rename) and the HID devices under them, present or not: after a crash, or a wheel Windows kept half-removed, they
# stay listed (Device Manager > Show hidden devices). Children first. Returns how many could not be removed.
function Remove-VirtualDevices {
    $all = @(Get-PnpDevice -ErrorAction SilentlyContinue)
    $ours = @($all | Where-Object { $_.InstanceId -like 'SWD\wheelbri\*' -or $_.InstanceId -like 'SWD\wheelemu\*' })
    if ($ours.Count -eq 0) { return 0 }
    $ids = @($ours | ForEach-Object { $_.InstanceId })
    $hid = @($all | Where-Object { $_.Class -eq 'HIDClass' -and $ids -notcontains $_.InstanceId })
    $children = @()
    if ($hid.Count -gt 0) {
        # One property query for all of them (one per device is slow).
        $children = @(Get-PnpDeviceProperty -InstanceId @($hid | ForEach-Object { $_.InstanceId }) `
                          -KeyName 'DEVPKEY_Device_Parent' -ErrorAction SilentlyContinue |
                      Where-Object { $_.Data -and ($ids -contains [string]$_.Data) } | ForEach-Object { $_.InstanceId })
    }
    $failed = 0
    foreach ($id in @($children) + @($ids)) {
        & pnputil /remove-device $id | Out-Null
        if ($LASTEXITCODE -in @(0, 3010)) {
            Write-Host "removed device $id"
        } else {
            Write-Host "could not remove device $id (pnputil $LASTEXITCODE): Device Manager > View > Show hidden devices > Uninstall" -ForegroundColor Yellow
            $failed++
        }
    }
    return $failed
}

# install-driver.ps1 and install-hide.ps1 started by the app at Start keep running when the app closes (stopped
# halfway, they could leave a device with a half-switched driver): wait for them before removing anything, never end
# them. Returns $false when one still runs after $seconds.
function Wait-DriverInstallers([int]$seconds = 120) {
    $deadline = (Get-Date).AddSeconds($seconds)
    $said = $false
    while ($true) {
        # "install-driver.ps1", not "uninstall-driver.ps1" (a letter before "install" means another script).
        $running = @(Get-CimInstance Win32_Process -Filter "Name = 'powershell.exe'" -ErrorAction SilentlyContinue |
                     Where-Object { $_.ProcessId -ne $PID -and "$($_.CommandLine)" -match '(?<![A-Za-z])install-(driver|hide)\.ps1' })
        if ($running.Count -eq 0) { return $true }
        if ((Get-Date) -ge $deadline) { return $false }
        if (-not $said) {
            Write-Host '    waiting for a driver installer WheelBri started to finish (it must not be cut off)'
            $said = $true
        }
        Start-Sleep -Milliseconds 500
    }
}

# The WheelBri Helper: a Windows service (LocalSystem) running wheelbri-helper.exe from the install folder, which does
# the steps needing Administrator for the app, so the app runs as a standard user. Stopping it removes the virtual
# wheel it made (G25, G27, G920 or G923) and shows the rig again (it undoes what every connected app set up).
$script:HelperService = 'WheelBriHelper'

# Stops the helper service if there is one (and waits up to 20 s). Returns $true when it existed.
function Stop-HelperService {
    $service = Get-Service -Name $script:HelperService -ErrorAction SilentlyContinue
    if (-not $service) { return $false }
    if ($service.Status -ne 'Stopped') {
        Stop-Service -Name $script:HelperService -Force -ErrorAction SilentlyContinue
        try { $service.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(20)) } catch { }
    }
    # Its process may take a moment longer to go; the program file stays locked until it has.
    $deadline = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $deadline -and @(Get-Process -Name 'wheelbri-helper' -ErrorAction SilentlyContinue |
               Where-Object { $_.SessionId -eq 0 }).Count -gt 0) {
        Start-Sleep -Milliseconds 250
    }
    return $true
}

# Stops and deletes the helper service. Returns $true when it is gone (or never was).
function Remove-HelperService {
    if (-not (Stop-HelperService)) { return $true }
    sc.exe delete $script:HelperService | Out-Null
    # 1060: no such service; 1072: already marked for deletion (Windows deletes it when the last handle closes).
    return $LASTEXITCODE -in @(0, 1060, 1072)
}

# Registers (or updates) the helper service for $exe, starts with Windows, restarts after a crash, and starts it now.
# Returns '' when it runs, else why not.
function Install-HelperService([string]$exe) {
    $binary = '"' + $exe + '"'   # quoted: the path has spaces (Program Files)
    $service = Get-Service -Name $script:HelperService -ErrorAction SilentlyContinue
    if ($service) {
        sc.exe config $script:HelperService binPath= $binary start= auto obj= LocalSystem | Out-Null
        if ($LASTEXITCODE -ne 0) { return "sc.exe config failed (exit code $LASTEXITCODE)" }
    } else {
        try {
            New-Service -Name $script:HelperService -BinaryPathName $binary -DisplayName 'WheelBri Helper' `
                -StartupType Automatic -ErrorAction Stop | Out-Null
        } catch {
            return "cannot register the service: $($_.Exception.Message)"
        }
    }
    sc.exe description $script:HelperService ('Lets WheelBri create its virtual wheel and hide your rig from games ' +
        'without running as Administrator.') | Out-Null
    # Restarted 5 s after a crash (twice), then after a minute; the count resets after a day.
    sc.exe failure $script:HelperService reset= 86400 actions= restart/5000/restart/5000/restart/60000 | Out-Null
    # Also when it stops with an error of its own (it reports one when its pipe cannot be served), not only a crash.
    sc.exe failureflag $script:HelperService 1 | Out-Null
    try {
        Start-Service -Name $script:HelperService -ErrorAction Stop
        (Get-Service -Name $script:HelperService).WaitForStatus('Running', [TimeSpan]::FromSeconds(15))
    } catch {
        return "the service did not start: $($_.Exception.Message)"
    }
    return ''
}

# Removes every installed copy of <name>.inf. Returns how many were removed.
function Remove-InstalledPackages([string]$name) {
    $packages = @(Get-InstalledPackages $name)
    if ($packages.Count -eq 0) {
        Write-Host "$name is not installed: nothing to remove."
        return 0
    }
    foreach ($package in $packages) {
        Write-Host "removing $package ($name)"
        Invoke-Pnputil @('/delete-driver', $package, '/uninstall', '/force') "removing $package" | Out-Null
    }
    return $packages.Count
}

# --- The install folder (install.ps1, uninstall.ps1, scripts/make/clean-pc.ps1) -----------------------------------
# installed-files.txt in the install folder lists, one relative path per line, every file the installer put there.
# Removing an install deletes those files and WheelBri's own subfolders, then the folders left empty: never a whole
# folder judged only by one file in it. An install from before the list existed is removed whole (the callers have
# checked that it holds wheelbri.exe). Nothing here follows a link (junction, symbolic link): a link is removed as a
# link, never entered, because what it points to could be anywhere on the PC (Windows PowerShell 5.1's
# Remove-Item -Recurse would delete what a directory link points to).
$script:InstallManifest = 'installed-files.txt'
$script:InstallSubfolders = @('config', 'driver', 'shim')   # only WheelBri's files are in these

function Test-Link($item) { return ([int]$item.Attributes -band 0x400) -ne 0 }   # FILE_ATTRIBUTE_REPARSE_POINT

# A manifest line that stays inside the install folder: relative, without a drive, "." or "..".
function Test-ManifestEntry([string]$entry) {
    try {
        if (-not $entry -or [IO.Path]::IsPathRooted($entry) -or $entry.Contains(':') -or $entry.Contains('/')) { return $false }
    } catch { return $false }   # characters a path cannot have
    return @($entry -split '\\' | Where-Object { $_ -in @('', '.', '..') }).Count -eq 0
}

# Whether <root>\<relative> is reached through no link (the last part included).
function Test-NoLinkBelow([string]$root, [string]$relative) {
    $path = $root
    foreach ($part in ($relative -split '\\')) {
        $path = Join-Path $path $part
        $item = Get-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
        if ($item -and (Test-Link $item)) { return $false }
    }
    return $true
}

# Deletes a file or a folder with everything in it, without following links. Errors are ignored: the caller checks
# what is left.
function Remove-Tree([string]$path) {
    $item = Get-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
    if (-not $item) { return }
    try {
        if ($item.PSIsContainer -and -not (Test-Link $item)) {
            foreach ($child in @(Get-ChildItem -LiteralPath $item.FullName -Force -ErrorAction SilentlyContinue)) {
                Remove-Tree $child.FullName
            }
            [IO.Directory]::Delete($item.FullName)
        } elseif ($item.PSIsContainer) {
            [IO.Directory]::Delete($item.FullName)   # a junction or directory link: the link only
        } else {
            if (-not (Test-Link $item) -and $item.IsReadOnly) { $item.IsReadOnly = $false }
            [IO.File]::Delete($item.FullName)        # a file link: the link only
        }
    } catch { }
}

# Deletes the folders under <folder> (and <folder>) that are empty, deepest first; links are left alone.
function Remove-EmptyFolders([string]$folder) {
    foreach ($child in @(Get-ChildItem -LiteralPath $folder -Directory -Force -ErrorAction SilentlyContinue)) {
        if (-not (Test-Link $child)) { Remove-EmptyFolders $child.FullName }
    }
    if (@(Get-ChildItem -LiteralPath $folder -Force -ErrorAction SilentlyContinue).Count -eq 0) {
        try { [IO.Directory]::Delete($folder) } catch { }
    }
}

# Removes an installation's files from <folder> (see above). Returns $true when the folder is gone. A folder that is
# a link itself is not touched (returns $false).
function Remove-InstallFolder([string]$folder) {
    $item = Get-Item -LiteralPath $folder -Force -ErrorAction SilentlyContinue
    if (-not $item) { return $true }
    if (Test-Link $item) {
        Write-Host "    $folder is a link (junction or symbolic link): not removed" -ForegroundColor Yellow
        return $false
    }
    $folder = $item.FullName.TrimEnd('\')
    $manifest = Join-Path $folder $script:InstallManifest
    if (Test-Path -LiteralPath $manifest -PathType Leaf) {
        foreach ($entry in @(Get-Content -LiteralPath $manifest)) {
            $entry = $entry.Trim()
            if (-not (Test-ManifestEntry $entry)) { continue }
            if (-not (Test-NoLinkBelow $folder $entry)) {
                Write-Host "    skipped $entry`: it is reached through a link" -ForegroundColor Yellow
                continue
            }
            $path = Join-Path $folder $entry
            if (Test-Path -LiteralPath $path -PathType Leaf) { Remove-Tree $path }
        }
        foreach ($subfolder in $script:InstallSubfolders) { Remove-Tree (Join-Path $folder $subfolder) }
        # The list goes last, and only with everything it names: a later install or uninstall still knows the rest.
        if (Test-Path -LiteralPath $manifest -PathType Leaf) {
            $left = @(Get-Content -LiteralPath $manifest | ForEach-Object { $_.Trim() } |
                      Where-Object { (Test-ManifestEntry $_) -and (Test-Path -LiteralPath (Join-Path $folder $_)) })
            if ($left.Count -eq 0) { Remove-Tree $manifest }
        }
        Remove-EmptyFolders $folder
    } else {
        Remove-Tree $folder
    }
    return -not (Test-Path -LiteralPath $folder)
}
