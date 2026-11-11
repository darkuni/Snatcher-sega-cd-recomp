#!/bin/sh
# Differential test of the recompiler against the Musashi 68000 interpreter (MIT licence).
# Usage: run.sh <path to a Musashi checkout> [cases] [seed]
set -e
M=$1; N=${2:-3000}; SEED=${3:-7}
D=$(cd "$(dirname "$0")" && pwd); B=$D/build; mkdir -p "$B"
[ -f "$B/m68kops.c" ] || (cd "$B" && cc -o m68kmake "$M/m68kmake.c" && ./m68kmake "$B" "$M/m68k_in.c" >/dev/null)
python3 "$D/gen.py" "$N" "$B" "$SEED"
cc -O1 -w -c -I"$M" -I"$B" "$M/m68kcpu.c" -o "$B/m68kcpu.o"
cc -O1 -w -c -I"$M" -I"$B" "$B/m68kops.c" -o "$B/m68kops.o"
cc -O1 -w -c -I"$M" "$M/m68kdasm.c" -o "$B/m68kdasm.o"
cc -O1 -w -c -I"$M" "$M/softfloat/softfloat.c" -o "$B/softfloat.o"
c++ -std=c++17 -O1 -w -I"$D/../../src/runtime" -c "$B/funcs.cpp" -o "$B/funcs.o"
c++ -std=c++17 -O1 -w -I"$M" -I"$B" -c "$D/harness.cpp" -o "$B/harness.o"
c++ -o "$B/difftest" "$B/harness.o" "$B/funcs.o" "$B/m68kcpu.o" "$B/m68kops.o" "$B/m68kdasm.o" "$B/softfloat.o"
"$B/difftest" 6
