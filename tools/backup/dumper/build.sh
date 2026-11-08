#!/bin/sh
# Builds dumper.bin (kept in git: dm32_dump.py needs no compiler). Run after a change to dumper.c or dumper.ld.
set -e
cd "$(dirname "$0")"
T=../../../toolchain/MinGW/csky-elfabiv2-tools/bin/csky-elfabiv2-
${T}gcc -mcpu=ck803s -O2 -Wall -ffreestanding -fno-tree-loop-distribute-patterns -nostdlib -T dumper.ld \
    -Wl,--no-warn-rwx-segments -o dumper.elf dumper.c
${T}objcopy -O binary -j .image dumper.elf dumper.bin
rm dumper.elf
echo "built dumper.bin ($(wc -c < dumper.bin) bytes)"
