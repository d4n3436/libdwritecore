#!/bin/bash
# Runs every stress mode under memcheck at two scales.
#
#   stress_leaks.sh <libcleartype.so> <stress_leaks_ft> <outdir> <font>...
#
# Build the driver with:
#   cc -O1 -g $(pkg-config --cflags freetype2) stress_leaks_ft.c \
#      -o stress_leaks_ft $(pkg-config --libs freetype2) -lpthread
#
# Set CLEARTYPE_LOG as well to get the shim's side-table census, which
# says whether anything outlived the faces it belonged to. A table that grows with
# the workload shows up as still-reachable bytes rising with the scale;
# anything the shim allocated and lost shows up as definitely lost.
if [ $# -lt 4 ]; then
  sed -n '2,20p' "$0" | sed 's/^# \?//'
  exit 2
fi
# Set again below on the driver alone: valgrind measuring a shim it was not
# pointed at is a measurement of the wrong file.
unset LD_PRELOAD

LIB=$1; DRIVER=$2; OUT=$3; shift 3
FONTS="$*"
MODES="live-faces memory-faces open-face sizes transforms heap-outlines sfnt variable refcount failure libraries threads"
mkdir -p "$OUT"

# One line per run, printed by whichever process finished it. Short enough to
# reach the terminal in one write, so two that land together do not interleave.
summarize() {                              # summarize <mode> <scale>
  local log="$OUT/$1-$2.log" d i r e
  d=$(grep -oP 'definitely lost: \K[0-9,]+' "$log" | tr -d ,)
  i=$(grep -oP 'indirectly lost: \K[0-9,]+' "$log" | tr -d ,)
  r=$(grep -oP 'still reachable: \K[0-9,]+' "$log" | tr -d ,)
  e=$(grep -oP 'ERROR SUMMARY: \K[0-9]+' "$log" | tail -1)
  printf "%-14s x%-3s definite=%-9s indirect=%-7s reachable=%-9s errors=%s\n" \
         "$1" "$2" "${d:-?}" "${i:-?}" "${r:-?}" "${e:-?}"
}

# The modes are not the same size. failure at x10 runs for minutes while most
# of the rest are done in seconds, so each run reports itself the moment it
# lands rather than every result waiting on the slowest one. The order that
# arrives in is the order they finished; the table at the end is the plan's.
for m in $MODES; do
  for s in 1 10; do
    (
      LD_PRELOAD=$LIB CLEARTYPE_FONTCONFIG=1 \
        valgrind --tool=memcheck --leak-check=full --show-leak-kinds=all \
                 --errors-for-leak-kinds=definite --error-limit=no --num-callers=20 \
                 --log-file="$OUT/$m-$s.log" \
                 "$DRIVER" "$m" "$s" $FONTS >/dev/null 2>&1
      summarize "$m" "$s"
    ) &
  done
done
wait

echo
echo "in plan order:"
for m in $MODES; do
  for s in 1 10; do
    summarize "$m" "$s"
  done
done
