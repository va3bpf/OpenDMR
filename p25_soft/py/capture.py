#!/usr/bin/env python3
"""A P25 transmission recorded as the software receivers get it (the FD6818's discriminator at 8 kHz, FM_BANDWIDTH 0x0F):
the transmitter (P25) sends p25_txcheck.py's numbered test frames, the receiver (in M17 mode: its raw recording tap,
m17_radio.c m17_raw_tap) takes 12000 samples (1.5 s) from --after seconds on. Saved: <name>.raw (16-bit, 8 kHz) and
<name>.json (fn0, the source ID, the lead, the talk group: tools/debug/p25_txcheck.py sent_stream() rebuilds what was sent).
  py p25_soft/py/capture.py name [--tx COM23] [--rx COM22] [--after 0.6] [--seconds 3] [--set p25_tx_low_power=1]
Both radios on the same frequency, the transmitter in P25, the receiver in M17 (DEBUG=1 builds, the ELF of src/build).
"""
import argparse
import json
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..', '..')
sys.path.insert(0, os.path.join(ROOT, 'tools', 'debug'))
from dm32dbg import ELF, Radio, Symbols  # noqa: E402

N = 12000


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('name')
    ap.add_argument('--tx', default='COM23')
    ap.add_argument('--rx', default='COM22')
    ap.add_argument('--after', type=float, default=0.6)
    ap.add_argument('--seconds', type=float, default=3.0)
    ap.add_argument('--set', nargs='*', default=[])
    a = ap.parse_args()
    s = Symbols(ELF)
    tx, rx = Radio(a.tx), Radio(a.rx)
    fn0 = int(time.time()) % 50000 * 10
    for name, value in [('p25_tx_source', 1), ('p25_tx_fn0', fn0)] + [(kv.split('=')[0], int(kv.split('=')[1], 0))
                                                                     for kv in a.set]:
        tx.value_write(s.resolve(name), 4, value)
    src = tx.call(s.resolve('codeplug_get_dmr_id'), []) & 0xFFFFFF
    lead = tx.value_read(s.resolve('p25_tx_lead'), 4)
    tg = tx.value_read(s.resolve('tx_tgid'), 2)  # p25_radio.c: the channel's talk group, in the sent frames
    rx.value_write(s.resolve('m17_raw_hold'), 4, 1)  # the tap stopped
    tx.call(s.resolve('radio_transmit'), [1])
    time.sleep(a.after)
    rx.value_write(s.resolve('m17_raw_tap_n'), 4, 0)
    rx.value_write(s.resolve('m17_raw_hold'), 4, N + 1)  # N samples from now, then it stops
    time.sleep(max(0.0, a.seconds - a.after))
    tx.call(s.resolve('radio_transmit'), [0])
    time.sleep(1.5)
    tx.value_write(s.resolve('p25_tx_source'), 4, 0)
    n = rx.value_read(s.resolve('m17_raw_tap_n'), 4)
    tap = s.resolve('m17_raw_tap')
    raw = b''
    while len(raw) < 2 * N:
        raw += rx.mem_read(tap + len(raw), min(240, 2 * N - len(raw)))
    out = os.path.join(HERE, '..', 'data', a.name)
    open(out + '.raw', 'wb').write(raw[:2 * min(n, N)])
    json.dump({'fn0': fn0, 'src': src, 'lead': lead, 'tg': tg, 'samples': min(n, N), 'after': a.after}, open(out + '.json', 'w'))
    print('%s: %d samples, fn0 %d, src %d, lead %d' % (a.name, min(n, N), fn0, src, lead))


if __name__ == '__main__':
    main()
