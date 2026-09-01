#!/usr/bin/env bash
#
# census_per_language.sh - run the fallback or raster census one language at a
# time, restarting both browsers in between.
#
#   census_per_language.sh --restart '<command>' --sideA cdp:host:port \
#       --sideB cdp:host:port [--restart-b '<command>'] \
#       [--mode fallback] [--out DIR] [--langs '- ja zh-CN'] [--accept FILE]...
#
# A renderer caches a fallback answer against the character alone, so the
# second language to ask a character gets the first one's answer and the run
# reads as divergence that is not there. Both sides have to be stopped and
# restarted for every language, the guest included; starting a browser without
# stopping the one already holding the port leaves the old renderer in place
# and poisons every language after the first.
#
# --restart and --restart-b are the shell commands that bring each side back
# up. They differ per build, and the far side is not always an Electron in a
# libvirt guest, so both are passed in rather than guessed at. A side with no
# restart command is only waited for, which is the right thing when that side
# has no cache to poison.
set -u

RESTART=""; RESTART_B=""; SIDE_A=""; SIDE_B=""
MODE=fallback; OUT=""; ACCEPT=""; VIEWPORT=""
LANGS="- ja zh-CN zh-TW ko ar th hi"

while [ $# -gt 0 ]; do
    case "$1" in
        --restart)    RESTART="$2"; shift 2 ;;
        --restart-b)  RESTART_B="$2"; shift 2 ;;
        --sideA)      SIDE_A="$2"; shift 2 ;;
        --sideB)      SIDE_B="$2"; shift 2 ;;
        --mode)       MODE="$2"; shift 2 ;;
        --out)        OUT="$2"; shift 2 ;;
        --langs)      LANGS="$2"; shift 2 ;;
        --accept)     ACCEPT="$ACCEPT $2"; shift 2 ;;
        # Raster compares screenshots, so sides whose windows differ in size
        # need the layout viewport pinned; see font_census.py --viewport.
        --viewport)   VIEWPORT="$2"; shift 2 ;;
        *) echo "unknown argument $1" >&2; exit 2 ;;
    esac
done
[ -n "$RESTART" ] && [ -n "$SIDE_A" ] && [ -n "$SIDE_B" ] || {
    echo "usage: $0 --restart CMD --sideA SPEC --sideB SPEC [--restart-b CMD]" >&2
    exit 2
}

HERE="$(cd "$(dirname "$0")" && pwd)"
[ -n "$OUT" ] && mkdir -p "$OUT"

# The port a spec names, which is what says whether the side came back.
port_of() { printf '%s\n' "$1" | sed -E 's/.*:([0-9]+)$/\1/'; }
host_of() { printf '%s\n' "$1" | sed -E 's|^(cdp:)?([^:]+):[0-9]+$|\2|'; }

answers() {                               # answers <host> <port>
    curl -s -m 3 "http://$1:$2/json/version" 2>/dev/null | grep -q Browser
}

wait_for() {                              # wait_for <host> <port> <seconds>
    local i=0
    while [ "$i" -lt "$3" ]; do
        answers "$1" "$2" && return 0
        sleep 1; i=$((i + 1))
    done
    return 1
}

total=0; diverge=0; accepted=0; failed=""
for lang in $LANGS; do
    # Local side: down, then up, and only then is it this language's browser.
    eval "$RESTART" > /dev/null 2>&1
    if ! wait_for "$(host_of "$SIDE_A")" "$(port_of "$SIDE_A")" 40; then
        echo "$lang: local side never answered"; failed="$failed $lang"; continue
    fi
    [ -n "$RESTART_B" ] && eval "$RESTART_B" > /dev/null 2>&1
    if ! wait_for "$(host_of "$SIDE_B")" "$(port_of "$SIDE_B")" 90; then
        echo "$lang: far side never answered"; failed="$failed $lang"; continue
    fi

    set -- "$MODE" "$SIDE_A" "$SIDE_B" --lang "$lang"
    for file in $ACCEPT; do set -- "$@" --accept "$file"; done
    [ -n "$VIEWPORT" ] && set -- "$@" --viewport "$VIEWPORT"
    out=$(timeout 900 python3 "$HERE/font_census.py" "$@" 2>&1)
    [ -n "$OUT" ] && printf '%s\n' "$out" > "$OUT/$MODE-$lang.txt"

    line=$(printf '%s\n' "$out" | grep "^== $MODE")
    if [ -z "$line" ]; then
        echo "$lang: no result"; failed="$failed $lang"; continue
    fi
    echo "$lang: $line"
    t=$(printf '%s\n' "$line" | sed -E 's/.* of ([0-9]+) cells.*/\1/')
    d=$(printf '%s\n' "$line" | sed -E 's/.*\(([0-9]+) diverge.*/\1/')
    a=$(printf '%s\n' "$line" | sed -E 's/.*, ([0-9]+) accepted.*/\1/')
    total=$((total + t)); diverge=$((diverge + d)); accepted=$((accepted + a))
done

echo "TOTAL: $((total - diverge)) of $total match, $diverge diverge, $accepted accepted"
[ -n "$failed" ] && echo "languages with no result:$failed"
exit 0
