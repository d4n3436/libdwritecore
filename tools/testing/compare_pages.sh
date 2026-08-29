#!/usr/bin/env bash
#
# compare_pages.sh - run a whole page set past two browsers and tabulate it.
#
#   compare_pages.sh <plan> [--keep] [--clusters N] [--css RULES]
#
# The plan is the comparison, written down. Everything a sweep needs is in it:
#
#     size 2200 1150
#     side linux x11::99          127.0.0.1  2828 http://127.0.0.1:8080
#     side win   libvirt:<domain> <guest-ip> 2929 http://<host-ip>:8080 cdp
#     page example/index.html     0
#     page example-long/index.html 0 1000 4600 8800
#
# parity.plan.example beside this script is a filled-in skeleton to copy.
#
#   size    the inner width and height both browsers are held at
#   side    a label, a capture backend (see capture_viewport.sh), the host
#           and port its browser listens on (Marionette or DevTools, per the
#           driver), and the URL prefix *that machine* reaches the page
#           server by, which is not the same string on both when one of them
#           is a guest
#   page    a path under both prefixes, then one or more scroll offsets
#
# A side's sixth field is its browser driver, `marionette` for Firefox or
# `cdp` for Chromium and Electron, and it defaults to marionette.
#
# When both sides are cdp the sweep takes the direct route, one DevTools
# connection per browser for the whole plan and one Page.captureScreenshot
# per cell. The shot is the viewport itself, so there is no marker, no
# whole-screen photograph and no crop. A cdp side's port field may list
# several ports comma-separated, one browser instance each; the cells are
# dealt out across them and a port that does not answer is dropped with a
# warning. Start the extra instances with run_electron_side.sh under their
# own DWC_ELECTRON_STATE.
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
while [ $# -gt 0 ]; do
    case "$1" in
        --keep)     KEEP=1; shift ;;
        --clusters) CLUSTERS="--clusters $2"; shift 2 ;;
        --css)      CSS=(--css "$2"); shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
[ -f "$PLAN" ] || { echo "no such plan: $PLAN" >&2; exit 2; }

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SHOTS="$(mktemp -d "${TMPDIR:-/tmp}/dwc-pages-XXXXXX")"
[ "$KEEP" = 1 ] || trap 'rm -rf "$SHOTS"' EXIT

WIDTH=""; HEIGHT=""
LABELS=(); BACKENDS=(); HOSTS=(); PORTS=(); PREFIXES=(); DRIVERS=()
PAGES=()

while read -r kind rest; do
    case "${kind:-}" in
        ""|\#*) continue ;;
        size) WIDTH="${rest%% *}"; HEIGHT="${rest##* }" ;;
        side)
            set -- $rest
            LABELS+=("$1"); BACKENDS+=("$2"); HOSTS+=("$3"); PORTS+=("$4"); PREFIXES+=("$5")
            DRIVERS+=("${6:-marionette}") ;;
        page) PAGES+=("$rest") ;;
        *) echo "unknown plan line: $kind $rest" >&2; exit 2 ;;
    esac
done < "$PLAN"

if [ -z "$WIDTH" ] || [ "${#LABELS[@]}" -ne 2 ] || [ "${#PAGES[@]}" -eq 0 ]; then
    echo "the plan needs a size, exactly two sides and at least one page" >&2
    exit 2
fi

printf '%-38s %8s  %10s  %8s\n' page scrollY identical "max diff"
printf '%-38s %8s  %10s  %8s\n' "-------------------------------------" ------- ---------- --------

STARTED=$(date +%s)
WORST=0
FAILED=0
CELLS=0
STRIKES=0

# Two CDP sides take the direct route. Each side sweeps the whole plan over
# one connection, every cell is one Page.captureScreenshot, and cells are
# compared here as soon as both sides have delivered them, overlapping with
# the captures still going.
if [ "${DRIVERS[0]}" = cdp ] && [ "${DRIVERS[1]}" = cdp ]; then
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
    for i in 0 1; do
        live=()
        for port in ${PORTS[$i]//,/ }; do
            if curl -s --max-time 3 "http://${HOSTS[$i]}:$port/json/version" \
                    > /dev/null 2>&1; then
                live+=("$port")
            else
                echo "side ${LABELS[$i]}: nothing answers on port $port; sweeping without it" >&2
            fi
        done
        if [ "${#live[@]}" -eq 0 ]; then
            echo "side ${LABELS[$i]}: no port answers" >&2
            exit 1
        fi
        for k in "${!live[@]}"; do
            awk -v n="${#live[@]}" -v k="$k" 'NR % n == (k + 1) % n' \
                "$SHOTS/cells" > "$SHOTS/cells.${LABELS[$i]}.$k"
            python3 "$HERE/sweep_pages_cdp.py" "${HOSTS[$i]}" "${live[$k]}" \
                    "${PREFIXES[$i]}" "$WIDTH" "$HEIGHT" "${LABELS[$i]}" \
                    "$SHOTS" "$SHOTS/cells.${LABELS[$i]}.$k" "${CSS[@]+"${CSS[@]}"}" \
                    >"$SHOTS/${LABELS[$i]}.$k.log" 2>&1 &
            SWEEPERS+=($!)
        done
    done
    trap 'kill "${SWEEPERS[@]}" 2>/dev/null; [ "$KEEP" = 1 ] || rm -rf "$SHOTS"' EXIT

    for n in $(seq 1 "$CELLS"); do
        cell="$SHOTS/cell$n"
        a="$cell/${LABELS[0]}_clean.png"; b="$cell/${LABELS[1]}_clean.png"
        while [ ! -f "$a" ] || [ ! -f "$b" ]; do
            # A sweeper that has finished will not deliver anything more.
            alive=0
            for pid in "${SWEEPERS[@]}"; do kill -0 "$pid" 2>/dev/null && alive=1; done
            if [ "$alive" = 0 ]; then break; fi
            sleep 0.05
        done
        if [ ! -f "$a" ] || [ ! -f "$b" ]; then
            printf '%-38s %8s  %10s\n' "${ALL_PATHS[$((n-1))]}" \
                   "${ALL_SCROLLS[$((n-1))]}" "CAPTURE FAILED"
            grep "^fail $n " "$SHOTS/${LABELS[0]}".*.log \
                 "$SHOTS/${LABELS[1]}".*.log 2>/dev/null | sed 's/^/    /' >&2
            FAILED=$((FAILED + 1))
            continue
        fi
        python3 "$HERE/compare_viewport.py" "$WIDTH" "$HEIGHT" "$a" "$b" \
                $CLUSTERS >"$cell/out" 2>&1
        out="$(cat "$cell/out")"
        pct="$(printf '%s' "$out" | sed -n 's/.*= \([0-9.]*\)%.*/\1/p' | head -1)"
        max="$(printf '%s' "$out" | sed -n 's/.*max |diff| \([0-9]*\).*/\1/p' | head -1)"
        printf '%-38s %8s  %9s%%  %8s\n' "${ALL_PATHS[$((n-1))]}" \
               "${ALL_SCROLLS[$((n-1))]}" "${pct:-?}" "${max:-0}"
        if [ -n "${max:-}" ] && [ "$max" -gt "$WORST" ]; then WORST="$max"; fi
        if [ -n "$CLUSTERS" ]; then
            printf '%s\n' "$out" | sed -n '/cluster(s)/,$p' | sed 's/^/    /'
        fi
    done
    wait "${SWEEPERS[@]}" 2>/dev/null
    [ "$KEEP" = 1 ] && echo "shots kept in $SHOTS" >&2

    printf '\n%d cells in %ds, worst channel difference %d%s\n' \
           "$CELLS" "$(( $(date +%s) - STARTED ))" "$WORST" \
           "$([ "$FAILED" -gt 0 ] && echo ", $FAILED failed")"
    exit "$((FAILED > 0))"
fi

# Comparing one cell and capturing the next have nothing to say to each other,
# so they overlap. The four shots move into a directory of their own, the
# comparison runs there in the background, and its line is printed just before
# the next one's. The table arrives in order and a cell at a time, and a sweep
# costs the captures plus one comparison instead of the sum of the two.
PENDING=""; P_PATH=""; P_SCROLL=""; P_DIR=""

report() {
    [ -n "$PENDING" ] || return 0
    wait "$PENDING"; PENDING=""
    local out pct max i
    out="$(cat "$P_DIR/out")"
    pct="$(printf '%s' "$out" | sed -n 's/.*= \([0-9.]*\)%.*/\1/p' | head -1)"
    max="$(printf '%s' "$out" | sed -n 's/.*max |diff| \([0-9]*\).*/\1/p' | head -1)"
    printf '%-38s %8s  %9s%%  %8s\n' "$P_PATH" "$P_SCROLL" "${pct:-?}" "${max:-0}"
    if [ -n "${max:-}" ] && [ "$max" -gt "$WORST" ]; then WORST="$max"; fi
    if [ -n "$CLUSTERS" ]; then
        printf '%s\n' "$out" | sed -n '/cluster(s)/,$p' | sed 's/^/    /'
    fi
    if [ "$KEEP" != 1 ]; then
        # Four full-screen PNGs a cell, and a sweep is many cells.
        rm -rf "$P_DIR"
    fi
    return 0
}

for entry in "${PAGES[@]}"; do
    set -- $entry
    path="$1"; shift
    for scroll in "$@"; do
        CELLS=$((CELLS + 1))
        pids=()
        for i in 0 1; do
            "$HERE/capture_viewport.sh" --driver "${DRIVERS[$i]}" "${BACKENDS[$i]}" \
                "$SHOTS/${LABELS[$i]}" "${HOSTS[$i]}" "${PORTS[$i]}" \
                "${PREFIXES[$i]}/$path" "$WIDTH" "$HEIGHT" --scroll "$scroll" \
                "${CSS[@]+"${CSS[@]}"}" \
                >"$SHOTS/${LABELS[$i]}.log" 2>&1 &
            pids+=($!)
        done
        ok=1
        for pid in "${pids[@]}"; do
            wait "$pid" || ok=0
        done
        if [ "$ok" = 0 ]; then
            report
            printf '%-38s %8s  %10s\n' "$path" "$scroll" "CAPTURE FAILED"
            sed 's/^/    /' "$SHOTS/${LABELS[0]}.log" "$SHOTS/${LABELS[1]}.log" >&2
            FAILED=$((FAILED + 1))
            STRIKES=$((STRIKES + 1))
            # A browser that has stopped answering fails every cell after this
            # one too, and each of those costs a Marionette setup timeout. One
            # failure can be a bad page; two in a row is the browser.
            if [ "$STRIKES" -ge 2 ]; then
                echo "two cells in a row failed, so the rest of the plan is skipped" >&2
                break 2
            fi
            continue
        fi
        STRIKES=0

        report
        cell="$SHOTS/cell$CELLS"
        mkdir -p "$cell"
        for i in 0 1; do
            mv "$SHOTS/${LABELS[$i]}_marked.png" "$cell/${LABELS[$i]}_marked.png"
            mv "$SHOTS/${LABELS[$i]}_clean.png"  "$cell/${LABELS[$i]}_clean.png"
        done
        python3 "$HERE/compare_viewport.py" "$WIDTH" "$HEIGHT" \
                "$cell/${LABELS[0]}_marked.png" "$cell/${LABELS[0]}_clean.png" \
                "$cell/${LABELS[1]}_marked.png" "$cell/${LABELS[1]}_clean.png" \
                $CLUSTERS >"$cell/out" 2>&1 &
        PENDING=$!; P_PATH="$path"; P_SCROLL="$scroll"; P_DIR="$cell"
    done
done
report
[ "$KEEP" = 1 ] && echo "shots kept in $SHOTS" >&2

printf '\n%d cells in %ds, worst channel difference %d%s\n' \
       "$CELLS" "$(( $(date +%s) - STARTED ))" "$WORST" \
       "$([ "$FAILED" -gt 0 ] && echo ", $FAILED failed")"
[ "$FAILED" -eq 0 ]
