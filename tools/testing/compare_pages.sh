#!/usr/bin/env bash
#
# compare_pages.sh - run a whole page set past two browsers and tabulate it.
#
#   compare_pages.sh <plan> [--keep] [--clusters N] [--css RULES]
#                            [--accept FILE]...
#
# The plan is the comparison, written down. Everything a sweep needs is in it:
#
#     size 2200 1150
#     scale 1.25
#     side linux x11::99          127.0.0.1  2828 http://127.0.0.1:8080
#     side win   libvirt:<domain> <guest-ip> 2929 http://<host-ip>:8080 cdp
#     accept widget-chrome.accept
#     page example/index.html     0
#     page example-long/index.html 0 1000 4600 8800
#
# parity.plan.example beside this script is a filled-in skeleton to copy.
#
#   size    the inner width and height both browsers are held at, in CSS
#           pixels, which is what the window is sized in
#   scheme  light or dark. Both sides are asked what they report and a side
#           that disagrees stops the sweep. The scheme is set when a browser
#           starts, so without this a plan carries no record of which one a
#           run measured. Set it with run_parity_firefox.sh --scheme.
#   scale   device pixels per CSS pixel, default 1. Both sides are held to it
#           and each is asked what it is actually at, so a side whose pref
#           never landed stops the sweep instead of contributing the scale as
#           a difference. Captures and crops are this many times `size`.
#   side    a label, a capture backend (see capture_viewport.sh), the host
#           and port its browser listens on (Marionette or DevTools, per the
#           driver), and the URL prefix *that machine* reaches the page
#           server by, which is not the same string on both when one of them
#           is a guest
#   page    a path under both prefixes, then one or more scroll offsets
#   accept  a file of page-path globs whose difference is known not to be the
#           shim's, one per line with # comments, repeatable. Those cells are
#           still captured, compared and printed, marked `accepted`, and are
#           kept out of the worst-channel figure and the wedge heuristics.
#           Nothing is dropped: a cell listed there that changes is still
#           visible. --accept on the command line adds a file to whatever the
#           plan names. A relative path is looked for beside the plan first,
#           then beside this script.
#
# A side's sixth field is its browser driver, `marionette` for Firefox or
# `cdp` for Chromium and Electron, and it defaults to marionette.
#
# Either kind of side may list several ports comma-separated, one browser
# instance each; the cells are dealt out across them and a port that does not
# answer is dropped with a warning. A Marionette side is photographed from its
# screen, so each of its instances needs a screen of its own. List one backend
# per port, comma-separated in the same order, or name a guest as
# guest:<host>, and each instance's capture server is found on its own port
# plus one, which is where run_parity_firefox.sh guest puts it.
#
# When both sides are cdp the sweep takes the direct route, one DevTools
# connection per browser for the whole plan and one Page.captureScreenshot
# per cell. The shot is the viewport itself, so there is no marker, no
# whole-screen photograph and no crop. Start extra cdp instances with
# run_electron_side.sh under their own DWC_ELECTRON_STATE.
#
# When any cell differs, a summary after the totals groups the differing
# clusters whose bounding boxes nearly coincide across cells, since many
# cells sharing one box is a single defect. --clusters N additionally
# prints up to N clusters under each differing cell as it lands.
#
# --css puts the same extra rule sheet on both sides, which is how one
# suspected cause is priced. `font-kerning: none` says what the two sides'
# kerning is worth; `font-weight: 400` says what their bold is.
#
# The two sides are captured at the same time, because they have nothing to
# say to each other, and waiting for the first to finish before starting the
# second doubles a sweep for no reason.
#
# Start a Firefox side with run_parity_firefox.sh, which also warms it, since
# a cold Firefox reflows its first page when the font list finishes building
# off the main thread and a capture taken during that is a good screenshot of
# the wrong layout. Start a Chromium or Electron side with run_electron_side.sh.
set -u

# An inherited LD_PRELOAD would make the control run not a control.
unset LD_PRELOAD

usage() { sed -n '3,47p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-2}"; }

[ $# -ge 1 ] || usage
case "$1" in -h|--help) usage 0 ;; esac

PLAN="$1"; shift
KEEP=0
CLUSTERS=""
CSS=()
ACCEPT_FILES=()
while [ $# -gt 0 ]; do
    case "$1" in
        --keep)     KEEP=1; shift ;;
        --clusters) CLUSTERS="--clusters $2"; shift 2 ;;
        --css)      CSS=(--css "$2"); shift 2 ;;
        --accept)   ACCEPT_FILES+=("$2"); shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
[ -f "$PLAN" ] || { echo "no such plan: $PLAN" >&2; exit 2; }

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SHOTS="$(mktemp -d "${TMPDIR:-/tmp}/dwc-pages-XXXXXX")"
[ "$KEEP" = 1 ] || trap 'rm -rf "$SHOTS"' EXIT

WIDTH=""; HEIGHT=""; SCALE=1; SCHEME=""
LABELS=(); BACKENDS=(); HOSTS=(); PORTS=(); PREFIXES=(); DRIVERS=()
PAGES=()

while read -r kind rest; do
    case "${kind:-}" in
        ""|\#*) continue ;;
        size) WIDTH="${rest%% *}"; HEIGHT="${rest##* }" ;;
        scale) SCALE="$rest" ;;
        scheme) SCHEME="$rest" ;;
        side)
            set -- $rest
            LABELS+=("$1"); BACKENDS+=("$2"); HOSTS+=("$3"); PORTS+=("$4"); PREFIXES+=("$5")
            DRIVERS+=("${6:-marionette}") ;;
        page) PAGES+=("$rest") ;;
        accept) ACCEPT_FILES+=("$rest") ;;
        *) echo "unknown plan line: $kind $rest" >&2; exit 2 ;;
    esac
done < "$PLAN"

if [ -z "$WIDTH" ] || [ "${#LABELS[@]}" -ne 2 ] || [ "${#PAGES[@]}" -eq 0 ]; then
    echo "the plan needs a size, exactly two sides and at least one page" >&2
    exit 2
fi

# A side left running across a rebuild answers every request and measures the
# build it started with, so the sweep reads as a change that did nothing or as
# a regression that is not there. Checked from the process holding the port,
# which is the only way to name the right process: `pgrep -f firefox` also
# matches this script.
#
# Two tells. The mapped file being gone is what a rebuild over a live mapping
# leaves behind, and it survives the path still looking correct. A different
# inode at the same path is the same thing when the build wrote a new file.
# Neither can see the guest, which has no shim of its own to compare; a guest
# that has drifted shows up as the wedge warn_if_wedged reports instead.
check_side_shim() {
    local host="$1" port="$2" label="$3" pid line path mapped now
    case "$host" in
        127.0.0.1|localhost|::1) ;;
        *) return 0 ;;
    esac
    pid="$(ss -ltnp 2>/dev/null | awk -v p=":$port\$" '$4 ~ p {print $NF}' \
           | grep -o 'pid=[0-9]*' | head -1 | cut -d= -f2)"
    [ -n "$pid" ] || return 0
    line="$(grep -m1 libcleartype "/proc/$pid/maps" 2>/dev/null)" || return 0
    [ -n "$line" ] || return 0
    case "$line" in
        *"(deleted)"*)
            echo "side $label on port $port maps a libcleartype.so that has been rebuilt since it started, so this sweep would measure the old build; restart that side" >&2
            return 1 ;;
    esac
    path="$(printf '%s' "$line" | sed -n 's|^[^/]*\(/.*\)$|\1|p')"
    mapped="$(printf '%s' "$line" | awk '{print $5}')"
    now="$(stat -c %i "$path" 2>/dev/null)" || return 0
    if [ -n "$now" ] && [ -n "$mapped" ] && [ "$now" != "$mapped" ]; then
        echo "side $label on port $port maps inode $mapped of $path but that path is inode $now now, so this sweep would measure the old build; restart that side" >&2
        return 1
    fi
    return 0
}

STALE=0
for i in 0 1; do
    for port in ${PORTS[$i]//,/ }; do
        check_side_shim "${HOSTS[$i]}" "$port" "${LABELS[$i]}" || STALE=1
    done
done
[ "$STALE" = 0 ] || exit 2

# The two sides have to be the same browser build. A machine with more than
# one installed hands whichever one the launcher found first, and a sweep
# across two versions measures the difference between them as though it were
# the shim's. Each driver is asked its version before anything is captured.
#
# Firefox is asked for Services.appinfo.version and not its build id, since
# the two sides are the Linux and Windows builds of one release and those
# always carry different build ids.
MARIONETTE_VERSION='import sys
sys.path.insert(0, sys.argv[3])
from marionette import Marionette
with Marionette(sys.argv[1], int(sys.argv[2]), timeout=20) as m:
    m.start()
    print(m.script("return Services.appinfo.version;", sandbox="system"))'

side_browser() {                          # side_browser <driver> <host> <port>
    case "$1" in
        cdp)
            curl -s --max-time 5 "http://$2:$3/json/version" 2>/dev/null |
                sed -n 's/.*"Browser": *"\([^"]*\)".*/\1/p' | head -1
            ;;
        marionette)
            python3 -c "$MARIONETTE_VERSION" "$2" "$3" "$HERE" 2>/dev/null
            ;;
    esac
}

# Every shard of both sides, not just the first port of each. A side lists one
# port per browser instance, and reading only the first left a second shard
# started on another build to pass, which measures two builds against each
# other in the cells that landed on it. A port that does not answer is skipped
# here; the sweep drops it below.
if [ "${DRIVERS[0]}" = "${DRIVERS[1]}" ]; then
    build=""; built_by=""
    for i in 0 1; do
        for port in ${PORTS[$i]//,/ }; do
            v="$(side_browser "${DRIVERS[$i]}" "${HOSTS[$i]}" "$port")"
            [ -n "$v" ] || continue
            if [ -z "$build" ]; then
                build="$v"; built_by="side ${LABELS[$i]} port $port"
            elif [ "$v" != "$build" ]; then
                echo "$built_by is $build and side ${LABELS[$i]} port $port is $v, so this sweep would measure the difference between two browser builds; start every shard of both sides on the same one" >&2
                exit 2
            fi
        done
    done
fi

printf '%-38s %8s  %10s  %8s\n' page scrollY identical "max diff"
printf '%-38s %8s  %10s  %8s\n' "-------------------------------------" ------- ---------- --------

STARTED=$(date +%s)
WORST=0
EXACT=0
# Page paths whose difference no font decision reaches. Those cells are still
# captured, compared and printed, marked, and counted on their own line; they
# stay out of the worst-channel figure and the wedge heuristics. A pattern is
# a shell glob against the page path as the plan writes it, and a relative
# accept file is looked for beside the plan first, then beside this script.
ACCEPT_PATTERNS=()
for file in ${ACCEPT_FILES+"${ACCEPT_FILES[@]}"}; do
    found="$file"
    [ -f "$found" ] || found="$(dirname "$PLAN")/$file"
    [ -f "$found" ] || found="$HERE/$file"
    [ -f "$found" ] || { echo "no such accept file: $file" >&2; exit 2; }
    while read -r pattern; do
        case "${pattern:-}" in ""|\#*) continue ;; esac
        ACCEPT_PATTERNS+=("$pattern")
    done < "$found"
done

is_accepted() {
    local path="$1" pattern
    for pattern in ${ACCEPT_PATTERNS+"${ACCEPT_PATTERNS[@]}"}; do
        case "$path" in $pattern) return 0 ;; esac
    done
    return 1
}

# Comparisons running at once. One cell is independent of every other, so
# this is what puts the machine's cores on the slowest part of a sweep.
COMPARE_WORKERS="${DWC_COMPARE_WORKERS:-16}"

# How long one cell may go undelivered while some sweeper is still alive. A
# slow page on a loaded machine is ordinary, so only a side that has stopped
# delivering should reach it.
STALL_SECONDS=180
STALL_TICKS=$((STALL_SECONDS * 20))

FAILED=0
CELLS=0
ACCEPTED=0
STALLED=0
STRIKES=0

# The longest run of consecutive cells that came out unrecognizable, and where
# it started and ended in plan order.
#
# A cell that captured fine and then compared at a third of its pixels is not a
# font difference. Nothing about a font makes consecutive *filenames* fail
# together, so a long run of them in plan order is one side wedging and
# recovering, which the two-strikes rule below cannot see because every capture
# succeeded. Seen on css-writing-modes as 75 cells reading 29% identical, where
# a rerun of the same plan on the same browsers read them all exact: 625 of 765
# against 724 of 765.
BAD_FLOOR=50
BAD_RUN=0; BAD_FROM=0
LONGEST_BAD=0; LONGEST_FROM=0; LONGEST_TO=0
BAD_TOTAL=0

note_divergence() {
    local pct="$1" n="$2" whole
    whole="${pct%%.*}"
    if [ -n "$pct" ] && [ "$pct" != "?" ] && [ "${whole:-100}" -lt "$BAD_FLOOR" ]; then
        BAD_TOTAL=$((BAD_TOTAL + 1))
        [ "$BAD_RUN" -eq 0 ] && BAD_FROM="$n"
        BAD_RUN=$((BAD_RUN + 1))
        if [ "$BAD_RUN" -gt "$LONGEST_BAD" ]; then
            LONGEST_BAD="$BAD_RUN"; LONGEST_FROM="$BAD_FROM"; LONGEST_TO="$n"
        fi
    else
        BAD_RUN=0
    fi
}

# Printed with the totals.
#
# Two shapes, both of which have cost a day. A long run of unrecognizable cells
# with the rest of the sweep fine is one side wedging partway through and
# recovering. A sweep where nothing at all matches is nearly always a browser
# that was already wedged when it started, since a change to glyph handling
# moves some pixels on some pages, not every pixel on every page. The second
# one cannot be told from a real regression inside a single run, which is the
# trap: the fix is to restart both sides and run it again, not to go read the
# diff. The shim check above will not catch either, because a wedged browser
# maps exactly the right file.
summarize_clusters() {
    [ -s "$SHOTS/clusters.tsv" ] || return 0
    echo
    python3 "$HERE/compare_viewport.py" --aggregate "$SHOTS/clusters.tsv"
}

warn_if_wedged() {
    # Most of them have to be unrecognizable as well, not merely inexact. A
    # plan of probe pages that are all a little bit off is the ordinary state
    # of an open front, and it has no exact cell either; a wedged browser
    # leaves the pages unreadable.
    if [ "$EXACT" = 0 ] && [ "$CELLS" -ge 3 ] && [ $((BAD_TOTAL * 2)) -gt "$CELLS" ]; then
        printf 'warning: not one of %d cells matched and %d of them came out below %d%% identical. That is more often a browser that was already wedged than a real change, and a restart of both sides costs less than reading the diff. Re-run before believing this.\n' \
               "$CELLS" "$BAD_TOTAL" "$BAD_FLOOR" >&2
        return 0
    fi
    [ "$LONGEST_BAD" -ge 10 ] || return 0
    [ "$EXACT" -gt "$LONGEST_BAD" ] || return 0
    printf 'warning: cells %d-%d came out unrecognizable, %d in a row, and the rest of the sweep did not. That is one side wedging rather than a difference. Re-run the plan before trusting this number.\n' \
           "$LONGEST_FROM" "$LONGEST_TO" "$LONGEST_BAD" >&2
}

# Both drivers have a direct route. Each side sweeps the whole plan over one
# connection, in one process, and cells are compared here as soon as both sides
# have delivered them, overlapping with the captures still going.
#
# A CDP side reads the compositor with Page.captureScreenshot. A Marionette
# side still photographs the screen, because Firefox has no faithful in-browser
DEV_W="$(python3 -c "print(round($WIDTH * $SCALE))")"
DEV_H="$(python3 -c "print(round($HEIGHT * $SCALE))")"

# capture; sweep_pages_marionette.py opens with why. It grabs through Xlib into
# memory instead of shelling out to `import`, and holds one process for the
# plan instead of one per page. python-xlib is what that needs, and a
# Marionette side has no other way to reach a screen.
for i in 0 1; do
    [ "${DRIVERS[$i]}" = cdp ] && continue
    python3 -c "import Xlib" 2>/dev/null && continue
    echo "side ${LABELS[$i]} is photographed from its screen and python-xlib is not installed, so there is nothing to read the screen with" >&2
    exit 1
done

ALL_PATHS=(); ALL_SCROLLS=()
: > "$SHOTS/cells"
for entry in "${PAGES[@]}"; do
    set -- $entry
    path="$1"; shift
    for scroll in "$@"; do
        CELLS=$((CELLS + 1))
        ALL_PATHS+=("$path"); ALL_SCROLLS+=("$scroll")
        printf '%d %s %s\n' "$CELLS" "$path" "$scroll" >> "$SHOTS/cells"
    done
done

# A side may list several ports, comma-separated, one browser instance
# each. The cells are dealt out round-robin so the heavy pages, which sit
# together in the plan, spread across the instances. Ports that do not
# answer are dropped, so a plan naming a second instance still works with
# only the first one running.
SWEEPERS=()
# Both sweepers hand their frames to the comparison over this. Nothing writes
# a capture.
FRAMES="$SHOTS/frames.sock"
WANT_CLUSTERS=0
[ -n "$CLUSTERS" ] && WANT_CLUSTERS="${CLUSTERS##* }"
for i in 0 1; do
    # The backend field may list one backend per port, in the same order,
    # for a Marionette side whose instances each need their own screen; a
    # single backend serves every port.
    read -ra backends <<< "${BACKENDS[$i]//,/ }"
    live=(); livebk=(); j=0
    for port in ${PORTS[$i]//,/ }; do
        # DevTools answers an HTTP probe; Marionette is a bare socket.
        if [ "${DRIVERS[$i]}" = cdp ]; then
            probe() { curl -s --max-time 3 "http://${HOSTS[$i]}:$port/json/version" >/dev/null 2>&1; }
        else
            probe() { (exec 3<>"/dev/tcp/${HOSTS[$i]}/$port") 2>/dev/null; }
        fi
        if probe; then
            live+=("$port")
            livebk+=("${backends[$j]:-${backends[0]}}")
        else
            echo "side ${LABELS[$i]}: nothing answers on port $port; sweeping without it" >&2
        fi
        j=$((j + 1))
    done
    if [ "${#live[@]}" -eq 0 ]; then
        echo "side ${LABELS[$i]}: no port answers" >&2
        exit 1
    fi
    for k in "${!live[@]}"; do
        awk -v n="${#live[@]}" -v k="$k" 'NR % n == (k + 1) % n' \
            "$SHOTS/cells" > "$SHOTS/cells.${LABELS[$i]}.$k"
        # The Marionette sweeper takes the side's screen backend first;
        # everything after that is the same argument list.
        if [ "${DRIVERS[$i]}" = cdp ]; then
            SWEEP=("$HERE/sweep_pages_cdp.py")
        else
            SWEEP=("$HERE/sweep_pages_marionette.py" "${livebk[$k]}")
        fi
        FRAME_ARGS=(--frames "$FRAMES"
                    --labels "${LABELS[0]},${LABELS[1]}"
                    --clusters "$WANT_CLUSTERS")
        python3 "${SWEEP[@]}" "${HOSTS[$i]}" "${live[$k]}" \
                "${PREFIXES[$i]}" "$WIDTH" "$HEIGHT" "${LABELS[$i]}" \
                --scale "$SCALE" ${SCHEME:+--scheme "$SCHEME"} \
                "${FRAME_ARGS[@]+"${FRAME_ARGS[@]}"}" \
                "$SHOTS" "$SHOTS/cells.${LABELS[$i]}.$k" "${CSS[@]+"${CSS[@]}"}" \
                >"$SHOTS/${LABELS[$i]}.$k.log" 2>&1 &
        SWEEPERS+=($!)
    done
done
trap 'kill "${SWEEPERS[@]}" 2>/dev/null; [ "$KEEP" = 1 ] || rm -rf "$SHOTS"' EXIT

# Both sides have to be laid out at the same viewport. converge_inner_size
# agrees a device size per side and steps off it where the window cannot
# hold the one the plan asks for, so a plan size that is not a whole
# number of device pixels can leave the two a pixel apart, and a page
# whose blocks fill the viewport then differs with no glyph taking part.
# Waited for, since a sweeper reports this only once it has converged.
vp_a=""; vp_b=""
for _ in $(seq 1 600); do
    vp_a="$(grep -m1 '^viewport ' "$SHOTS/${LABELS[0]}.0.log" 2>/dev/null)"
    vp_b="$(grep -m1 '^viewport ' "$SHOTS/${LABELS[1]}.0.log" 2>/dev/null)"
    [ -n "$vp_a" ] && [ -n "$vp_b" ] && break
    alive=0
    for pid in "${SWEEPERS[@]}"; do kill -0 "$pid" 2>/dev/null && alive=1; done
    [ "$alive" = 0 ] && break
    sleep 0.5
done
# A side that never reported one never converged, and every one of its
# sweepers has since exited. No cell can be captured now, so the plan
# ends here and prints what the sweepers said.
if [ -z "$vp_a" ] || [ -z "$vp_b" ]; then
    missing="${LABELS[0]}"
    [ -n "$vp_a" ] && missing="${LABELS[1]}"
    [ -z "$vp_a" ] && [ -z "$vp_b" ] && missing="${LABELS[0]} and ${LABELS[1]}"
    echo "side $missing never reported a viewport, so no cell can be captured. Every sweeper has to print one, and the size agreement below is checked against it:" >&2
    tail -n 3 "$SHOTS/${LABELS[0]}".*.log "$SHOTS/${LABELS[1]}".*.log 2>/dev/null \
        | sed 's/^/    /' >&2
    kill "${SWEEPERS[@]}" 2>/dev/null
    exit 2
fi
if [ "$vp_a" != "$vp_b" ]; then
    echo "side ${LABELS[0]} converged to ${vp_a#viewport } and side ${LABELS[1]} to ${vp_b#viewport }; the two sides would be laid out at different sizes, so every block that fills the viewport would differ without a glyph taking part. Pick a size whose device size both windows can hold." >&2
    kill "${SWEEPERS[@]}" 2>/dev/null
    exit 2
fi

# Comparing is most of a sweep's wall time and one cell says nothing about
# another, so it runs in a pool that outlives the loop instead of a fresh
# interpreter per cell. The pool pairs the frames itself and ends once every
# sweeper has connected and then closed; the loop below prints the cells in
# plan order as it finishes them.
python3 "$HERE/compare_viewport.py" --serve-frames "$COMPARE_WORKERS" \
        "$FRAMES" "${#SWEEPERS[@]}" >"$SHOTS/serve.log" 2>&1 &
COMPARER=$!
echo "$CELLS" > "$SHOTS/lastcell"

for n in $(seq 1 "$CELLS"); do
    cell="$SHOTS/cell$n"
    # lastcell bounds the printing, so a plan cut short stops here too.
    while [ ! -f "$cell/out" ] && [ ! -f "$cell/failed" ]; do
        if [ -f "$SHOTS/lastcell" ] && [ "$n" -gt "$(cat "$SHOTS/lastcell")" ]; then
            break
        fi
        # Nothing watches for a side that stopped delivering, so a cell
        # whose sweepers and comparison are all gone is one that is never
        # coming.
        alive=0
        for pid in "${SWEEPERS[@]}" "$COMPARER"; do
            kill -0 "$pid" 2>/dev/null && alive=1
        done
        if [ "$alive" = 0 ]; then
            mkdir -p "$cell"
            : > "$cell/failed"
            break
        fi
        sleep 0.02
    done
    if [ ! -f "$cell/out" ] && [ ! -f "$cell/failed" ]; then
        break
    fi
    if [ -f "$cell/failed" ]; then
        printf '%-38s %8s  %10s\n' "${ALL_PATHS[$((n-1))]}" \
               "${ALL_SCROLLS[$((n-1))]}" "CAPTURE FAILED"
        grep "^fail $n " "$SHOTS/${LABELS[0]}".*.log \
             "$SHOTS/${LABELS[1]}".*.log 2>/dev/null | sed 's/^/    /' >&2
        FAILED=$((FAILED + 1))
        continue
    fi
    out="$(cat "$cell/out")"
    pct="$(printf '%s' "$out" | sed -n 's/.*= \([0-9.]*\)%.*/\1/p' | head -1)"
    max="$(printf '%s' "$out" | sed -n 's/.*max |diff| \([0-9]*\).*/\1/p' | head -1)"
    mark=""
    if [ "${pct:-}" != "100.0000" ] && is_accepted "${ALL_PATHS[$((n-1))]}"; then
        mark=" accepted"
        ACCEPTED=$((ACCEPTED + 1))
    fi
    printf '%-38s %8s  %9s%%  %8s%s\n' "${ALL_PATHS[$((n-1))]}" \
           "${ALL_SCROLLS[$((n-1))]}" "${pct:-?}" "${max:-0}" "$mark"
    if [ -z "$mark" ]; then
        if [ -n "${max:-}" ] && [ "$max" -gt "$WORST" ]; then WORST="$max"; fi
        note_divergence "${pct:-}" "$n"
    fi
    [ "${pct:-}" = "100.0000" ] && EXACT=$((EXACT + 1))
    if [ -n "$CLUSTERS" ]; then
        printf '%s\n' "$out" | sed -n '/cluster(s)/,$p' | sed 's/^/    /'
    fi
done
wait "$COMPARER" 2>/dev/null
wait "${SWEEPERS[@]}" 2>/dev/null
# Each worker logs its clusters beside its own cell, since several
# appending to one file would interleave their rows.
cat "$SHOTS"/cell*/clusters.tsv > "$SHOTS/clusters.tsv" 2>/dev/null
[ "$KEEP" = 1 ] && echo "shots kept in $SHOTS" >&2

printf '\n%d of %d identical%s\n' "$EXACT" "$CELLS" \
       "$([ "$ACCEPTED" -gt 0 ] && echo ", $ACCEPTED accepted")"
printf '%d cells in %ds, worst channel difference %d%s\n' \
       "$CELLS" "$(( $(date +%s) - STARTED ))" "$WORST" \
       "$([ "$FAILED" -gt 0 ] && echo ", $FAILED failed")"
summarize_clusters
warn_if_wedged
exit "$((FAILED > 0))"
