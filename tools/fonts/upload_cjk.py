#!/usr/bin/env python3
"""Writes the Chinese font file (tools/fonts/build/cjk.bin, made by mkcjk.py) to the SPI flash of the DM-32, over the
programming cable. The radio runs our firmware as usual; the 'D' 'F' debug request (functions/debugCommands.c) writes
4K sectors at raw flash addresses, only in the font area (CJK_FONT_FLASH_ADDRESS - CJK_FONT_FLASH_END, gfxFontExt.h).

  py tools/fonts/upload_cjk.py COM6
  py tools/fonts/upload_cjk.py COM6 --from-sector 120     carry on after an interruption

Each 1 KB part carries a 16 bit sum and each sector is read back by the radio (requests cut short by the UART happened:
199 bytes of a sector were lost once). The header sector is written last, so a font that was cut short is not used. Restart the radio afterwards: the firmware
reads the header once.
"""
import argparse
import os
import struct
import sys
import time

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
FONT_FILE = os.path.join(HERE, 'build', 'cjk.bin')
FLASH_ADDRESS = 0x280000  # CJK_FONT_FLASH_ADDRESS
FLASH_END = 0x630000
SECTOR = 4096
PART = 1024


class Radio:
    def __init__(self, port):
        self.p = serial.Serial(port, 115200, timeout=0.5)
        time.sleep(0.2)

    def request(self, data, reply_length, timeout=3.0, retries=3):
        for _ in range(retries):
            self.p.reset_input_buffer()
            self.p.write(data)
            buf = b''
            end = time.time() + timeout
            while time.time() < end:
                buf += self.p.read(max(1, self.p.in_waiting))
                i = buf.find(data[:2])
                if i >= 0 and len(buf) - i >= reply_length:
                    return buf[i:i + reply_length]
            time.sleep(0.2)
        raise RuntimeError('no reply to %r' % data[:2])

    # Each part carries the sum of its bytes: the radio refuses a part the UART cut short, and reads the sector back
    # after programming it; the sector is sent again then. Returns the number of retries.
    def write_sector(self, address, data):
        for attempt in range(5):
            ok = True
            for part in range(SECTOR // PART):
                chunk = data[part * PART:(part + 1) * PART]
                r = self.request(b'DF' + struct.pack('>IBH', address, part, sum(chunk) & 0xFFFF) + chunk, 3,
                                 timeout=(5.0 if part == 3 else 3.0))
                if r[2] != 1:
                    ok = False
                    break
            if ok:
                return attempt
            time.sleep(0.3)
        raise RuntimeError('the radio did not write the sector at 0x%06X' % address)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('port')
    ap.add_argument('--file', default=FONT_FILE)
    ap.add_argument('--from-sector', type=int, default=1, help='first sector of the file to write (the header, 0, is always written last)')
    args = ap.parse_args()

    data = open(args.file, 'rb').read()
    if data[:8] != b'DM32CJK2':
        sys.exit('%s is not a font file of mkcjk.py' % args.file)
    data += b'\xff' * (-len(data) % SECTOR)
    if FLASH_ADDRESS + len(data) > FLASH_END:
        sys.exit('the font file is too big: %d bytes, %d available' % (len(data), FLASH_END - FLASH_ADDRESS))

    radio = Radio(args.port)
    sectors = len(data) // SECTOR
    order = list(range(max(1, args.from_sector), sectors)) + [0]
    start = time.time()
    retries = 0
    for n, s in enumerate(order, 1):
        retries += radio.write_sector(FLASH_ADDRESS + s * SECTOR, data[s * SECTOR:(s + 1) * SECTOR])
        if (n % 16 == 0) or (n == len(order)):
            elapsed = time.time() - start
            print('\rsector %d/%d  %.0f s, %.0f s left   ' % (s, sectors - 1, elapsed, elapsed / n * (len(order) - n)),
                  end='', flush=True)
    print('\nwritten and read back: %d bytes at 0x%06X, %d sectors sent again; restart the radio'
          % (len(data), FLASH_ADDRESS, retries))


if __name__ == '__main__':
    main()
