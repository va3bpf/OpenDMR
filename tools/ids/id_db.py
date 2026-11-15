#!/usr/bin/env python3
"""The DMR ID database with the callers' places (src/services/dmr_ids.c): built from RadioID's user list, written over
the programming cable.

  py tools/ids/id_db.py build [--csv user.csv] [--countries "Canada,United States"] [-o ids.bin]
  py tools/ids/id_db.py check [ids.bin] [--csv user.csv]
  py tools/ids/id_db.py write COM6 [ids.bin]
  py tools/ids/id_db.py countries [--csv user.csv]

build downloads https://radioid.net/static/user.csv (or takes --csv) and packs it; --countries keeps those countries
only (a smaller database, written faster). write sends it to the radio as the CPS writes a codeplug, a 4 KB sector at a
time (about 8.5 KB/s: the whole list takes some 7 minutes); the header goes last, so the radio never reads half of it.
countries makes src/services/dmr_countries.c: the country of each ID's first 3 digits, built into the firmware (a
caller's country without any database).

Layout, at the end of the firmware's flash window (as far from the OpenGD77 database as can be): the header in the last
sector (HEADER_AT), the rest below it from its base (a 4 KB boundary). Little endian.

  header: "ODID", version, 3 reserved, records, blocks, countries, base, size (from base to the header), then the
          offsets from the base of: the block index, the blocks, the prefix map, the country names, the country
          directory, the names, the places of other countries; then the field classes (5 x 4 widths)
  block index: the first ID of each block (u32)
  block (4 KB): 0xD1, 0, record count (u16), then the records, bit packed (most significant bit first)
  prefix map: 1000 bytes, the country of each ID's first 3 digits (0xFF: none)
  country directory: per country, the offsets of its lists: callsign prefixes, states, places
  list: count (u32), the offset of each 16th entry from the list (u32), then the entries: shared bytes with the one
        before (u8, 0 for every 16th), length (u8), the bytes
  places of a country: state number (u16 big endian, 0xFFFF none) + city, sorted
  other places: country (u8) << 24 | its place number

  record: [id - previous id] [callsign] [name] [place]
    callsign: 0 the previous record's, 1 a literal (length u8 + bytes), 2+ that prefix of the country's (by use) then
              the letters after it: count (3 bits) + base 26 (0 / 5 / 10 / 15 / 19 bits)
    name:     0 a literal, 1+ a name of the names list (by use)
    place:    0 the previous record's, 1 another country's (then its number in the other places), 2+ the country's
  A number is a class (2 bits) and that class's width of bits, the classes of each field in the header.
  The country of a record: the prefix map of its ID.
"""
import argparse
import collections
import csv
import io
import itertools
import os
import re
import struct
import sys
import time
import unicodedata
import urllib.request
import zlib

URL = 'https://radioid.net/static/user.csv'
HERE = os.path.dirname(os.path.abspath(__file__))
WINDOW_SIZE = 0x900000           # src/drivers/flash.h FLASH_WINDOW_SIZE
HEADER_AT = WINDOW_SIZE - 0x1000  # src/services/dmr_ids.c NEW_HEADER
MAX_SIZE = 0x400000              # below: the OpenGD77 database
SECTOR = 0x1000
BLOCK = 0x1000
CHUNK_WRITE = 1024               # bytes of an 'X' 2 (the radio's request buffer: 1536)
UART_CLOCK = 84000000 // 16      # the radio's UART: baud = UART_CLOCK / divisor
SPEED_DIVISORS = (7, 14, 21)     # 750000, 375000, 250000 baud (12 MHz / n for the CH340 too)
SPEED_BACK_S = 2.5               # an unconfirmed speed: the radio is back at 115200 after 2 s
CHUNK = 16
SUFFIX_BITS = [0, 5, 10, 15, 19]
FIELDS = ('delta', 'call', 'name', 'place', 'other')
CALL = re.compile(r'^([A-Z0-9]*[0-9])([A-Z]{0,4})$')


def ascii_fold(text):
    """Latin letters without their accents (the fonts have printable ASCII; the flash font CJK, Greek, Cyrillic)."""
    out = []
    for ch in unicodedata.normalize('NFKD', text.replace('ß', 'ss').replace('ø', 'o').replace('Ø', 'O')
                                    .replace('ł', 'l').replace('Ł', 'L').replace('đ', 'd').replace('Đ', 'D')
                                    .replace('æ', 'ae').replace('Æ', 'AE').replace('ı', 'i')):
        if unicodedata.combining(ch):
            continue
        out.append(ch)
    return ' '.join(''.join(out).split())


def clip(text, limit=60):
    data = text.encode()
    while len(data) > limit:
        text = text[:-1]
        data = text.encode()
    return text


def load(path, countries):
    if path:
        raw = open(path, 'rb').read()
    else:
        print('downloading', URL)
        req = urllib.request.Request(URL, headers={'User-Agent': 'Mozilla/5.0'})
        raw = urllib.request.urlopen(req, timeout=120).read()
    rows = {}
    for row in csv.reader(io.StringIO(raw.decode('utf-8', errors='replace'))):
        if len(row) < 7 or not row[0].isdigit():
            continue
        rid = int(row[0])
        if not 1000000 <= rid <= 9999999:
            continue
        call = row[1].strip().upper()
        name, city, state, country = (clip(ascii_fold(c.strip())) for c in (row[2], row[4], row[5], row[6]))
        if city.lower() == 'none':
            city = ''
        rows[rid] = (rid, call, name, city, state, country)
    rows = [rows[k] for k in sorted(rows)]
    if countries:
        keep = {c.strip() for c in countries.split(',')}
        rows = [x for x in rows if x[5] in keep]
    return rows


class Bits:
    def __init__(self):
        self.v, self.n = 0, 0

    def put(self, value, width):
        assert 0 <= value < (1 << width) or (width == 0 and value == 0), (value, width)
        self.v = (self.v << width) | value
        self.n += width

    def copy(self):
        b = Bits()
        b.v, b.n = self.v, self.n
        return b

    def bytes(self):
        return (self.v << ((-self.n) % 8)).to_bytes((self.n + 7) // 8, 'big')


def best_classes(values):
    import bisect
    vs = sorted(values) or [0]
    best = None
    for ws in itertools.combinations(range(0, 25), 4):
        if sum(1 << w for w in ws) <= vs[-1]:
            continue
        cost, base, lo = 0, 0, 0
        for w in ws:
            hi = bisect.bisect_left(vs, base + (1 << w))
            cost += (hi - lo) * (2 + w)
            lo, base = hi, base + (1 << w)
        if best is None or cost < best[0]:
            best = (cost, ws)
    return list(best[1])


def put_number(b, classes, value):
    base = 0
    for i, w in enumerate(classes):
        if value < base + (1 << w):
            b.put(i, 2)
            b.put(value - base, w)
            return
        base += 1 << w
    raise ValueError(value)


def put_text(b, text):
    data = text.encode()
    b.put(len(data), 8)
    for ch in data:
        b.put(ch, 8)


def string_list(entries):
    """count, the offsets of each CHUNK-th entry, the front coded entries"""
    body = bytearray()
    offsets = []
    prev = b''
    for i, e in enumerate(entries):
        if i % CHUNK == 0:
            offsets.append(len(body))
            shared = 0
        else:
            shared = 0
            while shared < min(len(prev), len(e), 255) and prev[shared] == e[shared]:
                shared += 1
        rest = e[shared:]
        assert len(rest) < 256
        body += bytes([shared, len(rest)]) + rest
        prev = e
    head = struct.pack('<I', len(entries))
    start = len(head) + 4 * len(offsets)
    return head + b''.join(struct.pack('<I', start + o) for o in offsets) + bytes(body)


def build(rows):
    # countries: by number of records; a prefix's country: the one most of its IDs have
    by_country = collections.Counter(x[5] for x in rows)
    countries = [c for c, _ in by_country.most_common()]
    assert len(countries) < 255
    number = {c: i for i, c in enumerate(countries)}
    votes = collections.defaultdict(collections.Counter)
    for x in rows:
        votes[x[0] // 10000][x[5]] += 1
    prefix_map = bytearray(b'\xff' * 1000)
    for p, c in votes.items():
        prefix_map[p] = number[c.most_common(1)[0][0]]

    def country_of(rid):
        return countries[prefix_map[rid // 10000]]

    # per country: callsign prefixes (by use), states (sorted), places (sorted (state, city))
    prefixes = collections.defaultdict(collections.Counter)
    states = collections.defaultdict(set)
    places = collections.defaultdict(set)
    for x in rows:
        m = CALL.match(x[1])
        if m:
            prefixes[country_of(x[0])][m.group(1)] += 1
        if x[4]:
            states[x[5]].add(x[4])
        places[x[5]].add((x[4], x[3]))
    state_list = {c: sorted(s) for c, s in states.items()}
    state_no = {c: {s: i for i, s in enumerate(l)} for c, l in state_list.items()}

    def place_bytes(c, state, city):
        n = state_no[c][state] if state else 0xFFFF
        return struct.pack('>H', n) + city.encode()

    place_list = {c: sorted(places[c], key=lambda p: place_bytes(c, *p)) for c in countries}
    place_no = {c: {p: i for i, p in enumerate(l)} for c, l in place_list.items()}
    prefix_list = {c: [p for p, _ in prefixes[c].most_common()] for c in countries}
    prefix_no = {c: {p: i for i, p in enumerate(l)} for c, l in prefix_list.items()}

    name_count = collections.Counter(x[2] for x in rows)
    names = [n for n, k in name_count.most_common() if k > 1]
    name_no = {n: i for i, n in enumerate(names)}

    others = collections.Counter((x[5], x[4], x[3]) for x in rows if x[5] != country_of(x[0]))
    other_list = [o for o, _ in others.most_common()]
    other_no = {o: i for i, o in enumerate(other_list)}

    def fields(x, prev):
        c = country_of(x[0])
        f = {'delta': (x[0] - prev[0]) if prev else None}
        m = CALL.match(x[1])
        if prev and prev[1] == x[1]:
            f['call'] = 0
        elif m:
            f['call'] = 2 + prefix_no[c][m.group(1)]
            f['suffix'] = m.group(2)
        else:
            f['call'] = 1
            f['call_text'] = x[1]
        if x[2] in name_no:
            f['name'] = 1 + name_no[x[2]]
        else:
            f['name'] = 0
            f['name_text'] = x[2]
        if prev and prev[3:] == x[3:]:
            f['place'] = 0
        elif x[5] == c:
            f['place'] = 2 + place_no[c][(x[4], x[3])]
        else:
            f['place'] = 1
            f['other'] = other_no[(x[5], x[4], x[3])]
        return f

    all_fields = [fields(x, rows[i - 1] if i else None) for i, x in enumerate(rows)]
    classes = {k: best_classes([f[k] for f in all_fields if f.get(k) is not None]) for k in FIELDS}

    def put_record(b, x, prev):
        f = fields(x, prev)
        if prev:
            put_number(b, classes['delta'], f['delta'])
        put_number(b, classes['call'], f['call'])
        if f['call'] == 1:
            put_text(b, f['call_text'])
        elif f['call'] >= 2:
            s = f['suffix']
            b.put(len(s), 3)
            v = 0
            for ch in s:
                v = v * 26 + ord(ch) - 65
            b.put(v, SUFFIX_BITS[len(s)])
        put_number(b, classes['name'], f['name'])
        if f['name'] == 0:
            put_text(b, f['name_text'])
        put_number(b, classes['place'], f['place'])
        if f['place'] == 1:
            put_number(b, classes['other'], f['other'])

    blocks, index = [], []
    i = 0
    while i < len(rows):
        b, prev, first = Bits(), None, i
        while i < len(rows):
            trial = b.copy()
            put_record(trial, rows[i], prev)
            if (trial.n + 7) // 8 + 4 > BLOCK:
                break
            b, prev, i = trial, rows[i], i + 1
        data = bytes([0xD1, 0]) + struct.pack('<H', i - first) + b.bytes()
        blocks.append(data + b'\xff' * (BLOCK - len(data)))
        index.append(rows[first][0])

    # the tables after the blocks
    tables = bytearray()

    def add(data):
        at = BLOCK * len(blocks) + len(tables)
        tables.extend(data)
        while len(tables) % 4:
            tables.append(0)
        return at

    off_index = add(b''.join(struct.pack('<I', v) for v in index))
    off_prefix = add(bytes(prefix_map))
    off_countries = add(string_list([c.encode() for c in countries]))
    directory = []
    for c in countries:
        a = add(string_list([p.encode() for p in prefix_list[c]]))
        s = add(string_list([t.encode() for t in state_list.get(c, [])]))
        p = add(string_list([place_bytes(c, *pl) for pl in place_list[c]]))
        directory.append(struct.pack('<III', a, s, p))
    off_directory = add(b''.join(directory))
    off_names = add(string_list([n.encode() for n in names]))
    off_other = add(b''.join(struct.pack('<I', (number[o[0]] << 24) | place_no[o[0]][(o[1], o[2])])
                             for o in other_list))
    body = b''.join(blocks) + bytes(tables)
    body += b'\xff' * ((-len(body)) % SECTOR)
    if len(body) > MAX_SIZE:
        sys.exit('%.2f MB: more than the %d MB the database may take (--countries)' % (len(body) / 1e6,
                                                                                      MAX_SIZE >> 20))
    base = HEADER_AT - len(body)
    header = b'ODID' + bytes([1, 0, 0, 0]) + struct.pack('<IIIII', len(rows), len(blocks), len(countries), base,
                                                         len(body))
    header += struct.pack('<IIIIIII', 0, off_index, off_prefix, off_countries, off_directory, off_names, off_other)
    header += bytes(w for k in FIELDS for w in classes[k])
    header += struct.pack('<I', zlib.crc32(body))
    header += b'\xff' * (SECTOR - len(header))
    stats = dict(records=len(rows), blocks=len(blocks), tables=len(tables), countries=len(countries),
                 names=len(names), classes=classes)
    return base, body, header, stats


def write(port_name, path):
    import serial
    data = open(path, 'rb').read()
    header, body = data[:SECTOR], data[SECTOR:]
    if header[:4] != b'ODID':
        sys.exit('%s: not a database of this tool' % path)
    base = struct.unpack_from('<I', header, 20)[0]
    port = serial.Serial(port_name, 115200, timeout=0.5)
    time.sleep(0.2)

    def request(data, answer):
        port.reset_input_buffer()
        port.write(data)
        return port.read(len(answer)) == answer

    def sector_crc(address):
        """The radio's CRC-32 of a sector of the window ('D' 'K'), None: no answer"""
        for _ in range(3):
            port.reset_input_buffer()
            port.write(b'DK' + struct.pack('>I', address))
            got = port.read(6)
            if got[:2] == b'DK' and len(got) == 6:
                return struct.unpack('>I', got[2:])[0]
            time.sleep(0.1)
        return None

    def speed_up():
        """The fastest speed both ends keep ('D' 'S', confirmed by 'D' 'Y'; the radio goes back by itself)"""
        for divisor in SPEED_DIVISORS:
            baud = UART_CLOCK // divisor
            if not request(b'DS' + struct.pack('>H', divisor), b'DS\x01'):
                return 115200  # an older firmware: as fast as it goes
            time.sleep(0.02)
            port.baudrate = baud
            time.sleep(0.3)  # a com0com bridge follows the change late
            if request(b'DY', b'DY') and all(sector_crc(HEADER_AT) is not None for _ in range(4)):
                return baud
            port.baudrate = 115200
            time.sleep(SPEED_BACK_S)
        return 115200

    def put_sector(address, sector):
        """'X' 1, 2, 3 as the CPS writes a sector, then its CRC read back; again when it is not there"""
        want = zlib.crc32(sector)
        for _ in range(4):
            if (request(b'X\x01' + struct.pack('>I', address // SECTOR)[1:], b'X\x01') and
                    all(request(b'X\x02' + struct.pack('>IH', address + at, len(sector[at:at + CHUNK_WRITE])) +
                                sector[at:at + CHUNK_WRITE], b'X\x02') for at in range(0, SECTOR, CHUNK_WRITE)) and
                    request(b'X\x03', b'X\x03') and sector_crc(address) == want):
                return
            time.sleep(0.2)
        sys.exit('\nsector 0x%06X could not be written' % address)

    def find():
        """At 115200, or still at a fast speed (a run just before: the radio goes back after 5 s of quiet)"""
        for baud in (115200,) + tuple(UART_CLOCK // d for d in SPEED_DIVISORS):
            port.baudrate = baud
            time.sleep(0.1)
            if sector_crc(HEADER_AT) is not None:
                return baud
        port.baudrate = 115200
        if request(b'R\x09' + struct.pack('>IH', 0, 8), b'R'):
            return 115200  # a firmware without 'D' 'K'
        sys.exit('the radio does not answer (locked? another program on %s?)' % port_name)

    start = time.time()
    baud = find()
    if baud == 115200:
        baud = speed_up()
    count = len(body) // SECTOR
    print('%d baud; comparing %d sectors' % (baud, count + 1))
    changed = [i for i in range(count)
               if sector_crc(base + i * SECTOR) != zlib.crc32(body[i * SECTOR:(i + 1) * SECTOR])]
    if changed or sector_crc(HEADER_AT) != zlib.crc32(header):
        put_sector(HEADER_AT, b'\xff' * SECTOR)  # no database while it is written
        for n, i in enumerate(changed):
            put_sector(base + i * SECTOR, body[i * SECTOR:(i + 1) * SECTOR])
            if n % 16 == 0 or n == len(changed) - 1:
                done = (n + 1) / len(changed)
                left = (time.time() - start) / done * (1 - done)
                print('\r%5.1f%%  %d of %d sectors changed  %3.0f s left ' % (done * 100, n + 1, len(changed), left),
                      end='', flush=True)
        put_sector(HEADER_AT, header)
    port.write(b'C\x05')  # the CPS's close: the radio's CPS screen goes (cps_screen.c)
    port.read(1)
    print('\n%d sectors written in %.0f s' % (len(changed), time.time() - start))


def prefix_countries(rows):
    """The country most IDs of each first 3 digits have"""
    votes = collections.defaultdict(collections.Counter)
    for x in rows:
        votes[x[0] // 10000][x[5]] += 1
    return {p: c.most_common(1)[0][0] for p, c in votes.items()}


def write_countries(rows, path):
    countries = prefix_countries(rows)
    names = sorted(set(countries.values()))
    assert len(names) < 255
    number = {n: i for i, n in enumerate(names)}
    first, last = min(countries), max(countries)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('/*\n * Generated by tools/ids/id_db.py countries from RadioID\'s user list. Do not edit.\n *\n'
                ' * The country of a DMR ID\'s first 3 digits (dmr_ids.c dmr_ids_country()): the one most of its IDs '
                'have.\n */\n\n#include <stdint.h>\n#include "hw.h"\n#include "dmr_countries.h"\n\n')
        f.write('const char *const DMR_COUNTRY_NAMES[%d] FLASH_CONST = {\n' % len(names))
        for n in names:
            f.write('    "%s",\n' % n.replace('\\', '\\\\').replace('"', '\\"'))
        f.write('};\n\n')
        f.write('// From prefix %d: the number of its country in DMR_COUNTRY_NAMES, 0xFF: none\n' % first)
        f.write('const uint8_t DMR_COUNTRY_OF_PREFIX[%d] FLASH_CONST = {\n' % (last - first + 1))
        values = [number[countries[p]] if p in countries else 0xFF for p in range(first, last + 1)]
        for i in range(0, len(values), 16):
            f.write('    ' + ', '.join('%3d' % v for v in values[i:i + 16]) + ',\n')
        f.write('};\n')
    header = os.path.splitext(path)[0] + '.h'
    with open(header, 'w', encoding='utf-8', newline='\n') as f:
        f.write('/*\n * Generated by tools/ids/id_db.py countries. Do not edit.\n */\n\n'
                '#ifndef DM32_DMR_COUNTRIES_H\n#define DM32_DMR_COUNTRIES_H\n\n#include <stdint.h>\n\n'
                '#define DMR_COUNTRY_FIRST_PREFIX %d\n#define DMR_COUNTRY_PREFIXES     %d\n#define DMR_COUNTRIES            %d\n\n'
                'extern const char *const DMR_COUNTRY_NAMES[DMR_COUNTRIES];\n'
                'extern const uint8_t DMR_COUNTRY_OF_PREFIX[DMR_COUNTRY_PREFIXES];\n\n#endif\n'
                % (first, last - first + 1, len(names)))
    print('%d prefixes (%d - %d), %d countries: %s' % (len(countries), first, last, len(names), path))


class Reader:
    """The database read as the firmware reads it (src/services/dmr_ids.c)."""

    def __init__(self, data):
        self.h, self.d = data[:SECTOR], data[SECTOR:]
        (self.records, self.blocks, self.countries, self.base, self.size, _, self.index, self.prefix,
         self.country_names, self.directory, self.names, self.other) = struct.unpack_from('<IIIII7I', self.h, 8)
        self.classes = {k: list(self.h[56 + 4 * i:60 + 4 * i]) for i, k in enumerate(FIELDS)}

    def u32(self, at):
        return struct.unpack_from('<I', self.d, at)[0]

    def entry(self, at, n):
        assert n < self.u32(at)
        pos = at + self.u32(at + 4 + 4 * (n // CHUNK))
        text = b''
        for _ in range(n % CHUNK + 1):
            shared, length = self.d[pos], self.d[pos + 1]
            text = text[:shared] + self.d[pos + 2:pos + 2 + length]
            pos += 2 + length
        return text

    def lookup(self, rid):
        lo, hi = 0, self.blocks - 1
        if rid < self.u32(self.index):
            return None
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if self.u32(self.index + 4 * mid) <= rid:
                lo = mid
            else:
                hi = mid - 1
        block = self.d[lo * BLOCK:(lo + 1) * BLOCK]
        bits = int.from_bytes(block[4:], 'big')
        total = (BLOCK - 4) * 8
        at = [0]

        def get(width):
            at[0] += width
            return (bits >> (total - at[0])) & ((1 << width) - 1)

        def number(k):
            ws = self.classes[k]
            c = get(2)
            return sum(1 << w for w in ws[:c]) + get(ws[c])

        def text():
            return bytes(get(8) for _ in range(get(8))).decode()

        current = self.u32(self.index + 4 * lo)
        call = place = None
        for n in range(struct.unpack_from('<H', block, 2)[0]):
            if n:
                current += number('delta')
            country = self.prefix_country(current)
            c = number('call')
            if c == 1:
                call = text()
            elif c >= 2:
                length = get(3)
                v = get(SUFFIX_BITS[length])
                suffix = ''
                for _ in range(length):
                    suffix = chr(65 + v % 26) + suffix
                    v //= 26
                call = self.entry(self.u32(self.directory + 12 * country), c - 2).decode() + suffix
            nm = number('name')
            name = text() if nm == 0 else self.entry(self.names, nm - 1).decode()
            p = number('place')
            if p == 1:
                o = self.u32(self.other + 4 * number('other'))
                place = (o >> 24, o & 0xFFFFFF)
            elif p >= 2:
                place = (country, p - 2)
            if current == rid:
                pc, pn = place
                raw = self.entry(self.u32(self.directory + 12 * pc + 8), pn)
                state_no = struct.unpack('>H', raw[:2])[0]
                state = '' if state_no == 0xFFFF else self.entry(self.u32(self.directory + 12 * pc + 4),
                                                                   state_no).decode()
                return (rid, call, name, raw[2:].decode(), state, self.entry(self.country_names, pc).decode())
            if current > rid:
                return None
        return None

    def prefix_country(self, rid):
        return self.d[self.prefix + rid // 10000]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='command', required=True)
    b = sub.add_parser('build', help='download and pack the list')
    b.add_argument('--csv', help="RadioID's user.csv (default: download it)")
    b.add_argument('--countries', help='only these, comma separated, as RadioID names them')
    b.add_argument('-o', '--output', default='ids.bin')
    c = sub.add_parser('check', help='read every record of a packed database back, as the firmware does')
    c.add_argument('file', nargs='?', default='ids.bin')
    c.add_argument('--csv', help="RadioID's user.csv it was built from (default: download it)")
    c.add_argument('--countries')
    c.add_argument('--sample', type=int, default=5000, help='records read at random (and the first and last of each block)')
    n = sub.add_parser('countries', help='make src/services/dmr_countries.c (the countries built into the firmware)')
    n.add_argument('--csv', help="RadioID's user.csv (default: download it)")
    n.add_argument('-o', '--output', default=os.path.join(HERE, '..', '..', 'src', 'services', 'dmr_countries.c'))
    w = sub.add_parser('write', help='write a packed database to the radio')
    w.add_argument('port', help='the programming cable, e.g. COM6')
    w.add_argument('file', nargs='?', default='ids.bin')
    args = parser.parse_args()

    if args.command == 'build':
        rows = load(args.csv, args.countries)
        if not rows:
            sys.exit('no records')
        base, body, header, stats = build(rows)
        with open(args.output, 'wb') as f:
            f.write(header + body)
        print('%d records, %d countries: %.2f MB (records %.2f MB, tables %.2f MB) at 0x%06X - 0x%06X of the window'
              % (stats['records'], stats['countries'], len(body) / 1e6, stats['blocks'] * BLOCK / 1e6,
                 stats['tables'] / 1e6, base, HEADER_AT + SECTOR))
    elif args.command == 'countries':
        write_countries(load(args.csv, None), os.path.normpath(args.output))
    elif args.command == 'check':
        reader = Reader(open(args.file, 'rb').read())
        rows = load(args.csv, args.countries)
        import random
        firsts = {struct.unpack_from('<I', reader.d, reader.index + 4 * i)[0] for i in range(reader.blocks)}
        sample = [x for i, x in enumerate(rows) if x[0] in firsts or (i + 1 < len(rows) and rows[i + 1][0] in firsts)]
        sample += random.Random(1).sample(rows, min(len(rows), args.sample)) + rows[-1:]
        bad = [x for x in sample if reader.lookup(x[0]) != x]
        missing = [i for i in (999999, 1000000, rows[0][0] - 1, rows[-1][0] + 1) if reader.lookup(i) is not None]
        print('%d records (%d read): %d read back wrong, %d IDs found that are not there' % (len(rows), len(sample), len(bad), len(missing)))
        for x in bad[:5]:
            print('  ', x, '->', reader.lookup(x[0]))
    else:
        write(args.port, args.file)


if __name__ == '__main__':
    main()
