"""Reads the received AMBE frames the firmware wrote to the data flash (src/services/rx_capture.h) and splits them
into calls: vocoder/data/capture/rx_<call>.bit (9 bytes a frame, as DVSI's .bit), with the decoder's kind per frame
and the gaps in the arrival times.

  python tools/debug/rx_capture.py [--port COM6] [--last 5]

The firmware must be built with the capture (src: make clean && make RX_CAPTURE=1), and src/build/opendmr.elf must be
that build (the symbols are read from it).
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dm32dbg  # noqa: E402

BASE = 0x68C000
SECTORS = 80
SECTOR = 4096
RECORD = 16
CHUNK = 1024
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'vocoder', 'data', 'capture')


def read_flash(radio, sym, address, length):
    rd = sym.resolve('flash_read_raw')
    buf = sym.resolve('rx_capture_read_buffer')
    data = b''
    for a in range(address, address + length, CHUNK):
        n = min(CHUNK, address + length - a)
        for _ in range(5):
            try:
                radio.call(rd, [a, buf, n])
                data += radio.mem_read(buf, n)
                break
            except RuntimeError:
                pass
        else:
            sys.exit('the radio does not answer')
    return data


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', default='COM6')
    ap.add_argument('--last', type=int, default=0, help='only the last N calls (0: all)')
    args = ap.parse_args()
    radio = dm32dbg.Radio(args.port)
    sym = dm32dbg.Symbols(dm32dbg.ELF)

    records = []
    for s in range(SECTORS):
        a = BASE + s * SECTOR
        if read_flash(radio, sym, a, 3) == b'\xff\xff\xff':
            continue
        d = read_flash(radio, sym, a, SECTOR)
        for i in range(0, SECTOR, RECORD):
            r = d[i:i + RECORD]
            if r[:3] == b'\xff\xff\xff':
                continue
            seq = r[0] | (r[1] << 8) | (r[2] << 16)
            ms = r[3] | (r[4] << 8)
            records.append((seq, ms, r[5:14], chr(r[14]) if 32 <= r[14] < 127 else '?', r[15]))
    # the frames still in RAM (a call going on): the firmware writes them 1 s after the last frame
    records.sort()
    print('%d frames, dropped %d, sectors written %d' % (len(records), radio.value_read(sym.resolve('rx_capture_dropped'), 4),
                                                     radio.value_read(sym.resolve('rx_capture_written'), 4)))
    calls = []
    for rec in records:
        if not calls or calls[-1][-1][4] != rec[4] or rec[0] != calls[-1][-1][0] + 1:
            calls.append([])
        calls[-1].append(rec)
    if args.last:
        calls = calls[-args.last:]
    os.makedirs(OUT, exist_ok=True)
    for c in calls:
        name = os.path.join(OUT, 'rx_%06d.bit' % c[0][0])
        with open(name, 'wb') as f:
            f.write(b''.join(r[2] for r in c))
        kinds = {}
        for r in c:
            kinds[r[3]] = kinds.get(r[3], 0) + 1
        gaps = [((c[i][1] - c[i - 1][1]) & 0xFFFF) for i in range(1, len(c))]
        late = sum(1 for g in gaps if g > 70)  # a burst (3 frames) every 60 ms: more is a late burst
        print('%s  %4d frames %5.1f s  kinds %s  late bursts %d  max gap %d ms' % (
            os.path.basename(name), len(c), len(c) * 0.02, ' '.join('%s%d' % kv for kv in sorted(kinds.items())),
            late, max(gaps) if gaps else 0))
        line = ''.join(r[3] for r in c)
        if set(line) - {'V'}:
            for i in range(0, len(line), 100):
                print('    %5d %s' % (i, line[i:i + 100]))


if __name__ == '__main__':
    main()
