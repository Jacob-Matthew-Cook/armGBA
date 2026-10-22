#!/bin/sh
# Builds bioscheck.gba. CROSS is the ARM toolchain's prefix, for example arm-none-eabi-.
set -e
cd "$(dirname "$0")"
CROSS=${CROSS:-arm-none-eabi-}
python3 data.py
FLAGS="-march=armv4t -mthumb-interwork -mfloat-abi=soft -ffreestanding -fno-builtin -O2 -Wall"
${CROSS}gcc $FLAGS -marm -c crt0.s -o crt0.o
${CROSS}gcc $FLAGS -mthumb -c main.c -o main.o
${CROSS}gcc $FLAGS -mthumb -c lib.c -o lib.o
${CROSS}gcc $FLAGS -nostartfiles -nostdlib -Wl,-z,noexecstack -T rom.ld -o bioscheck.elf crt0.o main.o lib.o
${CROSS}objcopy -O binary bioscheck.elf bioscheck.gba
# Padded past 256 KiB, so emulators can't take it for a multiboot image.
if [ "$(wc -c < bioscheck.gba)" -gt 524288 ]; then
	echo "bioscheck.gba is over 512 KiB" >&2
	exit 1
fi
truncate -s 512K bioscheck.gba
echo "bioscheck.gba: $(wc -c < bioscheck.gba) bytes"
