#!/usr/bin/env python3
"""Builds the Chinese font files kept in the SPI flash of the DM-32 (graphics/gfxFontExt.c), from Noto Sans SC (OFL).

The firmware falls back to it for the characters that are not in its built-in (Roboto) fonts:
  17 px      all of GB2312 (6763 hanzi, 682 symbols), for the text lines
  32 px bold the 3755 common hanzi (GB2312 level 1) and the symbols, for the large names; the others fall back to 17 px
  and the GB2312 to Unicode table for the names in the codeplug. About 2.7 MB at raw flash 0x280000 - 0x630000 (below
  the OpenGD77 window, free in the stock layout too: the stock 12 and 24 px fonts end before 0x280000).

  py tools/fonts/mkcjk.py             -> tools/fonts/build/cjk.bin
  py tools/fonts/upload_cjk.py COM6   writes it to the radio

File layout, little endian (keep in step with gfxFontExt.c):
  0   "DM32CJK2"
  8   u32 file length
  12  u32 CRC-32 of the file from byte 64 on
  16  u8 number of fonts, 3 bytes padding
  20  4 x { u32 font offset, u16 glyph count, u8 pixel size, u8 bold }
  52  u32 offset of the GB2312 table: 94 x 94 u16 Unicode, row 0xA1.. then column 0xA1.. (0: none); 0: no table
  56  reserved up to 64
  font: u16 codes[count] (sorted), padded to 4 bytes, then count x 8 byte records
        { u24 bitmap offset from the font start, u8 width, u8 height, s8 x offset, s8 top of the ink from the baseline,
          u8 advance }, then the bitmaps.
  bitmap: the 4 bit coverage of the ink box, row after row, as bytes:
        0x00 - 0x7F  run of (n & 0x3F) + 1 pixels of 0 (bit 6 clear) or 15 (bit 6 set)
        0x80 - 0xFF  (n & 0x7F) + 1 pixels with their own values, 2 per byte (first in the high nibble), in the bytes
                     that follow
"""
import os
import struct
import zlib

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
TTF = os.path.join(HERE, 'ttf', 'NotoSansSC-GB2312-VF.ttf')
BUILD = os.path.join(HERE, 'build')
HEADER_SIZE = 64
MAGIC = b'DM32CJK2'


def gb2312_table():
    table = []
    for hi in range(0xA1, 0xFF):
        for lo in range(0xA1, 0xFF):
            try:
                table.append(ord(bytes([hi, lo]).decode('gb2312')))
            except UnicodeDecodeError:
                table.append(0)
    return table


def compress(levels):
    out = bytearray()
    literal = []

    def flush():
        while literal:
            part = literal[:128]
            del literal[:128]
            out.append(0x80 | (len(part) - 1))
            packed = part + ([0] if len(part) % 2 else [])
            out.extend((packed[i] << 4) | packed[i + 1] for i in range(0, len(packed), 2))

    i = 0
    while i < len(levels):
        v = levels[i]
        n = 1
        while (i + n < len(levels)) and (levels[i + n] == v) and (n < 64):
            n += 1
        if v in (0, 15) and (n >= 2 or not literal):
            flush()
            out.append((0x40 if v else 0) | (n - 1))
            i += n
        else:
            literal.append(v)
            i += 1
    flush()
    return out


def build_font(size, weight, codes):
    font = ImageFont.truetype(TTF, size)
    font.set_variation_by_name(weight)
    glyphs = []
    bitmap = bytearray()
    for code in codes:
        ch = chr(code)
        advance = int(round(font.getlength(ch)))
        x0, y0, x1, y1 = font.getbbox(ch, anchor='ls')
        w, h = max(0, x1 - x0), max(0, y1 - y0)
        offset = len(bitmap)
        if w and h:
            im = Image.new('L', (w, h), 0)
            ImageDraw.Draw(im).text((-x0, -y0), ch, font=font, fill=255, anchor='ls')
            px = im.load()
            bitmap += compress([(px[x, y] + 8) // 17 for y in range(h) for x in range(w)])
        else:
            w = h = x0 = y0 = 0
        glyphs.append((offset, w, h, x0, y0, advance))

    codes_bytes = struct.pack('<%dH' % len(codes), *codes)
    codes_bytes += b'\0' * (-len(codes_bytes) % 4)
    start = len(codes_bytes) + (8 * len(glyphs))  # bitmap offsets are from the font start
    records = bytearray()
    for offset, w, h, x0, y0, advance in glyphs:
        records += struct.pack('<I', start + offset)[:3] + struct.pack('<BBbbB', w, h, x0, y0, advance)
    return codes_bytes + records + bitmap


def write_file(name, body, entries, table):
    table_offset = HEADER_SIZE + len(body)
    body += struct.pack('<%dH' % len(table), *table)

    header = bytearray(MAGIC)
    header += struct.pack('<II', HEADER_SIZE + len(body), zlib.crc32(body) & 0xFFFFFFFF)
    header += struct.pack('<B3x', len(entries))
    for off, count, size, bold in entries + [(0, 0, 0, 0)] * (4 - len(entries)):
        header += struct.pack('<IHBB', off, count, size, bold)
    header += struct.pack('<I', table_offset)
    header += bytes(1) * (HEADER_SIZE - len(header))

    path = os.path.join(BUILD, name)
    with open(path, 'wb') as f:
        f.write(header + body)
    print('%s: %d bytes (%.2f MB)' % (path, len(header) + len(body), (len(header) + len(body)) / 1048576))


def main():
    os.makedirs(BUILD, exist_ok=True)
    table = gb2312_table()
    all_codes = sorted(set(c for c in table if c))
    # GB2312 level 1: rows 0xB0 - 0xD7 (the symbols are rows 0xA1 - 0xA9)
    common = sorted(set(c for i, c in enumerate(table) if c and ((i // 94) + 0xA1) < 0xD8))
    body = bytearray()
    entries = []
    for size, weight, codes in ((17, 'Regular', all_codes), (32, 'Bold', common)):
        data = build_font(size, weight, codes)
        entries.append((HEADER_SIZE + len(body), len(codes), size, weight == 'Bold'))
        body += data
        body += bytes(1) * (-len(body) % 4)
        print('  %d px %s: %d glyphs, %d bytes' % (size, weight, len(codes), len(data)))
    write_file('cjk.bin', body, entries, table)


if __name__ == '__main__':
    main()
