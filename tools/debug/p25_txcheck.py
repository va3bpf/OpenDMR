#!/usr/bin/env python3
"""The P25 transmitter checked over the air with two radios and two cables: the transmitting radio keyed through
radio_transmit() (as the PTT) sending test frames (p25_radio.c p25_tx_source 1, p25_phy_tx_frame() from p25_tx_fn0),
the receiving radio's tap (p25_radio.c) read after it; each received block found in the stream that was sent
(p25/bin/p25_build.exe of the same frames, NAC 293, TG 1, the transmitter's DMR ID) and its wrong symbols counted, by
their place in the 144-dibit blocks the modem was given.

  py p25_txcheck.py --tx COM14 --rx COM20 [--rx-elf green.elf] [--set name=value ...] [--seconds 1.5]

--set writes variables of the transmitter first (e.g. p25_tx_low_power=1). The source goes back to the mic (0) after.
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile
import time
from collections import Counter

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from dm32dbg import ELF, Radio, Symbols  # noqa: E402
from p25_phy import bits_of, to_air  # noqa: E402

ROOT = os.path.join(HERE, '..', '..')
BUILD = os.path.join(ROOT, 'p25', 'bin', 'p25_build.exe')
TAP_BLOCKS, TAP_SIZE = 128, 44


def test_frame(n):
    x = (0x25 + n * 0x9E3779B9) & 0xFFFFFFFF
    u = []
    for _ in range(88):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        u.append(x & 1)
    return bytes(sum(u[8 * i + k] << (7 - k) for k in range(8)) for i in range(11))


SYNC = [1, 1, 1, 1, 1, 3, 1, 1, 3, 3, 1, 1, 3, 3, 3, 3, 1, 3, 1, 3, 3, 3, 3, 3]  # 0x5575F5FF77FF


def sent_stream(fn0, src, ldus=120, lead=0):
    """What the radio sent: the preamble blocks (+3, -3, ...), HDU, the LDUs, TDU, zeros to a whole block."""
    with tempfile.TemporaryDirectory() as d:
        imbe, out = os.path.join(d, 'f.imbe'), os.path.join(d, 'f.bin')
        open(imbe, 'wb').write(b''.join(test_frame(fn0 + n) for n in range(9 * ldus)))
        subprocess.run([BUILD, imbe, out, '0x293', '1', str(src)], check=True, capture_output=True)
        tx = np.fromfile(out, np.uint8)
    d = np.stack([(tx >> 6) & 3, (tx >> 4) & 3, (tx >> 2) & 3, tx & 3], 1).ravel()
    head = np.tile(np.array([1, 3], np.uint8), 72 * lead)  # p25_radio.c p25_tx_lead: +3, -3, ... (0x77 bytes)
    tail = np.zeros((-len(d)) % 144, np.uint8)
    return np.concatenate([head, d, tail])


def u32(r, s, name):
    return struct.unpack('<I', r.mem_read(s.resolve(name), 4))[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--tx', default='COM14')
    ap.add_argument('--rx', default='COM20')
    ap.add_argument('--tx-elf', default=ELF)
    ap.add_argument('--rx-elf', default=ELF)
    ap.add_argument('--set', nargs='*', default=[])
    ap.add_argument('--seconds', type=float, default=1.5)
    ap.add_argument('--fn0', type=int, default=None)
    ap.add_argument('--quiet', action='store_true')
    ap.add_argument('--dump', action='store_true', help='the blocks with errors, raw, against other places')
    a = ap.parse_args()

    tx, ts = Radio(a.tx), Symbols(a.tx_elf)
    rx, rs = Radio(a.rx), Symbols(a.rx_elf)
    fn0 = a.fn0 if a.fn0 is not None else int(time.time()) % 50000 * 10
    sets = [('p25_tx_source', 1), ('p25_tx_fn0', fn0)] + [(kv.split('=')[0], int(kv.split('=')[1], 0)) for kv in a.set]
    for name, value in sets:
        tx.value_write(ts.resolve(name), 4, value)
    src = 0
    try:
        src = tx.call(ts.resolve('codeplug_get_dmr_id'), []) & 0xFFFFFF
    except Exception:
        pass
    ldus0 = struct.unpack('<4I', tx.mem_read(ts.resolve('p25_tx_stats'), 16))[1]
    head0 = u32(rx, rs, 'p25_tap_head')
    tx.call(ts.resolve('radio_transmit'), [1])
    time.sleep(a.seconds)
    tx.call(ts.resolve('radio_transmit'), [0])
    time.sleep(2.0)
    tx.value_write(ts.resolve('p25_tx_source'), 4, 0)  # the mic again: the PTT sends speech
    state, blocks, under = (u32(tx, ts, n) for n in ('p25_phy_tx_state', 'p25_phy_tx_blocks', 'p25_phy_tx_underrun'))
    ldus = struct.unpack('<4I', tx.mem_read(ts.resolve('p25_tx_stats'), 16))[1] - ldus0
    head1 = u32(rx, rs, 'p25_tap_head')
    new = min((head1 - head0) & 0xFFFFFFFF, TAP_BLOCKS)
    tap = rs.resolve('p25_tap')
    raw = b''
    for i in range(0, TAP_BLOCKS * TAP_SIZE, 256):
        raw += rx.mem_read(tap + i, min(256, TAP_BLOCKS * TAP_SIZE - i))
    got = []
    for k in range(head1 - new, head1):
        e = raw[(k % TAP_BLOCKS) * TAP_SIZE:(k % TAP_BLOCKS + 1) * TAP_SIZE]
        got.append(struct.unpack('<II', e[:8]) + (e[8:],))

    lead = u32(tx, ts, 'p25_tx_lead')
    d = sent_stream(fn0, src, ldus, lead)  # exactly what was sent
    W = 12
    idx = {}
    for i in range(len(d) - W):
        idx.setdefault(d[i:i + W].tobytes(), []).append(i)
    pos = np.zeros(144, int)
    shifts = Counter()
    found = wrong = empty = 0
    in_lead = 0  # wrong symbols inside the lead blocks (no data there)
    prev = None
    for ms, kind, data in got:
        if data == prev:
            empty += 1
        prev = data
        a_ = to_air(bits_of(data), kind & 3)
        if a_ is None:
            continue
        r = np.array([(a_[2 * j] << 1) | a_[2 * j + 1] for j in range(144)], np.uint8)
        v = Counter()
        for o in range(0, 132, 4):
            for p in idx.get(r[o:o + W].tobytes(), [])[:50]:
                v[p - o] += 1
        if not v:
            continue
        p, c = v.most_common(1)[0]
        if c < 12 or p < 0 or p + 144 > len(d):
            continue
        e = d[p:p + 144] != r
        found += 1
        wrong += int(e.sum())
        in_lead += int(e[:max(0, 144 * lead - p)].sum())
        for j in np.nonzero(e)[0]:
            pos[(p + j) % 144] += 1
            for sh in (-576, -432, -288, -144, 144, 288, 432):
                if 0 <= p + j + sh < len(d) and r[j] == d[p + j + sh]:
                    shifts[sh] += 1
        if a.dump and e.sum() > 10:
            raw_bits = bits_of(data)
            print('  BAD type %08X at sent %d: raw %s' % (kind, p, data.hex()))
            for sh in (-288, -144, 144, 288):
                if 0 <= p + sh and p + sh + 144 <= len(d):
                    alt = d[p + sh:p + sh + 144]
                    print('     vs sent %+d: wrong where bad: %d of %d' % (sh, int(((alt != r) & e).sum()), int(e.sum())))
            # the received dibits where wrong, and what was sent there
            bad = np.nonzero(e)[0]
            print('     at %s' % [int(x) for x in bad[:24]])
            print('     got  %s' % ''.join(str(x) for x in r[bad[:24]]))
            print('     sent %s' % ''.join(str(x) for x in d[p + bad[:24]]))
        if not a.quiet:
            print('  %d type %08X block %d +%d: %d wrong %s' % (ms, kind, p // 144, p % 144, int(e.sum()),
                                                             ''.join('X' if x else '.' for x in e) if e.sum() else ''))
    print('%s | tx state %d, %d blocks, %d underruns | rx %d blocks, %d repeated, %d found in the sent stream, '
          '%d wrong symbols (%.1f%%); by place in the sent block (x8): %s' %
          (' '.join('%s=%d' % kv for kv in sets[2:]) or '-', state, blocks, under, len(got), empty, found, wrong,
           100.0 * wrong / max(1, found * 144), [int(pos[i:i + 8].sum()) for i in range(0, 144, 8)]))
    print('    in the lead blocks: %d, from the HDU on: %d' % (in_lead, wrong - in_lead))
    if wrong:
        print('    wrong symbols equal to the sent stream at a shift of: ' +
              ', '.join('%+d %.0f%%' % (sh, 100.0 * shifts[sh] / wrong) for sh in sorted(shifts)) + ' (chance 25%)')


if __name__ == '__main__':
    main()
