#!/bin/sh
# Compares struct GBA's layout, which is the save state's, between a 64-bit and a 32-bit build. A
# state made by one must load in the other, so every field has to sit at the same offset; only the
# sizes of host pointers, which each have an eight-byte slot, may differ.
# Usage: CC32=<32-bit ARM compiler> tests/state-layout.sh   (CC64 defaults to cc; needs gdb-multiarch)
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d)
printf '#include "gba.h"\nstruct GBA probe;\n' > "$out/probe.c"
for build in 64 32; do
	eval compiler=\${CC$build:-cc}
	$compiler -g -c -I "$here/src" -D'_Static_assert(x,y)=' -o "$out/$build.o" "$out/probe.c"
	gdb-multiarch -batch -ex "set max-value-size unlimited" -ex "ptype /o struct GBA" "$out/$build.o" \
		| sed 's/  */ /g; s/_Bool/bool/g' > "$out/$build.txt"
done
# Lines that only give a pointer's own size (inside its slot) don't count.
diff "$out/64.txt" "$out/32.txt" | grep '^[<>]' | grep -v '^[<>] /\* [48] \*/ ' | grep -v 'row\(Color\|Flags\)\?\[' > "$out/diff.txt" || true
if [ -s "$out/diff.txt" ]; then
	cat "$out/diff.txt"
	echo "struct GBA is laid out differently"
	exit 1
fi
echo "struct GBA is laid out the same"
