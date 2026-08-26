#!/usr/bin/env bash
#
# winfonts.sh - install the Windows fonts by ranged download of a Windows ISO.
#
#   tools/winfonts.sh [<iso-url-or-path>] [<dest-dir>] [--force]
#   tools/winfonts.sh [--iso <url-or-path>] [--dest <dir>] [--force]
#
# With no arguments it takes Microsoft's Windows 11 Enterprise evaluation
# image and installs into this user's font directory, which is all a machine
# needs for the Firefox parity path to have the families it asks for.
#
# The ISO is read by the byte over range requests. sources/install.wim is found
# by walking UDF, and a sparse copy of it is filled in with only what wimlib
# has to see: the header, the blob table, the XML, each image's metadata, and
# the blobs holding the wanted files. wimlib then writes the fonts straight
# into the font directory, and the holes are never read.
#
# Being sparse, that copy costs only the bytes fetched, which is around 190 MB
# for the default list, and it is built on a tmpfs. So nothing but the fonts
# themselves is ever written to disk. WINFONTS_MEM_MB caps the memory, 512 by
# default; a list needing more finishes on disk instead.
#
# The ranges are fetched four at a time.
#
# A font already in the destination is not fetched again, and a run that wants
# nothing else does no network work at all. --force fetches the whole list and
# overwrites the fonts that are already there.
#
# A local path works too and takes the same route through the code, with dd in
# place of curl.
#
# These are Microsoft's fonts under Microsoft's terms, which the evaluation
# image states.
#
# Needs curl (for a URL), od, awk, wimlib-imagex, and GNU dd for its
# skip_bytes/seek_bytes flags, which seek to a byte without reading at bs=1.
set -u

# The evaluation image Microsoft serves.
DEFAULT_ISO="https://software-static.download.prss.microsoft.com/dbazure/\
888969d5-f34g-4e03-ac9d-1f9786c66749/26100.1742.240906-0331.ge_release_svc_\
refresh_CLIENTENTERPRISEEVAL_OEMRET_x64FRE_en-us.iso"
DEFAULT_ISO="${DEFAULT_ISO//$'\n'/}"

# Where fontconfig looks without being told. Root installs for the machine,
# under the directory the FHS reserves for locally added files; anyone else
# installs for themselves, under the XDG data directory that fontconfig's
# stock fonts.conf carries as <dir prefix="xdg">fonts</dir>. Neither path is
# a distribution's own invention, so neither has to be looked up per distro.
font_dir() {
    if [ "$(id -u)" = 0 ]; then
        printf '%s\n' /usr/local/share/fonts
    else
        printf '%s\n' "${XDG_DATA_HOME:-$HOME/.local/share}/fonts"
    fi
}

SRC=""
DEST=""
FORCE=0

# The image and the destination, positionally in that order or by flag in any
# order, and nothing else takes an argument.
while [ $# -gt 0 ]; do
    case "$1" in
        --iso)
            [ -z "$SRC" ] || { echo "the image is given twice: $2" >&2; exit 2; }
            SRC="${2:?--iso needs a URL or path}"; shift 2 ;;
        --dest)
            [ -z "$DEST" ] || { echo "the destination is given twice: $2" >&2; exit 2; }
            DEST="${2:?--dest needs a directory}"; shift 2 ;;
        --force) FORCE=1; shift ;;
        -h|--help)
            sed -n '3,6p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) echo "unknown option: $1" >&2; exit 2 ;;
        *)
            if [ -z "$SRC" ]; then
                SRC="$1"
            elif [ -z "$DEST" ]; then
                DEST="$1"
            else
                echo "unexpected argument: $1" >&2; exit 2
            fi
            shift ;;
    esac
done

# A tilde the caller quoted is still a tilde, and mkdir would take it as a
# directory of that name in the working directory.
case "$DEST" in "~/"*) DEST="$HOME/${DEST#\~/}" ;; esac
case "$SRC" in "~/"*) SRC="$HOME/${SRC#\~/}" ;; esac

[ -n "$SRC" ] || SRC="$DEFAULT_ISO"
[ -n "$DEST" ] || DEST="$(font_dir)"

# Every face of the families the parity path asks for, which is not one file
# per family: Windows draws bold and italic from separate designed faces, and
# a family reduced to its regular face leaves the renderer synthesizing what
# Windows never synthesizes.
WANTED=(
    arial.ttf arialbd.ttf ariali.ttf arialbi.ttf ariblk.ttf               # Arial
    times.ttf timesbd.ttf timesi.ttf timesbi.ttf                          # Times New Roman
    cour.ttf courbd.ttf couri.ttf courbi.ttf                              # Courier New
    segoeui.ttf segoeuib.ttf segoeuii.ttf segoeuiz.ttf segoeuil.ttf
    seguili.ttf segoeuisl.ttf seguisli.ttf seguisb.ttf seguisbi.ttf
    seguibl.ttf seguibli.ttf                                              # Segoe UI
    seguiemj.ttf                                                          # Segoe UI Emoji
    comic.ttf comicbd.ttf comici.ttf comicz.ttf                           # Comic Sans MS
    consola.ttf consolab.ttf consolai.ttf consolaz.ttf                    # Consolas
    tahoma.ttf tahomabd.ttf                                               # Tahoma
    cambria.ttc                                                           # Cambria Math
    sylfaen.ttf                                                           # Sylfaen
    himalaya.ttf                                                          # Microsoft Himalaya
    Nirmala.ttc                                                           # Nirmala UI
    msgothic.ttc                                                          # MS Gothic
    YuGothR.ttc YuGothB.ttc YuGothL.ttc YuGothM.ttc                       # Yu Gothic
    malgun.ttf malgunbd.ttf malgunsl.ttf                                  # Malgun Gothic
    msyh.ttc msyhbd.ttc msyhl.ttc                                         # Microsoft YaHei
    msjh.ttc msjhbd.ttc msjhl.ttc                                         # Microsoft JhengHei
    simsun.ttc simsunb.ttf                                                # SimSun
    mingliub.ttc                                                          # MingLiU-ExtB
)

# fontconfig answers from its cache, so a font it has not been told about is
# not installed as far as anything asking is concerned, and the install is only
# real once it resolves the families by the names every caller will ask for.
settle() {
    command -v fc-cache >/dev/null && fc-cache -f "$DEST" >/dev/null 2>&1
    command -v fc-match >/dev/null || return 0
    local family answer
    for family in Arial "Times New Roman" "Segoe UI" Consolas; do
        answer=$(fc-match -f '%{family}' ":family=$family" 2>/dev/null)
        [ "$answer" = "$family" ] || printf 'warning      %s still resolves to %s\n' \
            "$family" "${answer:-nothing}"
    done
    return 0
}

# Only what is missing is worth fetching, and a run that is missing nothing
# stops here, before it needs a network, an image, or wimlib.
MISSING=()
for name in "${WANTED[@]}"; do
    if [ "$FORCE" = 0 ] && [ -f "$DEST/$name" ]; then
        continue
    fi
    MISSING+=("$name")
done
HAVE=$((${#WANTED[@]} - ${#MISSING[@]}))

if [ "${#MISSING[@]}" -eq 0 ]; then
    printf 'have         all %s fonts, in %s\n' "${#WANTED[@]}" "$DEST"
    settle
    exit 0
fi
if [ "$HAVE" -gt 0 ]; then
    printf 'have         %s of %s fonts; fetching the other %s\n' \
        "$HAVE" "${#WANTED[@]}" "${#MISSING[@]}"
fi
WANTED=("${MISSING[@]}")

for tool in dd od awk wimlib-imagex; do
    command -v "$tool" >/dev/null && continue
    [ "$tool" = wimlib-imagex ] && tool="wimtools / wimlib-utils / wimlib"
    echo "missing: $tool" >&2
    exit 1
done
case "$SRC" in
    http://*|https://*) command -v curl >/dev/null || { echo "missing: curl" >&2; exit 1; };;
    *) [ -f "$SRC" ] || { echo "no such file: $SRC" >&2; exit 1; };;
esac

# Checked before the work and not after it, since the alternative is finding
# out at the end of a download.
mkdir -p "$DEST" 2>/dev/null || { echo "cannot create $DEST" >&2; exit 1; }
[ -w "$DEST" ] || { echo "no write access to $DEST" >&2; exit 1; }
printf 'installing   into %s\n' "$DEST"

# Kept in a file and not a variable, because most calls happen inside $( ) and
# a subshell's arithmetic does not come back.
TALLY=""

# fetch <offset> <length> - those bytes of the source, on stdout.
fetch() {
    [ -n "$TALLY" ] && printf '%s\n' "$2" >> "$TALLY"
    case "$SRC" in
        http://*|https://*)
            curl -sfL --retry 3 -r "$1-$(( $1 + $2 - 1 ))" "$SRC" ;;
        *)
            dd if="$SRC" bs=1M iflag=skip_bytes,count_bytes \
               skip="$1" count="$2" status=none ;;
    esac
}

BLOCK=1048576

# cfetch <offset> <length> - like fetch, through cached blocks.
#
# The UDF structures are a few hundred bytes each and sit close together, so
# reading them one at a time spends eight round trips on 55 KB. A block is
# large enough that the whole walk usually comes from two.
cfetch() {
    local off=$1 len=$2 first last b
    first=$((off / BLOCK))
    last=$(((off + len - 1) / BLOCK))
    for ((b = first; b <= last; b++)); do
        [ -s "$CACHE/$b" ] || fetch $((b * BLOCK)) $BLOCK > "$CACHE/$b"
    done
    # fullblock, because the input is a pipe: without it a short read counts
    # as a whole block and the slice comes out of the wrong place, quietly.
    for ((b = first; b <= last; b++)); do cat "$CACHE/$b"; done |
        dd bs=1M iflag=fullblock,skip_bytes,count_bytes \
           skip=$((off - first * BLOCK)) count="$len" status=none
}

# le <file> <offset> <width> - one little-endian unsigned integer.
le() {
    od -An -j "$2" -N "$3" -t u"$3" -v < "$1" | tr -d ' \n'
}

WORK="$(mktemp -d)"
SPARSE=""
trap 'rm -rf "$WORK"; [ -n "$SPARSE" ] && rm -f "$SPARSE"' EXIT
TALLY="$WORK/tally"
: > "$TALLY"
CACHE="$WORK/cache"
mkdir -p "$CACHE"

MEM_LIMIT_KB=$(( ${WINFONTS_MEM_MB:-512} * 1024 ))

# A tmpfs with room for the whole sparse copy, or nothing if neither has it.
# XDG_RUNTIME_DIR comes first because it is the user's own and mode 0700.
memory_dir() {
    local dir
    for dir in "${XDG_RUNTIME_DIR:-}" /dev/shm; do
        [ -n "$dir" ] && [ -d "$dir" ] && [ -w "$dir" ] || continue
        [ "$(stat -f -c %T "$dir" 2>/dev/null)" = tmpfs ] || continue
        [ "$(df -k --output=avail "$dir" 2>/dev/null | tail -1)" -ge "$MEM_LIMIT_KB" ] || continue
        printf '%s\n' "$dir"
        return 0
    done
    return 1
}

# ---------------------------------------------------------------------------
# UDF: find sources/install.wim.
#
# Windows media is a UDF bridge. The ISO9660 side carries only README.TXT once
# install.wim passes 4 GiB, so the answer is on the UDF side: anchor at sector
# 256, which names the volume descriptor sequence, which names the partition
# and the file set, which names the root directory.
# ---------------------------------------------------------------------------
UDF_BLOCK=2048

cfetch $((256 * UDF_BLOCK)) $UDF_BLOCK > "$WORK/anchor"
[ "$(le "$WORK/anchor" 0 2)" = 2 ] || { echo "no UDF anchor at sector 256" >&2; exit 1; }
MVDS_LEN=$(le "$WORK/anchor" 16 4)
MVDS_LBA=$(le "$WORK/anchor" 20 4)

PART_START=""
FSD_LBA=""
cfetch $((MVDS_LBA * UDF_BLOCK)) "$MVDS_LEN" > "$WORK/mvds"
for ((i = 0; i < MVDS_LEN / UDF_BLOCK; i++)); do
    at=$((i * UDF_BLOCK))
    case "$(le "$WORK/mvds" $at 2)" in
        5) PART_START=$(le "$WORK/mvds" $((at + 188)) 4) ;;
        6) FSD_LBA=$(le "$WORK/mvds" $((at + 252)) 4) ;;   # LogicalVolumeContentsUse.lba
        8) break ;;
    esac
done
[ -n "$PART_START" ] && [ -n "$FSD_LBA" ] || { echo "UDF: no partition or logical volume" >&2; exit 1; }

cfetch $(((PART_START + FSD_LBA) * UDF_BLOCK)) $UDF_BLOCK > "$WORK/fsd"
[ "$(le "$WORK/fsd" 0 2)" = 256 ] || { echo "UDF: file set descriptor not found" >&2; exit 1; }
ICB_LBA=$(le "$WORK/fsd" 404 4)                  # RootDirectoryICB.lba

# extents <icb-lba> - the "offset length" runs of whatever that ICB describes.
extents() {
    local lba=$1 tag base ea ad at type len elba out=""
    cfetch $(((PART_START + lba) * UDF_BLOCK)) $UDF_BLOCK > "$WORK/fe"
    tag=$(le "$WORK/fe" 0 2)
    case "$tag" in
        261) base=176 ;;                         # file entry
        266) base=216 ;;                         # extended file entry
        *) echo "UDF: expected a file entry, got tag $tag" >&2; exit 1 ;;
    esac
    type=$(( $(le "$WORK/fe" 34 2) & 7 ))
    ea=$(le "$WORK/fe" $((base - 8)) 4)
    ad=$(le "$WORK/fe" $((base - 4)) 4)
    at=$((base + ea))
    while [ $at -lt $((base + ea + ad)) ]; do
        len=$(( $(le "$WORK/fe" $at 4) & 0x3FFFFFFF ))
        [ "$len" -eq 0 ] && break
        # A short_ad is 8 bytes and a long_ad is 16; both open with the length
        # and the block, so only the stride differs.
        if [ "$type" -eq 0 ]; then
            elba=$(le "$WORK/fe" $((at + 4)) 4); at=$((at + 8))
        else
            elba=$(le "$WORK/fe" $((at + 4)) 4); at=$((at + 16))
        fi
        out="$out $(( (PART_START + elba) * UDF_BLOCK )) $len"
    done
    echo "$out"
}

# child <icb-lba> <name> - the ICB of that entry in that directory.
child() {
    local runs off len at l_fi l_iu name total=0
    read -r -a runs <<< "$(extents "$1")"
    : > "$WORK/dir"
    for ((i = 0; i < ${#runs[@]}; i += 2)); do
        cfetch "${runs[i]}" "${runs[i+1]}" >> "$WORK/dir"
        total=$((total + runs[i+1]))
    done
    at=0
    while [ $((at + 38)) -le $total ]; do
        [ "$(le "$WORK/dir" $at 2)" = 257 ] || break
        l_fi=$(le "$WORK/dir" $((at + 19)) 1)
        l_iu=$(le "$WORK/dir" $((at + 36)) 2)
        # The identifier opens with a compression byte. 16 means UTF-16BE, so
        # the ASCII being matched sits in every other byte.
        name=$(dd if="$WORK/dir" bs=64K iflag=skip_bytes,count_bytes \
                  skip=$((at + 38 + l_iu + 1)) count=$((l_fi - 1)) \
                  status=none 2>/dev/null | tr -d '\0')
        if [ "${name,,}" = "${2,,}" ]; then
            le "$WORK/dir" $((at + 24)) 4
            return 0
        fi
        at=$(( (at + 38 + l_iu + l_fi + 3) & ~3 ))
    done
    echo "UDF: not found: $2" >&2
    exit 1
}

ICB_LBA=$(child "$ICB_LBA" sources)
ICB_LBA=$(child "$ICB_LBA" install.wim)
read -r -a WIM_RUNS <<< "$(extents "$ICB_LBA")"
WIM_SIZE=0
for ((i = 1; i < ${#WIM_RUNS[@]}; i += 2)); do WIM_SIZE=$((WIM_SIZE + WIM_RUNS[i])); done
printf 'install.wim  %s bytes in %s extent(s)\n' "$WIM_SIZE" "$((${#WIM_RUNS[@]} / 2))"

# wim_fetch <wim-offset> <length> - those bytes of install.wim, mapped through
# its extents, because UDF splits a file this size across several.
wim_fetch() {
    local at=$1 want=$2 take i
    for ((i = 0; i < ${#WIM_RUNS[@]}; i += 2)); do
        if [ "$at" -ge "${WIM_RUNS[i+1]}" ]; then at=$((at - WIM_RUNS[i+1])); continue; fi
        take=$((want < WIM_RUNS[i+1] - at ? want : WIM_RUNS[i+1] - at))
        fetch $((WIM_RUNS[i] + at)) "$take"
        want=$((want - take)); at=0
        [ "$want" -eq 0 ] && break
    done
}

# put <wim-offset> <length> - fetch that range into the sparse copy in place.
#
# Straight down the pipe, so a range is never staged in a file of its own.
# fullblock, because the input is a pipe and a short read would otherwise end
# the copy early.
put() {
    [ "$2" -eq 0 ] && return
    wim_fetch "$1" "$2" |
        dd of="$SPARSE" bs=1M iflag=fullblock oflag=seek_bytes seek="$1" \
           conv=notrunc status=none
}

# put_all <file> - every "offset length" line in it, several at a time.
#
# The ranges are disjoint and each lands at its own offset, so the order they
# arrive in does not matter.
PARALLEL=4
put_all() {
    local live=0 off size
    while read -r off size; do
        put "$off" "$size" &
        live=$((live + 1))
        if [ "$live" -ge "$PARALLEL" ]; then wait -n; live=$((live - 1)); fi
    done < "$1"
    wait
}

if MEM_DIR=$(memory_dir); then
    SPARSE="$MEM_DIR/winfonts-$$.wim"
else
    SPARSE="$DEST/.winfonts-$$.wim"
    printf 'memory       no tmpfs with %s MB free; staging in %s\n' \
        "$((MEM_LIMIT_KB / 1024))" "$DEST"
fi
: > "$SPARSE"
truncate -s "$WIM_SIZE" "$SPARSE"

wim_fetch 0 208 > "$WORK/hdr"
[ "$(od -An -c -N 5 "$WORK/hdr" | tr -d ' \n')" = "MSWIM" ] || {
    echo "sources/install.wim is not a WIM - an install.esd cannot be read this way" >&2
    exit 1
}
# A resource header is a 7-byte size, a flag byte, then two 8-byte fields.
TABLE_SIZE=$(od -An -j 48 -N 7 -t u1 -v "$WORK/hdr" | awk '{for(i=7;i>=1;i--) s=s*256+$i; print s}')
TABLE_OFF=$(le "$WORK/hdr" 56 8)
XML_SIZE=$(od -An -j 72 -N 7 -t u1 -v "$WORK/hdr" | awk '{for(i=7;i>=1;i--) s=s*256+$i; print s}')
XML_OFF=$(le "$WORK/hdr" 80 8)

dd if="$WORK/hdr" of="$SPARSE" bs=64K conv=notrunc status=none

# The blob table and the XML sit next to each other, so one request covers
# both, and the table is read back out of the sparse copy instead of asked for
# a second time.
INDEX_OFF=$((TABLE_OFF < XML_OFF ? TABLE_OFF : XML_OFF))
INDEX_END=$((TABLE_OFF + TABLE_SIZE))
[ $((XML_OFF + XML_SIZE)) -gt $INDEX_END ] && INDEX_END=$((XML_OFF + XML_SIZE))
put "$INDEX_OFF" $((INDEX_END - INDEX_OFF))
dd if="$SPARSE" of="$WORK/table" bs=1M iflag=skip_bytes,count_bytes \
   skip="$TABLE_OFF" count="$TABLE_SIZE" status=none

# The blob table holds 50-byte records of size, flags, offset, part, refcount
# and SHA-1. od and awk walk it because there are tens of thousands.
od -An -tx1 -v "$WORK/table" | tr -d ' \n' | fold -w100 | awk '
# Not strtonum, which only gawk has. The awk Debian and Ubuntu install by
# default is mawk, and there it answers zero for every field.
function hex(s,   i, n) {
    n = 0;
    for (i = 1; i <= length(s); i++)
        n = n * 16 + index("0123456789abcdef", substr(s, i, 1)) - 1;
    return n;
}
{
    size = 0; for (i = 7; i >= 1; i--) size = size * 256 + hex(substr($0, i*2-1, 2));
    flags = hex(substr($0, 15, 2));
    off = 0;  for (i = 8; i >= 1; i--) off  = off  * 256 + hex(substr($0, 16+i*2-1, 2));
    sha1 = substr($0, 61, 40);
    print sha1, off, size, flags;
}' > "$WORK/blobs"
printf 'index        %s blobs\n' "$(wc -l < "$WORK/blobs")"

# The metadata resource is what lets wimlib answer for the tree, and only
# image 1 is ever extracted from. A consumer image holds eleven editions and
# a metadata resource for each, which is 90 MB to fetch and read nothing of,
# so the first is taken alone and the rest only if that turns out to be the
# wrong one. Written to a file first, so that the fetch counter survives the
# loop.
awk '$4 % 4 >= 2 { print $2, $3 }' "$WORK/blobs" > "$WORK/meta.all"
head -1 "$WORK/meta.all" > "$WORK/meta"
put_all "$WORK/meta"
printf 'index        1 of %s image(s), %s bytes in %s requests so far\n' \
    "$(wc -l < "$WORK/meta.all")" "$(awk '{t += $1} END {print t + 0}' "$TALLY")" \
    "$(wc -l < "$TALLY")"

# ---------------------------------------------------------------------------
# The files themselves. wimlib knows the tree now, so it can say which blob
# holds each font, and only those are fetched.
# ---------------------------------------------------------------------------
printf '%s\n' "${WANTED[@]}" | tr 'A-Z' 'a-z' | sort > "$WORK/wanted"

# name, blob hash and the name as spelled in the image, which is the case
# wimlib will want back when extracting.
list_fonts() {
    wimlib-imagex dir "$SPARSE" 1 --detailed --path=/Windows/Fonts 2>/dev/null |
        awk '
            /^Full Path/ { n = $NF; gsub(/[\\"]/, "", n); sub(/.*\//, "", n); next }
            /^Hash/ && n != "" && $NF !~ /^0x0+$/ { print tolower(n), substr($NF, 3), n; n = "" }
        ' | sort
}

list_fonts > "$WORK/present"
if [ ! -s "$WORK/present" ]; then
    # The first metadata resource was not image 1's after all, so the guess
    # costs one wasted fetch and nothing else.
    put_all "$WORK/meta.all"
    list_fonts > "$WORK/present"
fi

join "$WORK/wanted" "$WORK/present" > "$WORK/found"
comm -23 "$WORK/wanted" <(cut -d' ' -f1 "$WORK/present" | sort) > "$WORK/absent"
[ -s "$WORK/absent" ] && printf 'not in image  %s\n' "$(tr '\n' ' ' < "$WORK/absent")"

# Blobs close enough together are asked for as one range. A gap costs the bytes
# skipped over; a separate request costs a round trip.
sort -k1,1 "$WORK/blobs" > "$WORK/blobs.sorted"
awk '{ print $2, $1 }' "$WORK/found" | sort -k1,1 |
    join -1 1 -2 1 -o 2.2,2.3 - "$WORK/blobs.sorted" | sort -u -n |
    awk -v gap="${WINFONTS_GAP:-1048576}" '
        NR == 1 { o = $1; l = $2; next }
        $1 - (o + l) <= gap { l = $1 + $2 - o; next }
        { print o, l; o = $1; l = $2 }
        END { if (NR) print o, l }
    ' > "$WORK/ranges"

printf 'plan         %s file(s), %s blob(s), %s compressed bytes\n' \
    "$(wc -l < "$WORK/found")" "$(wc -l < "$WORK/ranges")" \
    "$(awk '{t += $2} END {print t + 0}' "$WORK/ranges")"

# The blobs are the bulk of it, so the limit is tested against them before
# any are fetched. Only the index is written at this point, which is why
# moving the file is cheap when it does not fit.
PLANNED_KB=$(awk '{t += $2} END {printf "%d", (t + 1023) / 1024}' "$WORK/ranges")
USED_KB=$(du -sk "$SPARSE" | cut -f1)
if [ "$SPARSE" != "$DEST/.winfonts-$$.wim" ] &&
   [ $((USED_KB + PLANNED_KB)) -gt "$MEM_LIMIT_KB" ]; then
    printf 'memory       %s MB needed, over the %s MB limit; finishing on disk\n' \
        "$(( (USED_KB + PLANNED_KB) / 1024 ))" "$((MEM_LIMIT_KB / 1024))"
    cp --sparse=always "$SPARSE" "$DEST/.winfonts-$$.wim"
    rm -f "$SPARSE"
    SPARSE="$DEST/.winfonts-$$.wim"
fi

put_all "$WORK/ranges"

mapfile -t PATHS < <(awk '{ print "/Windows/Fonts/" $3 }' "$WORK/found")
[ ${#PATHS[@]} -eq 0 ] && { echo "nothing to extract" >&2; exit 1; }
printf 'held         %s KB in %s\n' \
    "$(du -sk "$SPARSE" | cut -f1)" \
    "$([ "$SPARSE" = "$DEST/.winfonts-$$.wim" ] && echo "$DEST" || echo memory)"

wimlib-imagex extract "$SPARSE" 1 "${PATHS[@]}" --dest-dir="$DEST" --no-acls \
    >/dev/null 2>&1 || { echo "extraction failed" >&2; exit 1; }
rm -f "$SPARSE"
SPARSE=""

INSTALLED=0
INSTALLED_BYTES=0
while read -r _ _ name; do
    [ -f "$DEST/$name" ] || continue
    INSTALLED=$((INSTALLED + 1))
    INSTALLED_BYTES=$((INSTALLED_BYTES + $(stat -c %s "$DEST/$name")))
done < "$WORK/found"

printf 'done         %s fonts, %s bytes in %s\n' "$INSTALLED" "$INSTALLED_BYTES" "$DEST"
FETCHED=$(awk '{t += $1} END {print t + 0}' "$TALLY")
printf 'transferred  %s bytes in %s requests (%s%% of the image)\n' \
    "$FETCHED" "$(wc -l < "$TALLY")" \
    "$(awk -v f="$FETCHED" -v w="$WIM_SIZE" 'BEGIN {printf "%.2f", f*100/w}')"

settle
