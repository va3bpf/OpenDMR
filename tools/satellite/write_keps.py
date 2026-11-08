#!/usr/bin/env python3
"""Writes the amateur satellites (orbits and frequencies) to the DM-32, as the OpenGD77 CPS does: the custom data block
type 3 (CODEPLUG_CUSTOM_DATA_TYPE_SATELLITE_TLE) after the "OpenGD77" header of the flash (codeplug.c), read by
loadKeps() in menuSatelliteScreen.c. Over the programming cable, the radio running our firmware.

  py tools/satellite/write_keps.py COM6                      TLEs from celestrak.org, frequencies of the CPS
  py tools/satellite/write_keps.py COM6 --tle amateur.txt    TLEs from a file (3 lines per satellite)

Frequencies: satellites.txt of the OpenGD77 CPS (default: its install folder). The keps get old: write them again
every week or two (a few minutes off for the ISS after a month).
"""
import argparse
import os
import struct
import sys
import time
import urllib.error
import urllib.request

import serial

CPS_SATELLITES = r'C:\Program Files (x86)\OpenGD77CPS\satellites.txt'
CUSTOM_DATA = 0x20000            # logical flash address of the "OpenGD77" custom data (FLASH_ADDRESS_OFFSET + 0)
BLOCK_TYPE = 3                   # CODEPLUG_CUSTOM_DATA_TYPE_SATELLITE_TLE
BLOCK_LEN = 2520                 # sizeof(codeplugSatelliteCuctsomDataUnion_t)
NUM_SATELLITES = 25
RECORD = 100                     # sizeof(codeplugSatelliteData_t)
SECTOR = 4096
TLE_CHARS = '0123456789. +-*'    # satellite.c DECOMPRESSION_LOOKUP


class Radio:
    def __init__(self, port):
        self.p = serial.Serial(port, 115200, timeout=0.5)
        time.sleep(0.2)

    def _request(self, data, length, header):
        for _ in range(3):
            self.p.reset_input_buffer()
            self.p.write(data)
            buf = b''
            end = time.time() + 3
            while time.time() < end and len(buf) < length:
                buf += self.p.read(max(1, self.p.in_waiting))
            if buf[:1] == b'-':
                raise RuntimeError('the radio refused %r' % data[:2])
            if buf[:len(header)] == header and len(buf) >= length:
                return buf[:length]
            time.sleep(0.2)
        raise RuntimeError('no reply to %r' % data[:2])

    def read(self, address, length):
        out = b''
        while len(out) < length:
            n = min(1024, length - len(out))
            out += self._request(b'R\x01' + struct.pack('>IH', address + len(out), n), 3 + n, b'R')[3:]
        return out

    def write_sector(self, sector, data):
        self._request(b'X\x01' + struct.pack('>I', sector)[1:], 2, b'X\x01')
        for i in range(0, SECTOR, 1024):
            self._request(b'X\x02' + struct.pack('>IH', sector * SECTOR + i, 1024) + data[i:i + 1024], 2, b'X\x02')
        self._request(b'X\x03', 2, b'X\x03')


def compress(text, length):
    """TLE characters, 2 per byte (first in the high nibble)."""
    text = text.ljust(length * 2)[:length * 2]
    out = bytearray()
    for i in range(0, len(text), 2):
        out.append((TLE_CHARS.index(text[i]) << 4) | TLE_CHARS.index(text[i + 1]))
    return bytes(out)


def record(name, line1, line2, f):
    # line 1: epoch year (2), epoch day (12), decay (10); line 2: inclination (8), RAAN (8), eccentricity (7),
    # argument of perigee (8), mean anomaly (8), mean motion (11), revolution number (5)
    tle1 = line1[18:20] + line1[20:32] + line1[33:43]
    tle2 = line2[8:16] + line2[17:25] + line2[26:33] + line2[34:42] + line2[43:51] + line2[52:63] + line2[63:68]
    hz = lambda mhz: int(round(float(mhz) * 1e6))
    tone = lambda t: int(round(float(t) * 10))
    data = name.encode('ascii')[:8].ljust(8)
    data += compress(tle1, 12) + compress(tle2, 28)
    data += struct.pack('<IIHHIIII', hz(f['Rx1']), hz(f['Tx1']), tone(f['CTCSS']), tone(f['ArmCTCSS']),
                        hz(f['Rx2']), hz(f['Tx2']), hz(f['Rx3']), hz(f['Tx3']))
    data += bytes(24)  # AdditionalData: the APRS paths of aprs.c, not set
    assert len(data) == RECORD
    return data


def load_frequencies(path):
    sats = []
    lines = [l for l in open(path, encoding='utf-8-sig').read().splitlines() if l.strip()]
    keys = [k.strip().replace(' ', '').replace('#', '') for k in lines[0].split(',')]
    for line in lines[1:]:
        values = [v.strip() for v in line.split(',')]
        row = dict(zip(keys, values))
        row['catalog'] = row['Cat'].rstrip('U')
        sats.append(row)
    return sats


def load_tles(sats, tle_file):
    tles = {}
    if tle_file:
        lines = [l.rstrip() for l in open(tle_file, encoding='utf-8') if l.strip()]
        for i in range(0, len(lines) - 2, 3):
            tles[lines[i + 1][2:7].strip()] = (lines[i + 1], lines[i + 2])
        return tles
    for s in sats:
        url = 'https://celestrak.org/NORAD/elements/gp.php?CATNR=%s&FORMAT=TLE' % s['catalog']
        try:
            text = urllib.request.urlopen(url, timeout=30).read().decode('ascii', 'replace')
        except urllib.error.URLError as e:
            print('%-8s %s' % (s['Name'], e))
            continue
        lines = [l.rstrip() for l in text.splitlines() if l.strip()]
        if len(lines) >= 3 and lines[1].startswith('1 ') and lines[2].startswith('2 '):
            tles[s['catalog']] = (lines[1], lines[2])
    return tles


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('port')
    ap.add_argument('--frequencies', default=CPS_SATELLITES)
    ap.add_argument('--tle', help='TLE file instead of celestrak.org')
    args = ap.parse_args()

    sats = load_frequencies(args.frequencies)
    tles = load_tles(sats, args.tle)
    body = b''
    count = 0
    for s in sats:
        if s['catalog'] not in tles:
            print('%-8s no TLE, left out' % s['Name'])
            continue
        if count == NUM_SATELLITES:
            print('%-8s left out: %d satellites at most' % (s['Name'], NUM_SATELLITES))
            continue
        line1, line2 = tles[s['catalog']]
        body += record(s['Name'], line1, line2, s)
        count += 1
        print('%-8s epoch 20%s day %s' % (s['Name'], line1[18:20], line1[20:32].strip()))
    body = body.ljust(BLOCK_LEN, b'\0')

    radio = Radio(args.port)
    region = radio.read(CUSTOM_DATA, 0x3000)
    if region[:8] != b'OpenGD77':
        sys.exit('no OpenGD77 custom data header at 0x%X: write a codeplug with the CPS first' % CUSTOM_DATA)

    # The satellite block, or the first empty one
    pos = 12
    while True:
        dtype, dlen = struct.unpack('<II', region[pos:pos + 8])
        if dtype == BLOCK_TYPE:
            if dlen != BLOCK_LEN:
                sys.exit('satellite block of %d bytes, %d expected' % (dlen, BLOCK_LEN))
            break
        if dlen in (0, 0xFFFFFFFF):
            break
        pos += 8 + dlen
        if pos + 8 + BLOCK_LEN > len(region):
            sys.exit('no room for the satellites in the custom data')

    new = bytearray(region)
    new[pos:pos + 8 + BLOCK_LEN] = struct.pack('<II', BLOCK_TYPE, BLOCK_LEN) + body
    for i in range(0, len(new), SECTOR):
        if new[i:i + SECTOR] != region[i:i + SECTOR]:
            radio.write_sector((CUSTOM_DATA + i) // SECTOR, bytes(new[i:i + SECTOR]))
    if radio.read(CUSTOM_DATA + pos, 8 + BLOCK_LEN) != bytes(new[pos:pos + 8 + BLOCK_LEN]):
        sys.exit('read back differs')
    print('%d satellites written at 0x%X (block at offset 0x%X); open the satellite screen again'
          % (count, CUSTOM_DATA, pos))


if __name__ == '__main__':
    main()
