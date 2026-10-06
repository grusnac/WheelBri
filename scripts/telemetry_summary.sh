#!/usr/bin/env bash
# Summarise wheelbri telemetry (bash and awk only: MSYS2 UCRT64 on Windows, any Linux shell).
#
# usage: scripts/telemetry_summary.sh [FILE | FOLDER] [--all]
#
# FILE is a telemetry-YYYY-MM-DD.log (also .log.xz / .log.gz). With a FOLDER (default: the service's log folder) the
# newest day is used. By default the last session of that day is summarised; --all summarises every session of the
# day together. The matching ffb-*.log next to it is read too: the game's force feedback changes and the delay of
# every call the service made to the wheel (kind=wheel_* lines).
#
# The files are logs like the others: "[date time] [info ] [telemetry     ] t_ms=... key=value ...". Rows are the
# lines whose message starts with t_ms=; "session started" lines start a session.
#
# Rows are written only when a value changes (only_changes), so each row stands for the time until the next one;
# shares of time below are weighted by that.

set -euo pipefail

all=0
target=""
for arg in "$@"; do
    case "$arg" in
        --all) all=1 ;;
        -h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        --*) echo "unknown option: $arg" >&2; exit 2 ;;
        *) target="$arg" ;;
    esac
done

default_folder() {
    if [[ -n "${LOCALAPPDATA:-}" ]]; then
        local folder="$LOCALAPPDATA/wheelbri/logs"
        if command -v cygpath >/dev/null 2>&1; then folder="$(cygpath -u "$folder")"; fi
        echo "$folder"
    else
        echo "${XDG_STATE_HOME:-$HOME/.local/state}/wheelbri"
    fi
}

path="${target:-$(default_folder)}"
if [[ -d "$path" ]]; then
    newest="$(ls -1 "$path" 2>/dev/null | grep -E '^telemetry-.*\.log(\.xz|\.gz)?$' | sort | tail -n 1 || true)"
    if [[ -z "$newest" ]]; then
        echo "no telemetry-*.log in $path" >&2
        exit 1
    fi
    path="$path/$newest"
fi
if [[ ! -f "$path" ]]; then
    echo "no such file: $path" >&2
    exit 1
fi

# The file's text, whatever it is compressed with.
read_text() {
    case "$1" in
        *.xz) xz -dc -- "$1" ;;
        *.gz) gzip -dc -- "$1" ;;
        *) cat -- "$1" ;;
    esac
}

sessions_in() { read_text "$1" | grep -c '\] session started ' || true; }

# Shared awk: reading a line's key=value pairs, session selection, sorting and percentiles.
read -r -d '' AWK_COMMON <<'AWK' || true
# The message of a log line: what follows "[date time] [level] [component] ".
function message(line,    i) {
    for (i = 0; i < 3; i++) {
        if (!match(line, /\] /)) return ""
        line = substr(line, RSTART + 2)
    }
    return line
}
# Fills val[] from the row's key=value pairs; val["time"] = the line's time of day.
function parse(msg,    n, i, eq, parts) {
    for (i in val) delete val[i]
    val["time"] = substr($2, 1, length($2) - 1)
    n = split(msg, parts, " ")
    for (i = 1; i <= n; i++) {
        eq = index(parts[i], "=")
        if (eq > 1) val[substr(parts[i], 1, eq - 1)] = substr(parts[i], eq + 1)
    }
}
function v(name) { return (name in val) ? val[name] : "" }
# Sorts A[left..right] ascending: heapsort, no recursion (some awks limit its depth).
function qsort(A, left, right,    n, i, end_, t) {
    n = right - left + 1
    if (n < 2) return
    for (i = int(n / 2) - 1; i >= 0; i--) siftDown(A, left, i, n)
    for (end_ = n - 1; end_ > 0; end_--) {
        t = A[left]; A[left] = A[left + end_]; A[left + end_] = t
        siftDown(A, left, 0, end_)
    }
}
function siftDown(A, base, root, n,    child, t) {
    while ((child = 2 * root + 1) < n) {
        if (child + 1 < n && A[base + child] < A[base + child + 1]) child++
        if (A[base + root] >= A[base + child]) return
        t = A[base + root]; A[base + root] = A[base + child]; A[base + child] = t
        root = child
    }
}
# p-th percentile of A[1..n] (sorted in place): values[min(n-1, int(p/100*n))], 0-based.
function pct(A, n, p,    k) {
    if (n == 0) return "nan"
    qsort(A, 1, n)
    k = int(p / 100.0 * n); if (k > n - 1) k = n - 1
    return A[k + 1]
}
function median(A, n) {
    if (n == 0) return "nan"
    qsort(A, 1, n)
    return (n % 2) ? A[(n + 1) / 2] : (A[n / 2] + A[n / 2 + 1]) / 2
}
function abs(x) { return x < 0 ? -x : x }
# Line handling common to both files; returns 1 when the line is a row of a chosen session (its values in val[]).
function take(    msg) {
    sub(/\r$/, "")
    msg = message($0)
    if (msg ~ /^session started /) { session++; chosen = (all || session == last); if (chosen) comments[++nc] = msg; return 0 }
    if (msg !~ /^t_ms=/) { if (chosen && session > 0) comments[++nc] = msg; return 0 }
    if (session == 0) { session = 1; chosen = (all || session == last) }
    if (!chosen) return 0
    parse(msg)
    return 1
}
AWK

count="$(sessions_in "$path")"
echo "file     $(basename "$path") ($count session(s))"
[[ "$count" -gt 0 ]] || exit 0

read_text "$path" | awk -v all="$all" -v last="$count" "$AWK_COMMON"'
function heldFor(now, following) {   # seconds a row held: until the next row (capped at 60 s)
    held = (following != "" && following + 0 >= now + 0) ? (following - now) / 1000.0 : 0.02
    return held > 60.0 ? 60.0 : held
}
# Adds the previous row, now that its weight is known.
function settle(w,    s) {
    total += w
    if (pThr > 0.05) wThr += w
    if (pBrk > 0.05) wBrk += w
    if (pClu > 0.05) wClu += w
    if (pThr > 0.05 && pBrk > 0.05) wBoth += w
    if (pWatch == "1") wWatch += w
    for (s = 0; s < 8; s++) if (pType[s] != "") typeW[pType[s]] += w
}
take() {
    rows++
    t = v("t_ms")
    if (rows > 1) settle(heldFor(prevT, t))
    if (rows == 1) firstTime = v("time")
    lastTime = v("time")
    for (i = 0; i < 8; i++) {
        a = v("raw_a" i) + 0
        if (rows == 1 || a < amin[i]) amin[i] = a
        if (rows == 1 || a > amax[i]) amax[i] = a
    }
    # Button presses: compared hex digit by hex digit (awk numbers cannot hold 64 bits).
    hex = tolower(v("raw_buttons")); gsub(/^0x/, "", hex)
    nd = length(hex)
    for (d = 0; d < 16; d++) {
        digit = (d < nd) ? index("0123456789abcdef", substr(hex, nd - d, 1)) - 1 : 0
        if (digit < 0) digit = 0
        for (b = 0; b < 4; b++) {
            now = int(digit / (2 ^ b)) % 2
            before = int(prevDigit[d] / (2 ^ b)) % 2
            if (now && !before) presses[d * 4 + b]++
        }
        prevDigit[d] = digit
    }
    pThr = v("throttle") + 0; pBrk = v("brake") + 0; pClu = v("clutch") + 0
    if (rows == 1 || pThr > maxThr) maxThr = pThr
    if (rows == 1 || pBrk > maxBrk) maxBrk = pBrk
    if (rows == 1 || pClu > maxClu) maxClu = pClu
    st = v("steer") + 0
    if (rows == 1 || st < steerMin) steerMin = st
    if (rows == 1 || st > steerMax) steerMax = st
    split("out_steer out_throttle out_brake out_clutch", outs, " ")
    for (k = 1; k <= 4; k++) {
        o = v(outs[k]) + 0
        if (rows == 1 || o < omin[k]) omin[k] = o
        if (rows == 1 || o > omax[k]) omax[k] = o
    }
    ffbPackets = v("ffb_packets"); ffbPlays = v("ffb_plays"); ffbFailures = v("ffb_failures")
    if (v("ffb_age_ms") != "") ages[++nAges] = v("ffb_age_ms") + 0
    pWatch = v("ffb_watchdog")
    for (s = 0; s < 8; s++) {
        kind = v("s" s "_type"); state = v("s" s "_state")
        pType[s] = (kind != "" && index(state, "p") > 0) ? kind : ""
        if (pType[s] != "") {
            types[kind] = 1
            r = abs(v("s" s "_req") + 0)
            req[kind, ++nReq[kind]] = r
            if (v("s" s "_out") != "") { out[kind, ++nOut[kind]] = abs(v("s" s "_out") + 0) }
        }
    }
    if (rows > 1) { periods[++nPeriods] = v("period_us") + 0; work = v("work_us") + 0; if (work > maxWork) maxWork = work }
    prevT = t
}
END {
    if (rows > 0) settle(0.02)
    for (i = 1; i <= nc; i++) {
        c = comments[i]
        if (c ~ /^(session started|wheelbri-service|source:|target_profile:|config:)/) print "         " c
    }
    if (rows == 0) { print "no rows"; exit }
    if (total == 0) total = 1.0
    printf "rows     %d, %s .. %s (%.0f s)\n", rows, firstTime, lastTime, total
    for (i = 1; i <= nc; i++) if (comments[i] ~ /^stopped: max_file_mb/) { print "         NOTE: the file hit max_file_mb, recording stopped early"; break }

    print "\nphysical axes (raw -1..1): min / max  -> moved?"
    for (i = 0; i < 8; i++) printf "  a%d  %+.3f / %+.3f  %s\n", i, amin[i], amax[i], (amax[i] - amin[i] > 0.05) ? "moved" : ""

    line = ""
    for (b = 0; b < 64; b++) if (presses[b] > 0) line = line (line == "" ? "" : ", ") b "x" presses[b]
    print "\nphysical button presses (sampled): " (line == "" ? "none" : line)

    print "\nmapped pedals: share of time pressed (>5%) / max"
    printf "  %-8s %5.1f%%  max %.3f\n", "throttle", wThr / total * 100, maxThr
    printf "  %-8s %5.1f%%  max %.3f\n", "brake", wBrk / total * 100, maxBrk
    printf "  %-8s %5.1f%%  max %.3f\n", "clutch", wClu / total * 100, maxClu
    printf "  throttle and brake together: %.1f%%\n", wBoth / total * 100
    printf "  steer    %+.3f .. %+.3f\n", steerMin, steerMax

    print "\nsent to the game (emulated wheel units): min / max"
    for (k = 1; k <= 4; k++) printf "  %-12s %7.0f / %7.0f\n", outs[k], omin[k], omax[k]

    if (ffbPackets == "") {
        print "\nforce feedback: off"
    } else {
        printf "\nforce feedback: %s packets, %s wheel updates, %s failed (last session'"'"'s counters)\n", ffbPackets, ffbPlays, ffbFailures
        if (nAges > 0) { mx = 0; for (i = 1; i <= nAges; i++) if (ages[i] > mx) mx = ages[i]; printf "  packet age: median %.0f ms, max %.0f ms\n", median(ages, nAges), mx }
        printf "  watchdog tripped in %.1f%% of rows (game quiet > 500 ms: menus, pause, alt-tab)\n", wWatch / total * 100
        print "  effect      playing  |req| p95 / max   |out| p95 / max"
        n = 0; for (k in types) names[++n] = k
        qsort(names, 1, n)
        for (i = 1; i <= n; i++) {
            k = names[i]
            m = nReq[k]; mx = 0; for (j = 1; j <= m; j++) { R[j] = req[k, j]; if (R[j] > mx) mx = R[j] }
            p95 = pct(R, m, 95)
            mo = nOut[k] + 0; omx = 0; for (j = 1; j <= mo; j++) { O[j] = out[k, j]; if (O[j] > omx) omx = O[j] }
            op95 = pct(O, mo, 95)
            printf "  %-10s %6.1f%%   %.2f / %.2f      %s / %.2f\n", k, typeW[k] / total * 100, p95, mx, (mo > 0 ? sprintf("%.2f", op95) : "nan"), omx
        }
    }
    if (nPeriods > 0) {
        mx = 0; for (i = 1; i <= nPeriods; i++) { P[i] = periods[i]; if (P[i] > mx) mx = P[i] }
        p99 = pct(P, nPeriods, 99)
        printf "\nloop period us: median %.0f, p99 %.0f, max %.0f; work max %.0f us\n", median(P, nPeriods), p99, mx, maxWork
    }
}'

dir="$(dirname "$path")"
base="$(basename "$path")"
ffb="$dir/${base/telemetry-/ffb-}"
[[ -f "$ffb" ]] || exit 0
ffbCount="$(sessions_in "$ffb")"
[[ "$ffbCount" -gt 0 ]] || exit 0

read_text "$ffb" | awk -v all="$all" -v last="$ffbCount" "$AWK_COMMON"'
function group(kind, type) {
    if (kind != "wheel_play") return "stops"
    if (type == "constant") return "constant"
    if (type == "spring" || type == "damper" || type == "friction" || type == "inertia") return "spring etc."
    return "periodic"
}
take() {
    kind = v("kind")
    if (kind ~ /^wheel_/) {
        if (v("wait_us") == "" || v("call_us") == "") next
        g = group(kind, v("type"))
        n = ++calls[g]
        waits[g, n] = v("wait_us") / 1000.0
        wheel[g, n] = v("call_us") / 1000.0
        anyCalls = 1
        next
    }
    events++
    key = (kind == "slot") ? "slot:" v("type") : kind
    if (!(key in counts)) keys[++nKeys] = key
    counts[key]++
}
END {
    # Most common first.
    for (i = 1; i <= nKeys; i++) for (j = i + 1; j <= nKeys; j++) if (counts[keys[j]] > counts[keys[i]]) { t = keys[i]; keys[i] = keys[j]; keys[j] = t }
    line = ""
    for (i = 1; i <= nKeys; i++) line = line (i > 1 ? ", " : "") keys[i] " " counts[keys[i]]
    printf "\nforce feedback changes from the game: %d  %s\n", events, line
    if (!anyCalls) exit
    print "\ndelay to the wheel (ms): wait = queued before the call, wheel = the wheel'"'"'s driver, total = both"
    printf "  %-12s %6s  wait avg/max   wheel avg/max   total p50 / p95 / max\n", "group", "calls"
    split("constant|periodic|spring etc.|stops", order, "|")
    for (o = 1; o <= 4; o++) {
        g = order[o]; n = calls[g] + 0
        if (n == 0) continue
        ws = 0; wm = 0; cs = 0; cm = 0; tm = 0
        for (i = 1; i <= n; i++) {
            w = waits[g, i]; c = wheel[g, i]
            ws += w; cs += c
            if (w > wm) wm = w
            if (c > cm) cm = c
            T[i] = w + c
            if (T[i] > tm) tm = T[i]
        }
        p50 = pct(T, n, 50); p95 = pct(T, n, 95)
        printf "  %-12s %6d  %5.2f/%5.1f   %5.1f/%5.1f     %5.1f / %5.1f / %5.1f\n", g, n, ws / n, wm, cs / n, cm, p50, p95, tm
        for (i = 1; i <= n; i++) delete T[i]
    }
}'
