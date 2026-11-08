#!/usr/bin/env python3
"""Backs up a DM-32's flash over the programming cable, about 2.5 times as fast as Roger VK3KYY's C7000_read_*.py
(github.com/rogerclarkmelbourne/DM32, python/):
the same handshake with the HR_C7000's internal bootloader, then our reader (dumper/dumper.bin, from dumper.c) in the
RAM, at 250000 baud, 4 KB blocks with a CRC each (a damaged block is read again). 375000 lost bytes in
the CH340 cable (2 of 8 blocks whole; 250000 and 115200: 8 of 8).

  py tools/backup/dm32_dump.py COM6 data backup/<colour>-<S/N>/dm32_25q128.bin       the 25Q128, 16 MB (about 11 min)
  py tools/backup/dm32_dump.py COM6 program backup/<colour>-<S/N>/dm32_progmem.bin   the program flash (size: its ID)
  py tools/backup/dm32_dump.py COM6 program backup/<colour>-<S/N>/dm32_progmem_25q08.bin --write
      writes it back: the sectors that differ only (--force: all), each read back, then all compared again;
      the program flash below 0xC000 (the bootloader) only with --boot
  options: --start 0x0 --size 0x1000000 (bytes, 4 KB multiples)  --baud 250000

Switch the radio off, start this, then switch it on (no key) within 30 s: a black screen, the LED off. Read each chip
twice and compare. A radio running our firmware has to be powered off first: the
bootloader only answers at power on.
"""
import argparse
import os
import struct
import sys
import time
import zlib

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
DUMPER = os.path.join(HERE, 'dumper', 'dumper.bin')
LOAD_ADDRESS = 0x10000
BLOCK = 4096
BOOT_REQUEST = b'\x02\x24\x00\x03'  # the bootloader at power on
BOOT_ANSWER = b'\x02\x14\x00\x03'
ACK_OK = b'\x02\x05\x00\x03'
SLOW = 115200
CHIPS = {'data': 0, 'program': 1}
BOOT_AREA = 0xC000  # the program flash below it: the bootloader (dumper.c BOOT_AREA)
CHIP_NAMES = {0: 'Data flash (25Q128)', 1: 'Program flash'}
SIZES = {0x14: 1 << 20, 0x15: 2 << 20, 0x18: 16 << 20}  # JEDEC capacity byte -> bytes

# The radio's screen: text lines rendered here (Pillow, the firmware's Roboto Condensed), sent as 1 bit pictures
FONT = os.path.join(HERE, '..', 'fonts', 'ttf', 'RobotoCondensed-VF.ttf')
SCREEN_WIDTH = 240


def colour(r, g, b):
    """The panel's 16 bits: RGB565 with red and blue swapped (it takes BGR)."""
    return ((b & 0xF8) << 8) | ((g & 0xFC) << 3) | (r >> 3)


WHITE = colour(255, 255, 255)
GREY = colour(160, 160, 160)
GREEN = colour(0, 210, 0)
RED = colour(240, 40, 40)


class Screen:
    """Lines of text at fixed heights, centred (the bar between them is the reader's own)."""
    LINES = {'title': (14, 26), 'chip': (50, 20), 'id': (78, 20), 'percent': (114, 52), 'amount': (226, 22),
             'speed': (256, 20), 'left': (284, 20)}  # y, font size (the bar is at y 190 to 214)
    BOLD = ('title', 'percent')
    WIDEST = {'percent': '100%'}  # right-aligned where this would end centred: the '%' stays put as digits come

    def __init__(self, port):
        self.port = port
        self.shown = {}  # the text of each line on the screen: a line is sent only when it changes
        try:
            from PIL import Image, ImageDraw, ImageFont
            self.pil = (Image, ImageDraw, ImageFont)
        except ImportError:
            self.pil = None  # no Pillow: the percentage and the bar only

    def line(self, which, text, fg=WHITE):
        if self.pil is None or self.shown.get(which) == (text, fg):
            return
        self.shown[which] = (text, fg)
        Image, ImageDraw, ImageFont = self.pil
        y, size = self.LINES[which]
        font = ImageFont.truetype(FONT, size)
        try:
            font.set_variation_by_name('Bold' if which in self.BOLD else 'Regular')
        except (OSError, AttributeError):
            pass
        height = size + 6
        im = Image.new('1', (SCREEN_WIDTH, height), 0)
        draw = ImageDraw.Draw(im)
        width = draw.textlength(text, font=font)
        if which in self.WIDEST:
            x = (SCREEN_WIDTH + draw.textlength(self.WIDEST[which], font=font)) / 2 - width
        else:
            x = (SCREEN_WIDTH - width) / 2
        draw.text((x, 0), text, font=font, fill=1)
        bits = im.tobytes()  # rows top first, the left pixel in the high bit, each row whole bytes
        self.port.write(b'T' + struct.pack('>HHHHH', 0, y, SCREEN_WIDTH, height, fg) + bits)
        self.port.read(1)  # 'T' once drawn: nothing may follow before (the reader takes the picture in whole first)


def mb(n):
    return '%.1f MB' % (n / (1 << 20)) if n >= (1 << 20) else '%d KB' % (n // 1024)


def wait_for_bootloader(port):
    print('switch the radio on now (within 30 s)')
    port.timeout = 30
    seen = b''
    while not seen.endswith(BOOT_REQUEST):
        b = port.read(1)
        if not b:
            sys.exit('no bootloader: switch the radio off and try again')
        seen = seen[-3:] + b
    port.write(BOOT_ANSWER)
    time.sleep(0.05)


def upload(port, image):
    """As C7000_read_*.py: 256 byte blocks to the RAM, each acknowledged, then the jump to the entry."""
    port.timeout = 2
    image += b'\x00' * (-len(image) % 256)
    for offset in range(0, len(image), 256):
        payload = (LOAD_ADDRESS + offset).to_bytes(4, 'big') + b'\x00\x00\x00\x40' + image[offset:offset + 256]
        port.write(b'\x02\x18' + bytes([sum(payload) & 0xFF]) + b'\x03' + payload)
        if port.read(4) != ACK_OK:
            sys.exit('the bootloader did not take block %d' % (offset // 256))
    entry = LOAD_ADDRESS.to_bytes(4, 'big')
    port.write(b'\x02\x1B' + bytes([sum(entry) & 0xFF]) + b'\x03' + entry)
    if port.read(4) != ACK_OK:
        sys.exit('the bootloader did not start the reader')


def hello(port):
    reply = port.read(6)
    if len(reply) != 6 or reply[:4] != b'DMP1':
        return None
    return struct.unpack('>H', reply[4:])[0]


def speed_up(port, divisor, baud):
    """The reader's UART to baud: its divisor scaled from the one of 115200; checked both ways, else back."""
    new = round(divisor * SLOW / baud)
    actual = divisor * SLOW / new
    if new < 1 or abs(actual - baud) / baud > 0.03:
        print('%d baud not reachable (divisor %d at 115200), staying slow' % (baud, divisor))
        return
    port.write(b'S' + struct.pack('>H', new))
    if port.read(1) != b'S':
        sys.exit('no answer to the speed change')
    # The PC side at the asked speed itself: 250000 is 12 MHz / 48, one the CH340 makes exactly; the radio's own
    # (its clock / 16 / divisor) is only known as near it
    port.baudrate = baud
    time.sleep(0.05)
    port.reset_input_buffer()
    port.write(b'H')
    if hello(port) is not None:
        port.write(b'Y')
        if port.read(1) == b'Y':
            print('%d baud (radio divisor %d)' % (baud, new))
            return
    port.baudrate = SLOW  # the reader goes back to the old divisor after its waits time out
    time.sleep(10)
    port.reset_input_buffer()
    print('%d baud failed, staying at 115200' % baud)


def drain(port):
    """Until the reader has sent the rest of a request: 0.5 s without a byte."""
    timeout = port.timeout
    port.timeout = 0.5
    while port.read(65536):
        pass
    port.timeout = timeout


def jedec_id(port, chip):
    port.write(b'I' + bytes([chip]))
    reply = port.read(4)
    if len(reply) != 4 or reply[0:1] != b'I':
        sys.exit('no answer to the ID request')
    return reply[1:]


def read_blocks(port, chip, address, count):
    """count blocks from address; a damaged block (CRC) is None."""
    port.write(b'R' + bytes([chip]) + struct.pack('>II', address, count))
    blocks = []
    for _ in range(count):
        frame = port.read(1 + BLOCK + 4)
        if len(frame) != 1 + BLOCK + 4 or frame[0:1] != b'D':
            raise IOError('the reader stopped')
        data = frame[1:1 + BLOCK]
        blocks.append(data if zlib.crc32(data) == struct.unpack('>I', frame[1 + BLOCK:])[0] else None)
    return blocks


SPEEDTEST_BAUDS = (1500000, 750000, 500000, 375000, 300000, 250000)  # 24 MHz / 16 / n, and 12 MHz / n for the CH340
SPEEDTEST_BLOCKS = 16


def speedtest(port, chip, divisor):
    """Each speed in turn, back at 115200 in between: 16 blocks in one request, whole ones counted."""
    for baud in SPEEDTEST_BAUDS:
        speed_up(port, divisor, baud)
        if port.baudrate != baud:
            continue
        t0 = time.time()
        try:
            blocks = read_blocks(port, chip, 0, SPEEDTEST_BLOCKS)
            whole = sum(1 for b in blocks if b is not None)
            note = ''
        except IOError:
            whole, note = 0, ', the stream broke'
            drain(port)
        elapsed = time.time() - t0
        print('%7d baud: %2d of %d blocks whole, %.1f KB/s%s' %
              (baud, whole, SPEEDTEST_BLOCKS, SPEEDTEST_BLOCKS * BLOCK / elapsed / 1024, note))
        speed_up(port, divisor, SLOW)


def backup(port, chip, args, size, screen):
    """The chip from args.start, size bytes, to args.file."""
    count = size // BLOCK
    t0 = time.time()
    with open(args.file, 'wb') as f:
        n = 0
        failures = 0  # in a row at the same place
        while n < count:
            if failures >= 10:
                screen.line('title', 'Backup failed', RED)
                screen.line('left', '10 errors at 0x%X' % (args.start + n * BLOCK), RED)
                sys.exit('\n10 failures in a row at 0x%x: stopped' % (args.start + n * BLOCK))
            todo = min(64, count - n)  # 256 KB a request: a lost byte costs one request, not the whole read
            try:
                blocks = read_blocks(port, chip, args.start + n * BLOCK, todo)
            except IOError:
                drain(port)
                failures += 1
                print('\nresync at 0x%x' % (args.start + n * BLOCK))
                continue
            good = 0
            for data in blocks:
                if data is None:
                    break
                f.write(data)
                good += 1
            if good < todo:
                print('\nCRC error at 0x%x, again' % (args.start + (n + good) * BLOCK))
            failures = 0 if good else failures + 1
            n += good
            show_progress(port, screen, n, count, n * BLOCK, size, t0, 'read')
    print('\n%s: %d bytes in %.0f s' % (args.file, size, time.time() - t0))
    screen.line('title', 'Backup done', GREEN)
    screen.line('left', 'in %.0f min' % ((time.time() - t0) / 60), GREEN)



def show_progress(port, screen, done_units, total_units, done_bytes, size, t0, what):
    port.write(b'P' + struct.pack('>HH', done_units, total_units))  # the radio's progress bar
    port.read(1)  # 'P' once drawn: bytes sent meanwhile were lost (the text then drawn in the wrong place)
    rate = max(done_bytes, 1) / max(time.time() - t0, 1e-3)
    left = (size - done_bytes) / rate
    print('\r%s 0x%07x / 0x%07x  %4.0f KB/s  %3.0f s left' % (what, done_bytes, size, rate / 1024, left),
          end='', flush=True)
    screen.line('percent', '%d%%' % (done_units * 100 // max(total_units, 1)))
    screen.line('amount', '%s / %s' % (mb(done_bytes), mb(size)))
    screen.line('speed', '%d baud, %.0f KB/s' % (port.baudrate, rate / 1024), GREY)
    screen.line('left', ('%d min left' % max(1, round(left / 60))) if done_bytes < size else '', GREY)


def sector_crcs(port, chip, address, count):
    """The radio's CRC-32 of count sectors from address, 64 a request."""
    crcs = []
    while len(crcs) < count:
        n = min(64, count - len(crcs))
        port.write(b'C' + bytes([chip]) + struct.pack('>II', address + len(crcs) * BLOCK, n))
        reply = port.read(4 * n)
        if len(reply) != 4 * n:
            drain(port)
            continue
        crcs += list(struct.unpack('>%dI' % n, reply))
    return crcs


def restore(port, chip, args, data, size, screen):
    """data to the chip from args.start: only the sectors that differ, each read back; then all compared again."""
    count = size // BLOCK
    wanted = [zlib.crc32(data[i * BLOCK:(i + 1) * BLOCK]) for i in range(count)]
    skip = 0
    if chip == CHIPS['program'] and not args.boot:
        skip = max(0, min(count, (BOOT_AREA - args.start) // BLOCK))  # the bootloader: only with --boot
        if skip:
            print('below 0x%x (the bootloader) left as it is: --boot to write it' % BOOT_AREA)
    flag = 0x80 if args.boot else 0
    screen.line('title', 'DM-32 restore')
    print('comparing ...')
    have = sector_crcs(port, chip, args.start, count)
    differ = [i for i in range(skip, count) if have[i] != wanted[i]]
    todo = list(range(skip, count)) if args.force else differ
    if todo and todo[0] == 0:  # the first sector last: a header there (the CJK font's) is valid only once all is in
        todo = todo[1:] + todo[:1]
    print('%d of %d sectors differ%s' % (len(differ), count - skip, ', all written (--force)' if args.force else ''))
    screen.line('left', '%d sectors to write' % len(todo), GREY)
    t0 = time.time()
    for k, i in enumerate(todo):
        address = args.start + i * BLOCK
        sector = data[i * BLOCK:(i + 1) * BLOCK]
        for attempt in range(3):
            port.write(b'X' + bytes([chip | flag]) + struct.pack('>I', address) + sector +
                       struct.pack('>I', wanted[i]))
            answer = port.read(1)
            if answer == b'K':
                break
            error = port.read(1) if answer == b'E' else b''
            print('\nsector 0x%x: %s, again' % (address, ('error %d' % error[0]) if error else 'no answer'))
            drain(port)
        else:
            screen.line('title', 'Restore failed', RED)
            screen.line('left', 'sector 0x%X' % address, RED)
            sys.exit('\nsector 0x%x not written: stopped' % address)
        show_progress(port, screen, k + 1, len(todo), (k + 1) * BLOCK, len(todo) * BLOCK, t0, 'write')
    print('\nchecking ...')
    have = sector_crcs(port, chip, args.start, count)
    bad = [args.start + i * BLOCK for i in range(skip, count) if have[i] != wanted[i]]
    if bad:
        screen.line('title', 'Restore failed', RED)
        screen.line('left', '%d sectors differ' % len(bad), RED)
        sys.exit('%d sectors still differ, first 0x%x' % (len(bad), bad[0]))
    print('%d sectors written, all %d the same as the file, %.0f s' % (len(todo), count - skip, time.time() - t0))
    screen.line('title', 'Restore done', GREEN)
    screen.line('left', '%d sectors written' % len(todo), GREEN)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('port')
    ap.add_argument('chip', choices=CHIPS)
    ap.add_argument('file', nargs='?', help='read into it; with --write, written from it; none with --speedtest')
    ap.add_argument('--write', action='store_true', help='the file to the flash: the sectors that differ, read back')
    ap.add_argument('--force', action='store_true', help='with --write: every sector, the same ones too')
    ap.add_argument('--boot', action='store_true', help='with --write: the program flash below 0xC000 too')
    ap.add_argument('--speedtest', action='store_true', help='which speeds the cable carries, nothing saved')
    ap.add_argument('--start', type=lambda x: int(x, 0), default=0)
    ap.add_argument('--size', type=lambda x: int(x, 0), help='default: the whole chip')
    ap.add_argument('--baud', type=int, default=250000)
    args = ap.parse_args()
    if args.start % BLOCK or (args.size or 0) % BLOCK:
        sys.exit('--start and --size: multiples of 4 KB')
    chip = CHIPS[args.chip]

    with open(DUMPER, 'rb') as f:
        image = f.read()
    if not args.speedtest and not args.file:
        sys.exit('the file?')
    data = None
    if args.write:
        with open(args.file, 'rb') as f:
            data = f.read()
        if not data:
            sys.exit('%s: empty' % args.file)
        data += b'\xff' * (-len(data) % BLOCK)  # the last sector filled up as erased
    port = serial.Serial(args.port, SLOW, timeout=2)
    if hasattr(port, 'set_buffer_size'):
        port.set_buffer_size(rx_size=1 << 20)  # Windows: the driver's queue, 4 KB by default, less than a block
    wait_for_bootloader(port)
    upload(port, image)
    port.timeout = 3
    divisor = hello(port)
    if divisor is None:
        sys.exit('the reader did not start')
    if args.speedtest:
        speedtest(port, chip, divisor)
        return
    speed_up(port, divisor, args.baud)

    ident = jedec_id(port, chip)
    print('%s flash: JEDEC ID %s' % (args.chip, ident.hex()))
    size = args.size
    if size is None:
        if ident[2] not in SIZES:
            sys.exit('unknown capacity byte %02x: give --size' % ident[2])
        size = SIZES[ident[2]] - args.start
    if args.write:
        if len(data) > size:
            sys.exit('%s: %d bytes, more than the %d from 0x%x' % (args.file, len(data), size, args.start))
        size = len(data)
    screen = Screen(port)
    screen.line('title', 'DM-32 backup')
    screen.line('chip', CHIP_NAMES[chip], GREY)
    screen.line('id', 'JEDEC %s, %s' % (ident.hex().upper(), mb(size)), GREY)
    screen.line('percent', '0%')

    if args.write:
        restore(port, chip, args, data, size, screen)
    else:
        backup(port, chip, args, size, screen)


if __name__ == '__main__':
    main()
