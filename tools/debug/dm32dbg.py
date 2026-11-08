#!/usr/bin/env python3
"""Debug commands for the DM-32 running our firmware, over the programming cable (CPS link, 'D' requests).

The firmware side is src/services/debug.c. Works with release and debug builds;
with a debug build the log lines are skipped while looking for a reply.

Examples:
  py dm32dbg.py info
  py dm32dbg.py fmr 0x00                  RDA5807M (FM broadcast receiver) register read (chip ID 0x58xx)
  py dm32dbg.py fmw 0x02 0xC001           RDA5807M register write
  py dm32dbg.py fdr 0x0c                  FD6818 register read (from the chip)
  py dm32dbg.py fdw 0x51 0x1000           FD6818 register write
  py dm32dbg.py fda --save chip.txt      FD6818 registers 0x00-0x7f read from the chip in one request, saved to a file
  py dm32dbg.py fdc --save silent.txt     FD6818 register cache (last written values), saved to a file
  py dm32dbg.py fdcdiff silent.txt ok.txt compare two saved files (fdc or fda)
  py dm32dbg.py mr 0x11000128 16         memory read (bytes); --width 1/2/4 reads values with that access width
  py dm32dbg.py mr hrc 64                 addresses can also be symbols of the firmware ELF (--elf)
  py dm32dbg.py mw 0x11000400 0xc3        memory write, 32 bit by default, --width 1/2/4
  py dm32dbg.py mwb 0x30000 "01 02 03"    write bytes
  py dm32dbg.py bits 0x11000080 0x10 0    read-modify-write: clear (value 0) or set the bits of the mask, --width
  py dm32dbg.py call trxSetRxCSS 0 0x8023 call a firmware function with up to 4 arguments, prints r0
  py dm32dbg.py reboot                    restart the radio (jump to the bootloader entry)
  py dm32dbg.py rxcss D023N               Rx CSS of the current channel: none, 88.5, D023N, D023I (not saved)
  py dm32dbg.py txcss none                Tx CSS
  py dm32dbg.py sq --watch 10             squelch state, every 0.25 s for 10 s
"""
import argparse
import struct
import sys
import time

import os
import subprocess

import serial

BOOTLOADER_ENTRY = 0x03001000  # restarts the radio as at power on (DM32_BOOTLOADER_ENTRY in main.h)
HERE = os.path.dirname(os.path.abspath(__file__))
ELF = os.path.join(HERE, '..', '..', 'src', 'build', 'opendmr.elf')
NM = os.path.join(HERE, '..', '..', 'toolchain', 'MinGW', 'csky-elfabiv2-tools', 'bin', 'csky-elfabiv2-nm.exe')


class Radio:
    def __init__(self, port):
        self.p = serial.Serial(port, 115200, timeout=0.5)
        time.sleep(0.2)

    def request(self, data, reply_header, reply_length, timeout=1.5, retries=3):
        """Send a request and return the reply (reply_length bytes starting with reply_header), skipping log text."""
        for _ in range(retries):
            self.p.reset_input_buffer()
            self.p.write(data)
            buf = b''
            end = time.time() + timeout
            while time.time() < end:
                buf += self.p.read(max(1, self.p.in_waiting))
                i = buf.find(reply_header)
                if i >= 0 and len(buf) - i >= reply_length:
                    return buf[i:i + reply_length]
            time.sleep(0.2)
        raise RuntimeError('no reply to %r' % data[:2])

    def debug(self, command, payload=b'', reply_length=2, retries=3):
        return self.request(b'D' + command + payload, b'D' + command, reply_length, retries=retries)

    def fd_read(self, reg):
        r = self.debug(b'r', bytes([reg]), 5)
        return (r[3] << 8) | r[4]

    def fd_write(self, reg, value):
        self.debug(b'w', bytes([reg, (value >> 8) & 0xFF, value & 0xFF]))

    def fd_read_range(self, first, count):
        r = self.debug(b'f', bytes([first, count]), 4 + count * 2)
        return {first + i: (r[4 + i * 2] << 8) | r[5 + i * 2] for i in range(r[3])}

    def fd_cache(self):
        r = self.debug(b'c', b'', 2 + 255 * 3)
        cache = {}
        for reg in range(255):
            written, hi, lo = r[2 + reg * 3: 5 + reg * 3]
            if written:
                cache[reg] = (hi << 8) | lo
        return cache

    def mem_read(self, address, length):
        return self.debug(b'm', struct.pack('>IH', address, length), 2 + length)[2:]

    def mem_write_bytes(self, address, data):
        self.debug(b'W', struct.pack('>IH', address, len(data)) + data)

    def value_read(self, address, width):
        return struct.unpack('>I', self.debug(b'v', struct.pack('>IB', address, width), 6)[2:6])[0]

    def value_write(self, address, width, value):
        self.debug(b'V', struct.pack('>IBI', address, width, value & 0xFFFFFFFF))

    def bits(self, address, width, mask, value):
        r = self.debug(b'b', struct.pack('>IBII', address, width, mask, value), 10)
        return struct.unpack('>II', r[2:10])

    def call(self, address, args):
        args = (list(args) + [0, 0, 0, 0])[:4]
        # Sent once: a called function may not return (a reboot), and sending it again would run it again
        r = self.debug(b'x', struct.pack('>IIIII', address, *[a & 0xFFFFFFFF for a in args]), 6, retries=1)
        return struct.unpack('>I', r[2:6])[0]

    def reboot(self):
        self.p.write(b'Dx' + struct.pack('>IIIII', BOOTLOADER_ENTRY, 0, 0, 0, 0))
        self.p.flush()

    def set_css(self, rx, tone):
        self.debug(b'R' if rx else b'T', struct.pack('>H', tone))

    def squelch(self):
        r = self.debug(b'q', b'', 11)
        signal, amp, led = r[2], r[3], r[4]
        reg0c, rx_tone, tx_tone = struct.unpack('>HHH', r[5:11])
        return signal, amp, led, reg0c, rx_tone, tx_tone

    def info(self):
        r = self.request(b'R' + bytes([9]) + struct.pack('>IH', 0, 64), b'R\x00', 3 + 46)
        d = r[3:]
        struct_version, radio_type = struct.unpack('<II', d[:8])
        return {'structVersion': struct_version, 'radioType': radio_type,
                'git': d[8:24].split(b'\0')[0].decode(errors='replace'),
                'build': d[24:40].split(b'\0')[0].decode(errors='replace'),
                'flashId': hex(struct.unpack('<I', d[40:44])[0])}


def parse_css(text):
    """none / 88.5 / D023N / D023I -> codeplug tone value"""
    t = text.strip().upper()
    if t in ('NONE', 'OFF', ''):
        return 0xFFFF
    if t.startswith('D'):
        inverted = t.endswith('I')
        digits = t[1:].rstrip('NI')
        return 0x8000 | int(digits, 16) | (0x4000 if inverted else 0)
    return int(round(float(t) * 10))


def format_css(tone):
    if tone in (0xFFFF, 0):
        return 'none'
    if tone & 0x8000:
        return 'D%03X%s' % (tone & 0x0FFF, 'I' if tone & 0x4000 else 'N')
    return '%.1f' % (tone / 10)


class Symbols:
    """Firmware symbols from the ELF (csky nm), so addresses can be given by name."""
    def __init__(self, elf):
        self.elf = elf
        self.table = None

    def resolve(self, text):
        try:
            return int(text, 0)
        except ValueError:
            pass
        if self.table is None:
            if not os.path.exists(self.elf):
                raise SystemExit('no ELF %s for the symbol %s (use --elf)' % (self.elf, text))
            out = subprocess.run([NM, self.elf], capture_output=True, text=True).stdout
            self.table = {}
            for line in out.splitlines():
                parts = line.split()
                if len(parts) == 3:
                    self.table[parts[2]] = int(parts[0], 16)
        if text not in self.table:
            raise SystemExit('unknown symbol %s' % text)
        return self.table[text]


def read_cache_file(path):
    cache = {}
    for line in open(path):
        if '=' in line:
            reg, value = line.split('=')
            cache[int(reg, 16)] = int(value, 16)
    return cache


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', default='COM6')
    ap.add_argument('--elf', default=ELF,
                    help='firmware ELF for symbol names (the one flashed)')
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('info')
    sub.add_parser('reboot')
    s = sub.add_parser('fmr'); s.add_argument('reg', type=lambda x: int(x, 0))
    s = sub.add_parser('fmw'); s.add_argument('reg', type=lambda x: int(x, 0)); s.add_argument('value', type=lambda x: int(x, 0))
    s = sub.add_parser('fdr'); s.add_argument('reg', type=lambda x: int(x, 0))
    s = sub.add_parser('fdw'); s.add_argument('reg', type=lambda x: int(x, 0)); s.add_argument('value', type=lambda x: int(x, 0))
    s = sub.add_parser('fda'); s.add_argument('--save')
    s.add_argument('--first', type=lambda x: int(x, 0), default=0); s.add_argument('--count', type=lambda x: int(x, 0), default=0x80)
    s = sub.add_parser('fdc'); s.add_argument('--save')
    s = sub.add_parser('fdcdiff'); s.add_argument('a'); s.add_argument('b')
    s = sub.add_parser('mr'); s.add_argument('addr'); s.add_argument('length', type=lambda x: int(x, 0), nargs='?', default=4)
    s.add_argument('--width', type=int, choices=(1, 2, 4))
    s = sub.add_parser('mw'); s.add_argument('addr'); s.add_argument('value', type=lambda x: int(x, 0))
    s.add_argument('--width', type=int, choices=(1, 2, 4), default=4)
    s = sub.add_parser('mwb'); s.add_argument('addr'); s.add_argument('data', help='hex bytes, e.g. "01 02 ff"')
    s = sub.add_parser('bits'); s.add_argument('addr'); s.add_argument('mask', type=lambda x: int(x, 0))
    s.add_argument('value', type=lambda x: int(x, 0)); s.add_argument('--width', type=int, choices=(1, 2, 4), default=4)
    s = sub.add_parser('call'); s.add_argument('addr'); s.add_argument('args', nargs='*', type=lambda x: int(x, 0))
    s = sub.add_parser('rxcss'); s.add_argument('tone')
    s = sub.add_parser('txcss'); s.add_argument('tone')
    s = sub.add_parser('sq'); s.add_argument('--watch', type=float, default=0)
    args = ap.parse_args()

    if args.cmd == 'fdcdiff':
        a, b = read_cache_file(args.a), read_cache_file(args.b)
        for reg in sorted(set(a) | set(b)):
            if a.get(reg) != b.get(reg):
                fa = '%04x' % a[reg] if reg in a else '----'
                fb = '%04x' % b[reg] if reg in b else '----'
                print('%02x: %s %s' % (reg, fa, fb))
        return

    radio = Radio(args.port)
    symbols = Symbols(args.elf)
    if args.cmd == 'info':
        print(radio.info())
    elif args.cmd == 'reboot':
        radio.reboot()
        print('reboot sent')
    elif args.cmd == 'fdr':
        print('%02x=%04x' % (args.reg, radio.fd_read(args.reg)))
    elif args.cmd == 'fdw':
        radio.fd_write(args.reg, args.value)
        print('%02x=%04x written, read back %04x' % (args.reg, args.value, radio.fd_read(args.reg)))
    elif args.cmd == 'fmr':
        r = radio.debug(b'Y', bytes([args.reg]), 5)
        print('%02x=%04x' % (args.reg, (r[3] << 8) | r[4]) if r[2] else 'no answer from the RDA5807M')
    elif args.cmd == 'fmw':
        r = radio.debug(b'Z', bytes([args.reg, (args.value >> 8) & 0xFF, args.value & 0xFF]), 3)
        print('%02x=%04x written' % (args.reg, args.value) if r[2] else 'no answer from the RDA5807M')
    elif args.cmd in ('fda', 'fdc'):
        values = radio.fd_read_range(args.first, args.count) if args.cmd == 'fda' else radio.fd_cache()
        lines = ['%02x=%04x' % (reg, value) for reg, value in sorted(values.items())]
        print(' '.join(lines))
        if args.save:
            open(args.save, 'w').write('\n'.join(lines) + '\n')
            print('saved', args.save)
    elif args.cmd == 'mr':
        addr = symbols.resolve(args.addr)
        if args.width:
            for a in range(addr, addr + args.length, args.width):
                print('%08x: %0*x' % (a, args.width * 2, radio.value_read(a, args.width)))
        else:
            data = radio.mem_read(addr, args.length)
            for i in range(0, len(data), 16):
                print('%08x: %s' % (addr + i, data[i:i + 16].hex(' ')))
    elif args.cmd == 'mw':
        addr = symbols.resolve(args.addr)
        radio.value_write(addr, args.width, args.value)
        print('%08x=%0*x written, read back %0*x' % (addr, args.width * 2, args.value, args.width * 2,
                                                       radio.value_read(addr, args.width)))
    elif args.cmd == 'mwb':
        addr = symbols.resolve(args.addr)
        data = bytes.fromhex(args.data)
        radio.mem_write_bytes(addr, data)
        print('%d bytes written at %08x' % (len(data), addr))
    elif args.cmd == 'bits':
        addr = symbols.resolve(args.addr)
        old, new = radio.bits(addr, args.width, args.mask, args.value)
        print('%08x: %0*x -> %0*x' % (addr, args.width * 2, old, args.width * 2, new))
    elif args.cmd == 'call':
        addr = symbols.resolve(args.addr)
        print('r0=%08x' % radio.call(addr, args.args))
    elif args.cmd in ('rxcss', 'txcss'):
        tone = parse_css(args.tone)
        radio.set_css(args.cmd == 'rxcss', tone)
        print('%s set to %s (0x%04x)' % (args.cmd, format_css(tone), tone))
    elif args.cmd == 'sq':
        end = time.time() + args.watch
        while True:
            signal, amp, led, reg0c, rx, tx = radio.squelch()
            print('%s signal=%d amp=%02x green=%d 0x0C=%04x rx=%s tx=%s' %
                  (time.strftime('%H:%M:%S'), signal, amp, led, reg0c, format_css(rx), format_css(tx)), flush=True)
            if time.time() >= end:
                break
            time.sleep(0.25)


if __name__ == '__main__':
    sys.exit(main())
