#!/usr/bin/env python3
"""The HR_C7000 physical layer receive (src/services/p25_phy.c, docs/p25.md) over the programming cable.

  py p25_phy.py capture --port COM6 [--blocks 96] [--p25] [--out phy.bin] [--wait 60]
      on the radio's current DMR channel: switch the modem to the physical layer, record the 30 ms blocks, read them
      back (the modem goes back to DMR by itself when the buffer is full), save and analyse them; --wait: again and
      again until a capture holds a sync
  py p25_phy.py analyse phy.bin [--dibits out.dibits]
      analyse a saved capture: the blocks put back into the order on the air (Figure 70), where the DMR / P25 syncs
      are; --dibits writes the stream for p25/bin/p25_cli.exe

  py p25_phy.py txtest --port COM6 [--delay 20] [--seconds 15] [--write-mode 0]
      after the delay, the radio sends a P25 test call (src/services/p25_phy.c p25_phy_tx_test()) at 50 mW on its
      channel's Tx frequency: time to move the only cable to the receiving radio
  py p25_phy.py verify phy.dibits
      the captured stream through p25/bin/p25_cli.exe, its IMBE frames against the ones sent

--p25: the sync detection on the P25 frame sync (else the DMR syncs as the channel set them up).
The file: per block, us (u32 LE), RX_TYPE_INFO (u32 LE), 36 bytes as the modem gave them.
"""
import argparse
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dm32dbg import ELF, Radio, Symbols  # noqa: E402

BLOCK = 88  # us, RX_TYPE_INFO, SYS_INTERP_LIST, us2 (u32 LE each), data[36], data2[36]
SYNCS = {  # 48-bit frame syncs (ETSI TS 102 361-1 9.1.1; P25 TIA-102.BAAA)
    'DMR BS voice': 0x755FD7DF75F7, 'DMR BS data': 0xDFF57D75DF5D,
    'DMR MS voice': 0x7F7D5DD57DFD, 'DMR MS data': 0xD5D7F77FD757,
    'DMR TDMA1 voice': 0x5D577F7757FF, 'DMR TDMA1 data': 0xF7FDD5DDFD55,
    'DMR TDMA2 voice': 0x7DFFD5F55D5F, 'DMR TDMA2 data': 0xD7557F5FF7F5,
    'P25': 0x5575F5FF77FF,
}


def bits_of(data):
    return [(b >> (7 - i)) & 1 for b in data for i in range(8)]


def to_air(bits, kind):
    """The 288 bits from the modem's order back to the order on the air (HR_C7000 Figure 70)."""
    c = bits[:24]
    if kind in (0, 1):  # CACH 24, sync 48, info 108, info 108 -> CACH, info, sync, info
        s, i1, i2 = bits[24:72], bits[72:180], bits[180:288]
        return c + i1 + s + i2
    if kind == 2:  # CACH 24, slot type 10, slot type 10, sync 48, info 98, info 98
        t1, t2, s, i1, i2 = bits[24:34], bits[34:44], bits[44:92], bits[92:190], bits[190:288]
        return c + i1 + t1 + s + t2 + i2
    return None


def find_syncs(bits, tolerance=4):
    """(position, name, bit errors) of every frame sync in the bit list"""
    out = []
    if len(bits) < 48:
        return out
    v = 0
    mask = (1 << 48) - 1
    for i, b in enumerate(bits):
        v = ((v << 1) | b) & mask
        if i >= 47:
            for name, s in SYNCS.items():
                e = bin(v ^ s).count('1')
                if e <= tolerance:
                    out.append((i - 47, name, e))
    return out


def load(path):
    return parse(open(path, 'rb').read())


def parse(raw):
    """(us, RX_TYPE_INFO, data) of each block"""
    return [(us, kind, data) for us, kind, _, _, data, _ in parse_full(raw)]


def parse_full(raw):
    """(us, RX_TYPE_INFO, SYS_INTERP_LIST, us2, data, data2) of each block"""
    blocks = []
    for k in range(len(raw) // BLOCK):
        r = raw[k * BLOCK:(k + 1) * BLOCK]
        us, kind, sys_list, us2 = struct.unpack('<IIII', r[:16])
        blocks.append((us, kind, sys_list, us2, r[16:52], r[52:88]))
    return blocks


def diag(path):
    """per block: the gap, SYS_INTERP_LIST, RX_TYPE_INFO, bytes the same as the block before, and the second read
    (at the receive timeslot interrupt): when, and how many of its bytes differ from the first"""
    b = parse_full(open(path, 'rb').read())
    print(' k    gap us  SYS_LIST  RX_TYPE   =prev  2nd read: +us  differs  =next block')
    for i, (us, kind, sys_list, us2, d, d2) in enumerate(b):
        gap = (us - b[i - 1][0]) & 0xFFFFFFFF if i else 0
        same = sum(x == y for x, y in zip(d, b[i - 1][4])) if i else 0
        diff2 = sum(x != y for x, y in zip(d, d2))
        nxt = sum(x == y for x, y in zip(d2, b[i + 1][4])) if i + 1 < len(b) else -1
        late = ((us2 - us) & 0xFFFFFFFF) if us2 else -1
        print('%2d  %7d  %08X  %08X   %2d    %7d    %2d       %2d' % (i, gap, sys_list, kind, same, late, diff2, nxt))


def analyse(blocks, dibits_path=None):
    if not blocks:
        print('no blocks')
        return
    print('%d blocks' % len(blocks))
    gaps = [(blocks[i][0] - blocks[i - 1][0]) & 0xFFFFFFFF for i in range(1, len(blocks))]
    if gaps:
        print('gaps between blocks: %d .. %d us (30000 expected)' % (min(gaps), max(gaps)))
    kinds = {}
    for _, kind, _ in blocks:
        kinds[kind & 3] = kinds.get(kind & 3, 0) + 1
    print('RX_TYPE_INFO[1:0]:', ', '.join('%d: %d blocks' % kv for kv in sorted(kinds.items())),
          '  (full values of the first: %s)' % ' '.join('%08X' % b[1] for b in blocks[:6]))

    # where the syncs are: in the modem's order and on the air, within each block
    raw_pos, air_pos = {}, {}
    stream = []
    for _, kind, data in blocks:
        b = bits_of(data)
        for pos, name, e in find_syncs(b):
            raw_pos.setdefault((pos, name), 0)
            raw_pos[(pos, name)] += 1
        a = to_air(b, kind & 3)
        if a is None:
            stream = []  # an unknown layout breaks the stream
            continue
        for pos, name, e in find_syncs(a):
            air_pos.setdefault((pos, name), 0)
            air_pos[(pos, name)] += 1
        stream += a
    print('syncs within a block, as the modem gives it (bit: count):')
    for (pos, name), n in sorted(raw_pos.items(), key=lambda x: -x[1])[:8]:
        print('   %-16s at bit %3d: %d' % (name, pos, n))
    print('syncs within a block, put back into the order on the air:')
    for (pos, name), n in sorted(air_pos.items(), key=lambda x: -x[1])[:8]:
        print('   %-16s at bit %3d: %d' % (name, pos, n))
    found = find_syncs(stream)
    print('syncs in the joined stream: %d (%s)' % (len(found), ', '.join(sorted({n for _, n, _ in found})) or '-'))
    if found:
        pos = [p for p, _, _ in found]
        spacing = sorted({pos[i] - pos[i - 1] for i in range(1, len(pos))})
        print('   distances between them (bits):', spacing[:12])
    if dibits_path:
        d = bytes((stream[i] << 1) | stream[i + 1] for i in range(0, len(stream) - 1, 2))
        open(dibits_path, 'wb').write(d)
        print('wrote %s (%d dibits)' % (dibits_path, len(d)))


def capture(args):
    """--wait S: capture again and again (each about 3 s) until a capture holds a sync, for up to S seconds"""
    sym = Symbols(args.elf)
    radio = Radio(args.port)
    deadline = time.time() + args.wait
    while True:
        raw = capture_once(args, sym, radio)
        has_sync = any(find_syncs(bits_of(d)) for _, _, d in parse(raw))
        if has_sync or time.time() >= deadline:
            break
        print('no sync in this capture, again (%.0f s left)' % (deadline - time.time()))
    open(args.out, 'wb').write(raw)
    print('saved %d blocks to %s' % (len(raw) // BLOCK, args.out))
    analyse(load(args.out), args.dibits)


def capture_once(args, sym, radio):
    count_addr = sym.resolve('p25_phy_count')
    buf_addr = sym.resolve('p25_phy_buf')
    r = radio.call(sym.resolve('p25_phy_capture'), [1 if args.p25 else 0, args.blocks])
    if r != 0:
        raise SystemExit('p25_phy_capture: %d (already running?)' % r)
    end = time.time() + args.blocks * 0.03 + 3
    n = 0
    while time.time() < end:
        n = struct.unpack('<I', radio.mem_read(count_addr, 4))[0]
        if n >= args.blocks:
            break
        time.sleep(0.2)
    radio.call(sym.resolve('p25_phy_stop'), [])  # stops it if it is still running (nothing otherwise)
    if n < args.blocks:
        print('only %d of %d blocks (no interrupts?): stopped' % (n, args.blocks))
    raw = b''
    total = n * BLOCK
    while len(raw) < total:
        raw += radio.mem_read(buf_addr + len(raw), min(256, total - len(raw)))
    return raw


def tx_frame(n):
    """The 88 parameter bits of the test call's IMBE frame n (p25_phy_tx_frame() of the firmware)"""
    m = 0xFFFFFFFF
    x = (0x25 + n * 0x9E3779B9) & m
    u = []
    for _ in range(88):
        x ^= (x << 13) & m
        x ^= x >> 17
        x ^= (x << 5) & m
        u.append(x & 1)
    return bytes(int(''.join(map(str, u[i:i + 8])), 2) for i in range(0, 88, 8))


def txtest(args):
    sym = Symbols(args.elf)
    radio = Radio(args.port)
    r = radio.call(sym.resolve('p25_phy_tx_test'), [int(args.delay * 1000), int(args.seconds * 1000), args.write_mode])
    if r != 0:
        raise SystemExit('p25_phy_tx_test: %d (busy?)' % r)
    print('the test call starts in %.0f s and lasts %.0f s (50 mW, write mode %d): move the cable now' %
          (args.delay, args.seconds, args.write_mode))


def verify(args):
    """dibits of a capture of the test call -> p25_cli -> the IMBE frames against the ones sent"""
    import subprocess
    cli = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'p25', 'bin', 'p25_cli.exe')
    frames_path = args.dibits + '.imbe'
    out = subprocess.run([cli, args.dibits, args.dibits + '.wav', frames_path], capture_output=True, text=True).stdout
    print(out)
    got = open(frames_path, 'rb').read()
    got = [got[i:i + 11] for i in range(0, len(got), 11)]
    if not got:
        print('no voice frames')
        return
    expected = {tx_frame(n): n for n in range(9 * 400)}
    first = next((i for i, f in enumerate(got) if f in expected), None)
    if first is None:
        print('none of the %d frames is one of the test call: the stream is not right' % len(got))
        return
    n0 = expected[got[first]] - first
    same = sum(1 for i, f in enumerate(got) if f == tx_frame(n0 + i))
    bad = [i for i, f in enumerate(got) if f != tx_frame(n0 + i)]
    print('IMBE frames: %d of %d exactly as sent (the capture starts at frame %d of the call)' % (same, len(got), n0))
    if bad:
        print('different:', bad[:30])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('capture')
    c.add_argument('--port', default='COM6')
    c.add_argument('--elf', default=ELF)
    c.add_argument('--blocks', type=int, default=64)
    c.add_argument('--p25', action='store_true')
    c.add_argument('--out', default='phy.bin')
    c.add_argument('--dibits')
    c.add_argument('--wait', type=float, default=0, help='capture again until a sync is found, up to this many s')
    a = sub.add_parser('analyse')
    a.add_argument('file')
    a.add_argument('--dibits')
    t = sub.add_parser('txtest')
    t.add_argument('--port', default='COM6')
    t.add_argument('--elf', default=ELF)
    t.add_argument('--delay', type=float, default=20)
    t.add_argument('--seconds', type=float, default=15)
    t.add_argument('--write-mode', type=int, default=0)
    v = sub.add_parser('verify')
    v.add_argument('dibits')
    g = sub.add_parser('diag')
    g.add_argument('file')
    args = ap.parse_args()
    if args.cmd == 'capture':
        capture(args)
    elif args.cmd == 'txtest':
        txtest(args)
    elif args.cmd == 'verify':
        verify(args)
    elif args.cmd == 'diag':
        diag(args.file)
    else:
        analyse(load(args.file), args.dibits)


if __name__ == '__main__':
    main()
