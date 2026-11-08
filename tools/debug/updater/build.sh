#!/bin/sh
# Builds updater.bin (kept in git: fw_update.py needs no compiler). Run after a change to updater.c or updater.ld.
set -e
cd "$(dirname "$0")"
T=../../../toolchain/MinGW/csky-elfabiv2-tools/bin/csky-elfabiv2-
${T}gcc -mcpu=ck803s -O2 -Wall -ffreestanding -fno-tree-loop-distribute-patterns -nostdlib -T updater.ld \
    -Wl,--no-warn-rwx-segments -o updater.elf updater.c
${T}objcopy -O binary -j .image updater.elf updater.bin
rm updater.elf
echo "built updater.bin ($(wc -c < updater.bin) bytes)"
