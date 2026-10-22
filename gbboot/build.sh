#!/bin/sh
# Builds armGBA's Game Boy boot ROMs with RGBDS (rgbasm, rgblink) and writes libretro/armgba-boot.h:
# the original Game Boy's and the Pocket's from dmg.asm, the Game Boy Color's and the Game Boy
# Advance's from cgb.asm. The Color's art and tables are written by cgbdata.py;
# its output is kept here.
set -e
cd "$(dirname "$0")"
RGBASM=${RGBASM:-rgbasm}
RGBLINK=${RGBLINK:-rgblink}
mkdir -p out
build() {
	name=$1
	source=$2
	shift 2
	$RGBASM "$@" -o "out/$name.o" "$source"
	$RGBLINK -x -o "out/$name.bin" "out/$name.o"
}
build dmg dmg.asm
build mgb dmg.asm -D MODEL_POCKET
build cgb cgb.asm
build agb cgb.asm -D MODEL_AGB
python3 image.py out ../libretro/armgba-boot.h
