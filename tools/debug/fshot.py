#!/usr/bin/env python3
"""Screenshot of our firmware (src/): its frame buffer (frame_buffer_data,
4 bit palette indexes, 2 pixels a byte, drivers/framebuffer.c) and palette, read over the cable, saved as a PNG.

  py tools/debug/fshot.py shot.png [--port COM6] [--scale 2]
"""
import argparse
import os
import struct
import subprocess
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dm32dbg  # noqa: E402

ELF = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'src', 'build',
                   'opendmr.elf')
WIDTH, HEIGHT = 240, 320


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--port', default='COM6')
    ap.add_argument('--scale', type=int, default=1)
    args = ap.parse_args()

    sym = {}
    for line in subprocess.run([dm32dbg.NM, ELF], capture_output=True, text=True).stdout.splitlines():
        p = line.split()
        if len(p) == 3:
            sym[p[2]] = int(p[0], 16)
    radio = dm32dbg.Radio(args.port)
    palette = struct.unpack('<16H', radio.mem_read(sym['palette'], 32))
    data = b''
    for a in range(0, WIDTH * HEIGHT // 2, 512):
        data += radio.mem_read(sym['frame_buffer_data'] + a, 512)

    rgb = []
    for c in palette:
        c = ((c & 0xFF) << 8) | (c >> 8)  # native colours are byte swapped RGB565
        rgb.append((((c >> 11) & 0x1F) * 255 // 31, ((c >> 5) & 0x3F) * 255 // 63, (c & 0x1F) * 255 // 31))
    img = Image.new('RGB', (WIDTH, HEIGHT))
    px = img.load()
    for i, b in enumerate(data):
        x = (i * 2) % WIDTH
        y = (i * 2) // WIDTH
        px[x, y] = rgb[b >> 4]  # the even pixel in the high half
        px[x + 1, y] = rgb[b & 0x0F]
    if args.scale > 1:
        img = img.resize((WIDTH * args.scale, HEIGHT * args.scale), Image.NEAREST)
    img.save(args.out)
    print(args.out)


if __name__ == '__main__':
    main()
