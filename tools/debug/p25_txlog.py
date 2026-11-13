#!/usr/bin/env python3
"""The P25 transmit log of a radio (src/services/p25_phy.c p25_phy_txlog): the begin, the transmit interrupts and the end,
with the time since the last interrupt and LAYER2_STATUS (tx_slot_choose, tx_bit_cnt: counting in the first block only).

  py p25_txlog.py --port COM14
"""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from dm32dbg import ELF, Radio, Symbols  # noqa: E402

EVENTS = {0: 'begin/end', 1: 'TX IRQ'}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', default='COM14')
    ap.add_argument('--elf', default=ELF)
    a = ap.parse_args()
    r, s = Radio(a.port), Symbols(a.elf)
    n = struct.unpack('<I', r.mem_read(s.resolve('p25_phy_txlog_n'), 4))[0]
    base = s.resolve('p25_phy_txlog')
    raw = b''
    while len(raw) < n * 16:
        raw += r.mem_read(base + len(raw), min(256, n * 16 - len(raw)))
    irq_us = None
    last = None
    print('%d entries' % n)
    print('%10s %8s %-9s %6s %4s %6s %8s' % ('us', '+irq', 'event', 'arg', 'slot', 'bitcnt', 'sloton'))
    for i in range(n):
        us, status, sloton, ev, arg = struct.unpack('<IIIHH', raw[16 * i:16 * i + 16])
        if ev == 1:
            irq_us = us
        slot, cnt = status >> 31, status & 0x1FF
        rel = (us - irq_us) & 0xFFFFFFFF if irq_us is not None else 0
        if True:
            print('%10d %8d %-9s %6d %4d %6d %08X' % (us, rel, EVENTS.get(ev, ev), arg, slot, cnt, sloton))
        last = (slot, cnt, sloton)


if __name__ == '__main__':
    main()
