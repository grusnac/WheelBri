# Measures what WheelBri costs the PC, from outside: nothing is added to the app, so what it shows is the app's own
# use. Run it alongside a session (or several: mark each with M), then read the summary.
#
#   measure.ps1                         sample every second until Q (or Ctrl+C)
#   measure.ps1 -Seconds 300            stop by itself after 5 minutes
#   measure.ps1 -Phase "stopped"        the first phase's name (default "start")
#   measure.ps1 -Game NFS16             also measure the game (its process name, without .exe; several: comma-separated)
#   measure.ps1 -IntervalMs 500         sample twice a second
#   measure.ps1 -Top 12                 threads listed per phase in the summary (default 10)
#
# Keys while it runs (in a real console window; scripts/make/measure.sh opens one):
#   M  start a new phase: type its name ("virtual-no-game", "shim"...) and Enter; spaces become -, a name used before
#      gets -2, -3... so every phase has its own block in the summary
#   Q  stop and write the summary
#
# What it measures, every sample:
#   - each process: wheelbri.exe, wheelbri-service.exe, wheelbri-helper.exe, the driver hosts that load WheelBri's
#     drivers (WUDFHost.exe with wheelbri-wheel.dll = the virtual wheel, with wheelbri-hide.dll = the hide filter; one
#     host may carry both: "WUDFHost:wheel-driver+hide-filter", its threads tell them apart), the
#     game (-Game) and this script itself: CPU as Task Manager shows it (share of the whole PC) and as a share of one
#     core, memory (working set, private), handles, threads, GDI and USER objects;
#   - each thread of those processes: CPU (share of one core), priority, state and wait reason, name (when the
#     program gave its threads one);
#   - the PC: total CPU, and the timer resolution Windows uses now (WheelBri asks for 1 ms: it makes the whole PC wake
#     up more often).
# Run it as Administrator (measure.sh does): without it the helper's and the driver hosts' figures cannot be read.
#
# Output: one log file in WheelBri's logs folder, %LOCALAPPDATA%\wheelbri\logs\perf-<date>-<time>.log (housekeeping
# does not delete it), in the other logs' line format ("[date time] [info ] [component] message"):
#   [perf]          one line per process per sample: t_s, phase, process, pid, cpu_pc_pct, cpu_core_pct, working_set_mb,
#                   private_mb, handles, threads, gdi, user, pc_cpu_pct, timer_ms (key=value; spaces in names are _)
#   [perf.thread]   one line per thread per sample: t_s, phase, process, pid, tid, name, cpu_core_pct, priority, state,
#                   wait_reason
#   [perf.phase]    a line when a phase starts
#   [perf.summary]  at the end: per phase, each process (average / peak) and the busiest threads; also printed
#
# Exit codes: 0 done, 1 could not start (the red line says why).

param(
    [int]$Seconds = 0,
    [int]$IntervalMs = 1000,
    [string]$Phase = 'start',
    [string[]]$Game = @(),
    [int]$Top = 10,
    [string]$OutDir = ''
)
$ErrorActionPreference = 'Stop'

function Fail([string]$text) { Write-Host $text -ForegroundColor Red; exit 1 }

if ($IntervalMs -lt 200) { Fail '-IntervalMs must be 200 or more (sampling faster costs more than it shows).' }
if (-not $OutDir) { $OutDir = Join-Path $env:LOCALAPPDATA 'wheelbri\logs' }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$stamp = Get-Date -Format 'yyyy-MM-dd-HHmmss'
$logFile = Join-Path $OutDir "perf-$stamp.log"

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class WheelBriPerf {
    [DllImport("user32.dll")] public static extern uint GetGuiResources(IntPtr process, uint flags);
    [DllImport("ntdll.dll")] public static extern int NtQueryTimerResolution(out uint maximum, out uint minimum, out uint current);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern IntPtr OpenThread(uint access, bool inherit, uint id);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll")] public static extern int GetThreadDescription(IntPtr thread, out IntPtr description);
    [DllImport("kernel32.dll")] public static extern IntPtr LocalFree(IntPtr memory);
    [DllImport("kernel32.dll")] public static extern bool GetSystemTimes(out long idle, out long kernel, out long user);

    // The name a program gave a thread (SetThreadDescription), or "".
    public static string ThreadName(uint id) {
        IntPtr thread = OpenThread(0x0800 /* THREAD_QUERY_LIMITED_INFORMATION */, false, id);
        if (thread == IntPtr.Zero) return "";
        try {
            IntPtr text;
            if (GetThreadDescription(thread, out text) < 0 || text == IntPtr.Zero) return "";
            string name = Marshal.PtrToStringUni(text);
            LocalFree(text);
            return name ?? "";
        } catch (EntryPointNotFoundException) {
            return "";
        } finally {
            CloseHandle(thread);
        }
    }

    // The timer resolution in effect now, in milliseconds (15.6 when nothing asked for a finer one).
    public static double TimerResolutionMs() {
        uint maximum, minimum, current;
        return NtQueryTimerResolution(out maximum, out minimum, out current) == 0 ? current / 10000.0 : -1;
    }
}
'@

$elevated = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
$cores = [Environment]::ProcessorCount
$Game = @($Game | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })   # -File gives "a,b" as one
$names = @('wheelbri', 'wheelbri-service', 'wheelbri-helper') + @($Game | ForEach-Object { $_ -replace '\.exe$', '' })

# The driver hosts: WUDFHost.exe processes, labelled by which of WheelBri's drivers they load (needs Administrator).
# Windows may load both drivers into one host, and a driver into a host that is already running (seen 2026-10-06: the
# virtual wheel joined the hide filter's host when a session started), so the modules are looked at again every
# $hostRecheckSeconds. Hosts without them are left out.
$hostRecheckSeconds = 5
$hostLabels = @{}   # pid -> @{ Label; At }
function Get-HostLabel($process) {
    $known = $hostLabels[$process.Id]
    if ($known -and ((Get-Date) - $known.At).TotalSeconds -lt $hostRecheckSeconds) { return $known.Label }
    $label = ''
    try {
        $modules = @($process.Modules | ForEach-Object { $_.ModuleName.ToLower() })
        $parts = @()
        $wheel = $modules -contains 'wheelbri-wheel.dll' -or $modules -contains 'wheelbri-g920.dll'   # g920: old name
        if ($wheel) { $parts += 'wheel-driver' }
        if ($modules -contains 'wheelbri-hide.dll') { $parts += 'hide-filter' }
        if ($parts.Count -gt 0) { $label = 'WUDFHost:' + ($parts -join '+') }
    } catch { $label = '' }
    if ($known -and $known.Label -and $label -ne $known.Label) {
        $changed = New-Object System.Collections.Generic.List[string]
        $changed.Add((Stamp 'perf') + "driver host $($process.Id) is now $(if ($label) { $label } else { 'without WheelBri drivers' }) (was $($known.Label))")
        Write-LogLines $changed
    }
    $hostLabels[$process.Id] = @{ Label = $label; At = Get-Date }
    return $label
}

function Get-Watched {
    $list = @()
    foreach ($p in @(Get-Process -Name $names -ErrorAction SilentlyContinue)) {
        $list += [pscustomobject]@{ Process = $p; Label = "$($p.ProcessName).exe" -replace ' ', '_' }
    }
    foreach ($p in @(Get-Process -Name 'WUDFHost' -ErrorAction SilentlyContinue)) {
        $label = Get-HostLabel $p
        if ($label) { $list += [pscustomobject]@{ Process = $p; Label = $label } }
    }
    $self = Get-Process -Id $PID
    $list += [pscustomobject]@{ Process = $self; Label = 'measure.ps1' }
    return $list
}

# One line in the other logs' format: "[2026-10-06 13:20:15.442] [info ] [perf          ] message".
function Stamp([string]$component) {
    return '[{0}] [info ] [{1,-14}] ' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff'), $component
}
# key=value pairs from an ordered list of names and values; empty values are left out, spaces become _.
function Pairs([string[]]$keys, [object[]]$values) {
    $parts = for ($i = 0; $i -lt $keys.Count; $i++) {
        $text = "$($values[$i])"
        if ($text -ne '') { "$($keys[$i])=$($text -replace '\s', '_')" }
    }
    return $parts -join ' '
}
function Write-LogLines([System.Collections.Generic.List[string]]$lines) {
    if ($lines.Count -gt 0) { Add-Content -Path $logFile -Encoding ASCII -Value $lines }
}
$processKeys = @('t_s', 'phase', 'process', 'pid', 'cpu_pc_pct', 'cpu_core_pct', 'working_set_mb', 'private_mb',
                 'handles', 'threads', 'gdi', 'user', 'pc_cpu_pct', 'timer_ms')
$threadKeys = @('t_s', 'phase', 'process', 'pid', 'tid', 'name', 'cpu_core_pct', 'priority', 'state', 'wait_reason')

$startLines = New-Object System.Collections.Generic.List[string]
$startLines.Add((Stamp 'perf') + "WheelBri resource measurement started: $cores logical processors, every $IntervalMs ms" +
                $(if (-not $elevated) { ', NOT Administrator: the helper and the driver hosts may be missing' } else { '' }))
$Phase = $Phase.Trim() -replace '\s+', '-'
if (-not $Phase) { $Phase = 'start' }
$startLines.Add((Stamp 'perf.phase') + (Pairs @('phase') @($Phase)))
Write-LogLines $startLines

Write-Host "WheelBri resource measurement" -ForegroundColor Green
Write-Host "  every $IntervalMs ms; $cores logical processors; log: $logFile"
if (-not $elevated) {
    Write-Host '  not Administrator: the helper''s and the driver hosts'' figures cannot be read (measure.sh asks for it)' -ForegroundColor Yellow
}
$keys = $true
try { [void][Console]::KeyAvailable } catch { $keys = $false }
if ($keys) { Write-Host "  keys: M = new phase, Q = stop" } else { Write-Host '  no console keys here (Ctrl+C stops; -Seconds to stop by itself)' -ForegroundColor Yellow }
Write-Host "  phase: $Phase"

# A phase name used again gets -2, -3...: each phase keeps its own block in the summary (the same name three times was
# one block, printed three times).
function Get-UniquePhase([string]$name) {
    $name = $name.Trim() -replace '\s+', '-'
    $used = @($phases | ForEach-Object { $_.Name })
    if ($used -notcontains $name) { return $name }
    $n = 2
    while ($used -contains "$name-$n") { $n++ }
    return "$name-$n"
}

$previous = @{}        # pid -> @{ Cpu = ticks; Threads = @{ tid -> ticks }; At = ticks }
$threadNames = @{}     # "pid/tid" -> name
# A thread is often seen before its program names it (the app names its main thread once it has started): names still
# empty are read again every $nameRetrySeconds, so they show up once given.
$nameRetrySeconds = 5
$lastNameRetry = 0.0
$samples = New-Object System.Collections.Generic.List[object]
$threadSamples = New-Object System.Collections.Generic.List[object]
$phases = New-Object System.Collections.Generic.List[object]
$phases.Add([pscustomobject]@{ Name = $Phase; Start = Get-Date })
$clock = [Diagnostics.Stopwatch]::StartNew()
$systemPrevious = $null
$timerSeen = @{}
$deadline = if ($Seconds -gt 0) { (Get-Date).AddSeconds($Seconds) } else { [datetime]::MaxValue }
$quit = $false

try {
    while (-not $quit -and (Get-Date) -lt $deadline) {
        $nowTicks = $clock.Elapsed.Ticks
        $time = Get-Date -Format 'HH:mm:ss.fff'
        $t = [math]::Round($clock.Elapsed.TotalSeconds, 1)

        # The whole PC's CPU since the last sample.
        $idle = 0L; $kernel = 0L; $user = 0L
        [void][WheelBriPerf]::GetSystemTimes([ref]$idle, [ref]$kernel, [ref]$user)
        $pcCpu = ''
        if ($systemPrevious) {
            $busy = ($kernel - $systemPrevious.Kernel) + ($user - $systemPrevious.User)
            $all = $busy
            $busy -= ($idle - $systemPrevious.Idle)
            if ($all -gt 0) { $pcCpu = [math]::Round(100.0 * $busy / $all, 2) }
        }
        $systemPrevious = @{ Idle = $idle; Kernel = $kernel; User = $user }
        $timer = [math]::Round([WheelBriPerf]::TimerResolutionMs(), 3)
        $timerSeen[$timer] = $true

        $retryNames = ($clock.Elapsed.TotalSeconds - $lastNameRetry) -ge $nameRetrySeconds
        if ($retryNames) { $lastNameRetry = $clock.Elapsed.TotalSeconds }
        $processRows = @()
        $threadRows = @()
        $processLines = New-Object System.Collections.Generic.List[string]
        $threadLines = New-Object System.Collections.Generic.List[string]
        foreach ($w in Get-Watched) {
            $p = $w.Process
            try { $p.Refresh() } catch { continue }
            $cpu = $null
            try { $cpu = $p.TotalProcessorTime.Ticks } catch { }
            $last = $previous[$p.Id]
            $pcPct = ''; $corePct = ''
            if ($null -ne $cpu -and $last -and $nowTicks -gt $last.At) {
                $share = ($cpu - $last.Cpu) / [double]($nowTicks - $last.At)
                $corePct = [math]::Round(100.0 * $share, 2)
                $pcPct = [math]::Round(100.0 * $share / $cores, 2)
            }
            $gdi = ''; $usr = ''
            try { $gdi = [WheelBriPerf]::GetGuiResources($p.Handle, 0); $usr = [WheelBriPerf]::GetGuiResources($p.Handle, 1) } catch { }
            $ws = ''; $priv = ''
            try { $ws = [math]::Round($p.WorkingSet64 / 1MB, 1); $priv = [math]::Round($p.PrivateMemorySize64 / 1MB, 1) } catch { }
            $phaseName = $phases[$phases.Count - 1].Name
            $threadList = @()
            try { $threadList = @($p.Threads) } catch { }   # a protected process without Administrator
            $handles = ''
            try { $handles = $p.HandleCount } catch { }
            $row = [pscustomobject]@{ Phase = $phaseName; Process = $w.Label; Pid = $p.Id; PcPct = $pcPct; CorePct = $corePct
                                      WorkingSet = $ws; Private = $priv; Handles = $handles; Threads = $threadList.Count
                                      Gdi = $gdi; User = $usr }
            $processRows += $row
            $processLines.Add((Stamp 'perf') + (Pairs $processKeys @($t, $phaseName, $w.Label, $p.Id, $pcPct, $corePct,
                $ws, $priv, $handles, $threadList.Count, $gdi, $usr, $pcCpu, $timer)))

            # Its threads.
            $threadTicks = @{}
            foreach ($thread in $threadList) {
                $ticks = $null
                try { $ticks = $thread.TotalProcessorTime.Ticks } catch { continue }
                $threadTicks[$thread.Id] = $ticks
                $key = "$($p.Id)/$($thread.Id)"
                if (-not $threadNames.ContainsKey($key) -or ($retryNames -and -not $threadNames[$key])) {
                    $threadNames[$key] = [WheelBriPerf]::ThreadName([uint32]$thread.Id)
                }
                if (-not $last -or -not $last.Threads.ContainsKey($thread.Id) -or $nowTicks -le $last.At) { continue }
                $threadPct = [math]::Round(100.0 * ($ticks - $last.Threads[$thread.Id]) / [double]($nowTicks - $last.At), 2)
                $state = "$($thread.ThreadState)"
                $wait = ''
                if ($state -eq 'Wait') { try { $wait = "$($thread.WaitReason)" } catch { } }
                $priority = ''
                try { $priority = "$($thread.PriorityLevel)" } catch { try { $priority = "$($thread.CurrentPriority)" } catch { } }
                $threadRows += [pscustomobject]@{ Phase = $phaseName; Process = $w.Label; Pid = $p.Id; Tid = $thread.Id
                                                  Name = $threadNames[$key]; CorePct = $threadPct; Priority = $priority }
                $threadLines.Add((Stamp 'perf.thread') + (Pairs $threadKeys @($t, $phaseName, $w.Label, $p.Id, $thread.Id,
                    $threadNames[$key], $threadPct, $priority, $state, $wait)))
            }
            if ($null -ne $cpu) { $previous[$p.Id] = @{ Cpu = $cpu; Threads = $threadTicks; At = $nowTicks } }
        }
        # Written once a sample (opening the files for every row would cost this script more than it measures).
        $processLines.AddRange($threadLines)
        Write-LogLines $processLines
        foreach ($r in $processRows) { $samples.Add($r) }
        foreach ($r in $threadRows) { $threadSamples.Add($r) }

        # One line a sample: WheelBri's processes only.
        $line = ($processRows | Where-Object { $_.Process -ne 'measure.ps1' -and "$($_.PcPct)" -ne '' } |
                 ForEach-Object { "$($_.Process) $($_.PcPct)%" }) -join ' | '
        Write-Host ("{0} pc {1}% timer {2} ms  {3}" -f $time, $pcCpu, $timer, $(if ($line) { $line } else { 'no WheelBri process yet' }))

        # Keys, while waiting for the next sample.
        $next = $nowTicks + [TimeSpan]::FromMilliseconds($IntervalMs).Ticks
        while ($clock.Elapsed.Ticks -lt $next -and -not $quit) {
            if ($keys -and [Console]::KeyAvailable) {
                $key = [Console]::ReadKey($true).Key
                if ($key -eq 'Q') { $quit = $true }
                elseif ($key -eq 'M') {
                    $name = Read-Host 'new phase name'
                    if (-not $name) { $name = "phase $($phases.Count + 1)" }
                    $name = Get-UniquePhase $name
                    $phases.Add([pscustomobject]@{ Name = $name; Start = Get-Date })
                    $phaseLine = New-Object System.Collections.Generic.List[string]
                    $phaseLine.Add((Stamp 'perf.phase') + (Pairs @('phase') @($name)))
                    Write-LogLines $phaseLine
                    Write-Host "phase: $name" -ForegroundColor Cyan
                }
            }
            Start-Sleep -Milliseconds 50
        }
    }
} finally {
    # The summary, also after Ctrl+C.
    $out = New-Object System.Collections.Generic.List[string]
    $out.Add("WheelBri resource measurement $stamp ($cores logical processors, every $IntervalMs ms$(if (-not $elevated) { ', NOT Administrator: helper and driver hosts may be missing' }))")
    $out.Add('CPU "PC %" is what Task Manager shows (share of the whole PC); "core %" is the share of one core (100 = one core busy).')
    foreach ($part in $phases) {   # not $phase: that is the [string] parameter $Phase
        $rows = @($samples | Where-Object { $_.Phase -eq $part.Name -and "$($_.CorePct)" -ne '' })
        $out.Add('')
        $out.Add("== phase: $($part.Name) (from $($part.Start.ToString('HH:mm:ss')), $(@($rows | Select-Object -ExpandProperty Pid -Unique).Count) processes)")
        if ($rows.Count -eq 0) { $out.Add('   no samples'); continue }
        $out.Add(('   {0,-28} {1,15} {2,17} {3,16} {4,8} {5,8} {6,9}' -f 'process', 'PC % avg/peak', 'core % avg/peak',
                  'private MB avg/max', 'handles', 'threads', 'GDI/USER'))
        # One row per process: a driver host's label changes when a driver joins or leaves it (the virtual wheel comes
        # and goes with each session), so a process is named by the label it had most often in the phase.
        $labelOf = @{}
        foreach ($byPid in ($rows | Group-Object Pid)) {
            $labelOf[[string]$byPid.Name] = @($byPid.Group | Group-Object Process | Sort-Object Count -Descending)[0].Name
        }
        foreach ($group in ($rows | Group-Object Pid)) {
            $group | Add-Member -NotePropertyName Label -NotePropertyValue $labelOf[[string]$group.Name] -Force
            $core = @($group.Group | ForEach-Object { [double]$_.CorePct })
            $pc = @($group.Group | ForEach-Object { [double]$_.PcPct })
            $mem = @($group.Group | ForEach-Object { [double]$_.Private })
            $lastRow = $group.Group[$group.Group.Count - 1]
            $out.Add(('   {0,-28} {1,7:N2}/{2,7:N2} {3,8:N1}/{4,8:N1} {5,8:N1}/{6,7:N1} {7,8} {8,8} {9,9}' -f $group.Label,
                ($pc | Measure-Object -Average).Average, ($pc | Measure-Object -Maximum).Maximum,
                ($core | Measure-Object -Average).Average, ($core | Measure-Object -Maximum).Maximum,
                ($mem | Measure-Object -Average).Average, ($mem | Measure-Object -Maximum).Maximum,
                $lastRow.Handles, $lastRow.Threads, "$($lastRow.Gdi)/$($lastRow.User)"))
        }
        $threads = @($threadSamples | Where-Object { $_.Phase -eq $part.Name -and $_.Process -ne 'measure.ps1' })
        if ($threads.Count -gt 0) {
            $out.Add("   busiest threads (core %, 100 = one core):")
            $ranked = foreach ($group in ($threads | Group-Object Pid, Tid)) {
                $values = @($group.Group | ForEach-Object { [double]$_.CorePct })
                $first = $group.Group[0]
                $named = @($group.Group | Where-Object { $_.Name } | Select-Object -Last 1)
                $label = $labelOf[[string]$first.Pid]
                if (-not $label) { $label = $first.Process }
                [pscustomobject]@{ Process = $label; Tid = $first.Tid
                                   Name = $(if ($named.Count -gt 0) { $named[0].Name } else { '' }); Priority = $first.Priority
                                   Avg = ($values | Measure-Object -Average).Average; Peak = ($values | Measure-Object -Maximum).Maximum }
            }
            foreach ($r in @($ranked | Sort-Object Avg -Descending | Select-Object -First $Top)) {
                $out.Add(('     {0,-28} tid {1,-6} {2,-20} priority {3,-13} avg {4,6:N1}  peak {5,6:N1}' -f $r.Process, $r.Tid,
                    $(if ($r.Name) { $r.Name } else { '(no name)' }), $r.Priority, $r.Avg, $r.Peak))
            }
        }
    }
    $timers = @($timerSeen.Keys | Sort-Object)
    $out.Add('')
    $out.Add("timer resolution seen: $(($timers | ForEach-Object { "$_ ms" }) -join ', ') (WheelBri asks for 1 ms while it runs; 15.6 ms is Windows' default)")
    $summaryLines = New-Object System.Collections.Generic.List[string]
    foreach ($text in $out) { $summaryLines.Add((Stamp 'perf.summary') + $text) }
    Write-LogLines $summaryLines
    Write-Host ''
    $out | ForEach-Object { Write-Host $_ }
    Write-Host ''
    Write-Host "log: $logFile" -ForegroundColor Green
}
exit 0
