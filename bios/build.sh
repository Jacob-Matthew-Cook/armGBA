#!/bin/sh
# Builds the BIOS image and writes src/bios-image.c. CROSS is the ARM toolchain's prefix, for
# example arm-none-eabi-.
set -e
cd "$(dirname "$0")"
CROSS=${CROSS:-arm-none-eabi-}
FLAGS="-march=armv4t -mthumb-interwork -Wall"
mkdir -p out
for s in *.s; do
	${CROSS}gcc $FLAGS -marm -c "$s" -o "out/${s%.s}.o"
done
${CROSS}ld -z noexecstack -T bios.ld -o out/bios.elf out/*.o
${CROSS}objcopy -O binary out/bios.elf out/bios.bin
size=$(wc -c < out/bios.bin)
if [ "$size" -gt 16384 ]; then
	echo "BIOS is $size bytes, over 16 KiB" >&2
	exit 1
fi
truncate -s 16384 out/bios.bin
python3 image.py out/bios.bin ../src/bios-image.c
echo "BIOS: $size bytes of 16384"
