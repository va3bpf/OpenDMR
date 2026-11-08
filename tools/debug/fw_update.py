#!/usr/bin/env python3
"""Firmware update over the programming cable, without the bootloader's upgrade mode (PTT + SK1).

  py tools/debug/fw_update.py [--port COM6] [src/build/opendmr.bin]

The running firmware (src/, with services/serial_update.c) gets the debug request 'D' 'U' "UPD!": it turns
the receiver off and stops everything (keys, knob, PTT, power switch: the interrupts are off), then its loader takes
the update program (updater/updater.bin, built from updater.c for the fixed address of that RAM, 0x44400, by
updater/build.sh; kept in git, so no compiler is needed here) and starts it. That program
shows a progress bar and writes the program flash as this tool asks: each 4 KB sector of the firmware area is
compared by CRC-32 and written only when it differs, then read back. At the end the radio restarts into the new
firmware.

Only a firmware with that loader is written (its marker in the image), so the next update works the same way.
Not while transmitting (the radio refuses). The bootloader (below 0xC000) is never written: if anything goes wrong,
the upgrade mode and the stock firmware loader still work.
"""
import argparse
import os
import struct
import sys
import time
import zlib

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'backup'))
from dm32_dump import GREEN, RED, Screen  # noqa: E402  the radio's screen: text rendered here
UPDATER = os.path.join(HERE, 'updater', 'updater.bin')
LOADER_MARKER = b'DM32-SERIAL-UPDATE-LOADER-1'  # src/services/serial_update.c
DEFAULT_BIN = os.path.join(HERE, '..', '..', 'src', 'build', 'opendmr.bin')
FLASH_BASE = 0xC000     # the firmware area of the program flash (opendmr.bin starts there)
FLASH_END = 0xDE000
SECTOR = 4096
ERRORS = {1: 'the sector came in damaged', 2: 'address outside the firmware area', 3: 'read back different',
          4: 'the flash stayed busy', 5: 'the request was cut short'}


UART_CLOCK = 84000000 // 16  # the radio's UART: 84 MHz APB, baud = UART_CLOCK / divisor
FAST_DIVISORS = (21,)  # 250000 baud. 375000 lost bytes from the radio to the PC now and then (the green radio: the
                       # update stopped after 7 sectors; tools/backup/dm32_dump.py measured it too), 250000 never
PROBE_DIVISORS = (14, 16, 21)  # where a radio left in the update by an older run may be: 375000, 328125, 250000
UPDATER_AREA = 0x44400  # updater/updater.ld: the update area, which the firmware's loader gives the program


class Updater:
    def __init__(self, port):
        self.p = serial.Serial(port, 115200, timeout=1.0)

    def read(self, n, timeout=5.0):
        data = b''
        end = time.time() + timeout
        while len(data) < n and time.time() < end:
            data += self.p.read(n - len(data))
        if len(data) < n:
            raise RuntimeError('the radio did not answer (got %r)' % data)
        return data

    def wait_for(self, marker, timeout):
        data = b''
        end = time.time() + timeout
        while time.time() < end:
            data += self.p.read(max(1, self.p.in_waiting))
            if marker in data:
                return data
        return None

    def after(self, data, marker, n, timeout=3.0):
        """The n bytes after marker in data (a bytearray, read further into it as needed)."""
        end = time.time() + timeout
        while time.time() < end:
            k = data.find(marker)
            if k >= 0 and len(data) >= k + len(marker) + n:
                return bytes(data[k + len(marker):k + len(marker) + n])
            data += self.p.read(max(1, self.p.in_waiting))
        return None

    def start(self):
        """The radio into the update program: through the firmware's loader, or already there (a run cut short)."""
        self.p.reset_input_buffer()
        self.p.write(b'DUUPD!')
        data = bytearray(self.wait_for(b'DU', 3.0) or b'')
        if self.after(data, b'DU', 1, timeout=1.0) == bytes([0]):
            raise RuntimeError('the radio refused: transmitting')
        area = self.after(data, b'UPL1', 8) if data else None
        if area is None:
            data = self.probe()  # in the loader or the update program already?
            if data is None:
                raise RuntimeError('no answer: is the radio on, with this firmware, and the cable in?')
            data = bytearray(data)
            if self.after(data, b'UP', 2) == b'D1':
                return
            area = self.after(data, b'UPL1', 8)
        area, size = struct.unpack('>II', area)
        image = open(UPDATER, 'rb').read()
        if area != UPDATER_AREA or len(image) + SECTOR + 256 > size:
            raise RuntimeError('the firmware gives 0x%X (%d bytes), the update program needs 0x%X (%d bytes)'
                               % (area, size, UPDATER_AREA, len(image) + SECTOR + 256))
        self.p.write(b'L' + struct.pack('>II', area, len(image)) + image + struct.pack('>I', zlib.crc32(image)))
        reply = self.read(1)
        if reply != b'K':
            raise RuntimeError('the loader refused the update program: %r' % (reply + self.p.read(1)))
        self.p.write(b'J' + struct.pack('>I', area))
        if self.read(1) != b'J' or self.wait_for(b'UPD1', 3.0) is None:
            raise RuntimeError('the update program did not start')

    def probe(self):
        """'H' at the usual speed, then at the fast ones (an update cut short leaves the radio there)."""
        for baud in (115200,) + tuple(UART_CLOCK // d for d in PROBE_DIVISORS):
            self.p.baudrate = baud
            self.p.reset_input_buffer()
            self.p.write(b'H')
            data = self.wait_for(b'UP', 0.7)
            if data is not None:
                return data
        self.p.baudrate = 115200
        return None

    def speed_up(self):
        """The fastest speed the cable takes (the update program goes back to the old one if 'H' doesn't come)."""
        for divisor in FAST_DIVISORS:
            if self.p.baudrate == UART_CLOCK // divisor:
                return
            if self.p.baudrate > UART_CLOCK // divisor:
                continue
            old = self.p.baudrate
            self.p.reset_input_buffer()
            self.p.write(b'S' + struct.pack('>H', divisor))
            if self.read(1) != b'S':
                raise RuntimeError('bad reply to S')
            time.sleep(0.02)
            self.p.baudrate = UART_CLOCK // divisor
            self.p.reset_input_buffer()
            self.p.write(b'H')
            time.sleep(0.2)
            if self.p.read(self.p.in_waiting or 1) == b'UPD1':
                self.p.write(b'Y')  # both ways sound: the radio keeps the speed
                if self.wait_for(b'Y', 0.5) is not None:
                    return
            self.p.baudrate = old  # the radio goes back by itself
            time.sleep(4.5)  # up to two waits of about 2 s
            self.p.reset_input_buffer()
            self.p.write(b'H')
            if self.wait_for(b'UPD1', 1.0) is None:
                raise RuntimeError('lost the radio after trying %d baud' % (UART_CLOCK // divisor))

    def sector_crc(self, address):
        self.p.write(b'C' + struct.pack('>I', address))
        reply = self.read(5)
        if reply[0:1] != b'C':
            raise RuntimeError('bad reply to C: %r' % reply)
        return struct.unpack('>I', reply[1:])[0]

    def write_sector(self, address, data):
        for attempt in range(3):
            self.p.write(b'W' + struct.pack('>I', address) + data + struct.pack('>I', zlib.crc32(data)))
            reply = self.read(1, timeout=10.0)
            if reply == b'K':
                return
            if reply != b'E':
                raise RuntimeError('bad reply to W: %r' % reply)
            error = self.read(1)[0]
            if error not in (1, 5):  # damaged on the way: send it again
                raise RuntimeError('sector 0x%X: %s' % (address, ERRORS.get(error, error)))
        raise RuntimeError('sector 0x%X: %s' % (address, ERRORS.get(error, error)))

    def progress(self, done, total):
        self.p.write(b'P' + struct.pack('>HH', done, total))
        if self.read(1) != b'P':
            raise RuntimeError('bad reply to P')

    def restart(self):
        """The watchdog resets the radio about 3 s after 'B' and stays on: turned off here over the cable as soon as
        the firmware answers (a firmware older than the watchdog restart would be reset again and again)."""
        self.p.write(b'B')
        self.read(1)
        self.p.baudrate = 115200
        end = time.time() + 15
        while time.time() < end:
            self.p.reset_input_buffer()
            ok = True
            for address, value in ((0x14010000, 0x5ada7200), (0x14010010, 0), (0x14010008, 0xC0000000),
                                   (0x14010000, 0)):  # WDG_LOCK open, WDG_EN off, the 60 s timeout, locked
                self.p.write(b'DV' + struct.pack('>IBI', address, 4, value))
                if self.wait_for(b'DV', 0.3) is None:
                    ok = False
                    break
            if ok:
                return
        raise RuntimeError('the radio did not come back: the watchdog may reset it again and again (power it off)')


def bar(done, total, written, width=40):
    filled = (width * done) // total
    sys.stdout.write('\r[%s%s] %3d%%  %d/%d sectors, %d written' % ('#' * filled, '.' * (width - filled),
                                                                  (100 * done) // total, done, total, written))
    sys.stdout.flush()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('bin', nargs='?', default=DEFAULT_BIN, help='opendmr.bin (not the BFUV32 file)')
    ap.add_argument('--port', default='COM6')
    ap.add_argument('--all', action='store_true', help='write every sector, the same ones too')
    args = ap.parse_args()

    image = open(args.bin, 'rb').read()
    if image[:9] == b'BFUV32-V2':
        image = image[0x100:]  # the upgrade mode's header
    if LOADER_MARKER not in image:
        sys.exit('not our firmware with the cable update loader: not written')
    image += b'\xFF' * (-len(image) % SECTOR)
    if FLASH_BASE + len(image) > FLASH_END:
        sys.exit('too big for the firmware area: %d bytes' % len(image))
    sectors = [(FLASH_BASE + i, image[i:i + SECTOR]) for i in range(0, len(image), SECTOR)]

    u = Updater(args.port)
    t0 = time.time()
    u.start()
    u.speed_up()
    print('the radio is in the update at %d baud (%d sectors, %s)' % (u.p.baudrate, len(sectors),
                                                                       os.path.basename(args.bin)))
    screen = Screen(u.p)
    screen.line('title', 'Firmware update')
    screen.line('chip', 'Do not power off', RED)
    screen.line('percent', '0%')
    written = 0
    for n, (address, data) in enumerate(sectors):
        if args.all or u.sector_crc(address) != zlib.crc32(data):
            u.write_sector(address, data)
            written += 1
        u.progress(n + 1, len(sectors))
        bar(n + 1, len(sectors), written)
        percent = (n + 1) * 100 // len(sectors)
        if (percent // 5 != n * 100 // len(sectors) // 5) or (n + 1 == len(sectors)):  # each 5 %: 1.7 KB a time
            screen.line('percent', '%d%%' % percent)
    # all written: checked once more before the restart
    bad = [a for a, d in sectors if u.sector_crc(a) != zlib.crc32(d)]
    if bad:
        sys.exit('\nstill different: %s (the radio stays in the update: run again)' % ', '.join(hex(a) for a in bad))
    screen.line('title', 'Update done', GREEN)
    screen.line('chip', 'Restarting', GREEN)
    u.restart()
    print('\ndone in %.0f s, %d sectors written; the radio restarts' % (time.time() - t0, written))


if __name__ == '__main__':
    main()
