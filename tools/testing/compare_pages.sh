#!/usr/bin/env bash
#
# compare_pages.sh - run a whole page set past two browsers and tabulate it.
#
#   compare_pages.sh <plan> [--keep] [--clusters N]
#
# The plan is the comparison, written down. Everything a sweep needs is in it:
#
#     size 2200 1150
#     side linux x11::99          127.0.0.1  2828 http://127.0.0.1:8080
#     side win   libvirt:<domain> <guest-ip> 2929 http://<host-ip>:8080
#     page example/index.html     0
#     page example-long/index.html 0 1000 4600 8800
#
# parity.plan.example beside this script is a filled-in skeleton to copy.
#
#   size    the inner width and height both browsers are held at
#   side    a label, a capture backend (see capture_viewport.sh), the host and
#           port its Marionette listens on, and the URL prefix *that machine*
#           reaches the page server by - which is not the same string on both
#           when one of them is a guest
#   page    a path under both prefixes, then one or more scroll offsets
#
# The two sides are captured at the same time, because they have nothing to do
# with each other: one is a browser on this machine and the other is usually a
# browser in a virtual machine, and waiting for the first to finish before
# starting the second doubles a sweep for no reason.
#
# Start the browsers with run_parity_firefox.sh, which also warms them - a cold
# Firefox reflows its first page when the font list finishes building off the
# main thread, and a capture taken during that is a good screenshot of the
# wrong layout.
set -u

# An inherited LD_PRELOAD would make the control run not a control.
unset LD_PRELOAD

usage() { sed -n '3,32p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-2}"; }

[ $# -ge 1 ] || usage
case "$1" in -h|--help) usage 0 ;; esac

PLAN="$1"; shift
KEEP=0
CLUSTERS=""
while [ $# -gt 0 ]; do
    case "$1" in
        --keep)     KEEP=1; shift ;;
        --clusters) CLUSTERS="--clusters $2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
[ -f "$PLAN" ] || { echo "no such plan: $PLAN" >&2; exit 2; }

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SHOTS="$(mktemp -d "${TMPDIR:-/tmp}/dwc-pages-XXXXXX")"
[ "$KEEP" = 1 ] || trap 'rm -rf "$SHOTS"' EXIT

WIDTH=""; HEIGHT=""
LABELS=(); BACKENDS=(); HOSTS=(); PORTS=(); PREFIXES=()
PAGES=()

while read -r kind rest; do
    case "${kind:-}" in
        ""|\#*) continue ;;
        size) WIDTH="${rest%% *}"; HEIGHT="${rest##* }" ;;
        side)
            set -- $rest
            LABELS+=("$1"); BACKENDS+=("$2"); HOSTS+=("$3"); PORTS+=("$4"); PREFIXES+=("$5") ;;
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
    if [ "$KEEP" = 1 ]; then
        for i in 0 1; do
            cp "$P_DIR/${LABELS[$i]}_clean.png" \
               "$(printf '%s_%s_%s.png' "${LABELS[$i]}" "$(echo "$P_PATH" | tr /. __)" "$P_SCROLL")"
        done
    else
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
            "$HERE/capture_viewport.sh" "${BACKENDS[$i]}" \
                "$SHOTS/${LABELS[$i]}" "${HOSTS[$i]}" "${PORTS[$i]}" \
                "${PREFIXES[$i]}/$path" "$WIDTH" "$HEIGHT" --scroll "$scroll" \
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

printf '\n%d cells in %ds, worst channel difference %d%s\n' \
       "$CELLS" "$(( $(date +%s) - STARTED ))" "$WORST" \
       "$([ "$FAILED" -gt 0 ] && echo ", $FAILED failed")"
[ "$FAILED" -eq 0 ]
