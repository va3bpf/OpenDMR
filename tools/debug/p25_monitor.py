#!/usr/bin/env python3
"""Watch the P25 receiver (src/services/p25_radio.c) over the programming cable for as long as it runs: every
block it receives is pulled from its tap, each call (no new block for --gap s: the end) is saved, decoded again on
the PC with p25/bin/p25_cli.exe (the WAV next to it), and printed next to the radio's own log of that call.

  py p25_monitor.py --port COM6 [--out ../../p25/out/calls] [--gap 2] [--min 2]

--min: signals with at least this many seconds of voice (LDUs x 180 ms, as decoded on the PC) are the ones reported;
shorter ones get a "skipped" line and go to OUT/short/.

The radio on a P25 channel or VFO. Ctrl+C stops. Per call, in OUT/<date>_<time>/: blocks.bin (per block ms u32,
RX_TYPE_INFO u32, 36 bytes, as the radio got them; ms of the interrupt), air.dibits (the stream on the air), decode.txt (p25_cli's
lines), voice.wav, and monitor.log in OUT with a line a call.
"""
import argparse
import datetime
import os
import shutil
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dm32dbg import ELF, Radio, Symbols  # noqa: E402
from p25_phy import bits_of, to_air  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = os.path.join(HERE, '..', '..', 'p25', 'bin', 'p25_cli.exe')
TAP_BLOCKS = 128
TAP_SIZE = 44
LOG_CALLS = 16
LOG_SIZE = 40
STATS = 'blocks stale overruns units ldus lc_bad imbe_frames imbe_repeat pcm_drops underruns rearms overlaps gaps'.split()


def place(blocks):
    """The air stream from the blocks by when they came, as p25_radio.c place() does: 30 ms the next 144 dibits,
    53 ms 112 dibits not given (zeros), ~7 ms a sync block 32 dibits into the last one, a repeated block (not
    written) a block of zeros, more than ~2 blocks late a break. Returns (dibits as bytes, repeated blocks)."""
    out = []
    held = None
    prev = None
    stale = 0
    for ms, kind, data in blocks:
        d = None
        if data == prev:
            stale += 1
        else:
            a = to_air(bits_of(data), kind & 3)
            if a is not None:
                d = [(a[2 * i] << 1) | a[2 * i + 1] for i in range(144)]
        prev = data
        if held is not None:
            dt = (ms - held[0]) & 0xFFFFFFFF
            if dt < 18:
                out += held[1][:32]
            elif dt < 42:
                out += held[1]
            elif dt < 66:
                out += held[1] + [0] * 112
            else:
                out += held[1] + [0] * 576  # the receiver loses the frame there
        held = (ms, d if d is not None else [0] * 144)
    if held is not None:
        out += held[1]
    return bytes(out), stale


class Monitor:
    def __init__(self, args):
        self.args = args
        self.radio = Radio(args.port)
        sym = Symbols(args.elf)
        self.a_tap = sym.resolve('p25_tap')
        self.a_head = sym.resolve('p25_tap_head')
        self.a_log = sym.resolve('p25_call_log')
        self.a_log_head = sym.resolve('p25_call_log_head')
        self.a_stats = sym.resolve('p25_radio_stats')
        self.head = self.u32(self.a_head)
        self.log_head = self.u32(self.a_log_head)
        self.blocks = []  # of the call being received
        self.last_block_time = 0
        self.lost = 0
        os.makedirs(args.out, exist_ok=True)
        self.logfile = open(os.path.join(args.out, 'monitor.log'), 'a', encoding='utf-8')

    def u32(self, addr):
        return struct.unpack('<I', self.radio.mem_read(addr, 4))[0]

    def read(self, addr, n):
        out = b''
        while len(out) < n:
            out += self.radio.mem_read(addr + len(out), min(256, n - len(out)))
        return out

    def say(self, text):
        line = '%s  %s' % (datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S'), text)
        print(line, flush=True)
        self.logfile.write(line + '\n')
        self.logfile.flush()

    def pull_blocks(self):
        head = self.u32(self.a_head)
        new = (head - self.head) & 0xFFFFFFFF
        if new == 0:
            return 0
        if new > TAP_BLOCKS:
            self.lost += new - TAP_BLOCKS
            self.say('lost %d blocks (the tap went round before it was read)' % (new - TAP_BLOCKS))
            self.head = (head - TAP_BLOCKS) & 0xFFFFFFFF
            new = TAP_BLOCKS
        # read them in index order, wrapping at the end of the ring
        i = self.head % TAP_BLOCKS
        first = min(new, TAP_BLOCKS - i)
        raw = self.read(self.a_tap + i * TAP_SIZE, first * TAP_SIZE)
        if new > first:
            raw += self.read(self.a_tap, (new - first) * TAP_SIZE)
        for k in range(new):
            r = raw[k * TAP_SIZE:(k + 1) * TAP_SIZE]
            ms, kind = struct.unpack('<II', r[:8])
            self.blocks.append((ms, kind, r[8:]))
        self.head = head
        self.last_block_time = time.time()
        return new

    def radio_calls(self):
        head = self.u32(self.a_log_head)
        out = []
        while self.log_head != head:
            i = self.log_head % LOG_CALLS
            r = self.read(self.a_log + i * LOG_SIZE, LOG_SIZE)
            start, end, nac, tg, src, dst, lco, enc, ldus, lc_bad, frames, rep = struct.unpack('<IIHHIIBBxxIIII', r)
            out.append(dict(start=start, end=end, nac=nac, tg=tg, src=src, dst=dst, lco=lco, enc=enc, ldus=ldus,
                            lc_bad=lc_bad, frames=frames, repeat=rep))
            self.log_head = (self.log_head + 1) & 0xFFFFFFFF
        return out

    def finish_call(self):
        blocks, self.blocks = self.blocks, []
        stamp = datetime.datetime.now().strftime('%Y%m%d_%H%M%S')
        d = os.path.join(self.args.out, stamp)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, 'blocks.bin'), 'wb') as f:
            for ms, kind, data in blocks:
                f.write(struct.pack('<II', ms, kind) + data)
        dib, stale = place(blocks)
        dpath = os.path.join(d, 'air.dibits')
        open(dpath, 'wb').write(dib)
        out = subprocess.run([CLI, dpath, os.path.join(d, 'voice.wav')], capture_output=True, text=True).stdout
        open(os.path.join(d, 'decode.txt'), 'w', encoding='utf-8').write(out)
        lines = out.splitlines()
        units = sum(1 for l in lines if ' NAC ' in l)
        ldus = sum(1 for l in lines if ' NAC ' in l and ' LDU' in l)
        calls = self.radio_calls()
        if ldus * 0.18 < self.args.min:
            short = os.path.join(self.args.out, 'short', stamp)
            os.makedirs(os.path.dirname(short), exist_ok=True)
            shutil.move(d, short)  # kept apart, for a look when something goes wrong
            self.say('skipped: %.2f s of voice (%d units, %d LDUs, %d blocks, %d repeated), under %.1f s (in short/)' %
                     (ldus * 0.18, units, ldus, len(blocks), stale, self.args.min))
            return
        lc_ok = sum(1 for l in lines if 'LC ok' in l)
        lc_all = sum(1 for l in lines if ' LC ' in l)
        es_ok = sum(1 for l in lines if 'ES ok' in l)
        es_all = sum(1 for l in lines if ' ES ' in l)
        tail = [l for l in lines if l.startswith('IMBE frames')]
        first_lc = next((l.split('LC ', 1)[1].split('  fec')[0] for l in lines if 'LC ok' in l), '-')
        span = (blocks[-1][0] - blocks[0][0]) / 1000.0 if blocks else 0
        self.say('PC  %s: %d blocks (%.1f s, %d repeated), %d units, %d LDUs, LC ok %d/%d, ES ok %d/%d, %s; LC: %s' %
                 (stamp, len(blocks), span, stale, units, ldus, lc_ok, lc_all, es_ok, es_all,
                  tail[0] if tail else 'no voice', first_lc))
        for c in calls:
            self.say('RADIO call %.1f s: NAC %03X %s TG %d src %d dst %d%s, %d LDUs, LC bad %d, IMBE %d frames %d '
                     'repeated' % ((c['end'] - c['start']) / 1000.0, c['nac'], 'unit' if c['lco'] == 3 else 'group',
                                   c['tg'], c['src'], c['dst'], ' ENCRYPTED' if c['enc'] else '', c['ldus'],
                                   c['lc_bad'], c['frames'], c['repeat']))

    def run(self):
        s = struct.unpack('<13I', self.read(self.a_stats, 52))
        self.say('watching on %s; the receiver so far: %s' % (self.args.port, dict(zip(STATS, s))))
        while True:
            try:
                n = self.pull_blocks()
            except RuntimeError as e:
                self.say('cable: %s' % e)
                time.sleep(2)
                continue
            if n and len(self.blocks) == n:
                self.say('signal: blocks coming in')
            if self.blocks and (time.time() - self.last_block_time) > self.args.gap:
                self.finish_call()
            time.sleep(self.args.poll)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', default='COM6')
    ap.add_argument('--elf', default=ELF)
    ap.add_argument('--out', default=os.path.join(HERE, '..', '..', 'p25', 'out', 'calls'))
    ap.add_argument('--gap', type=float, default=2.0)
    ap.add_argument('--min', type=float, default=2.0, help='keep signals with at least this many s of voice')
    ap.add_argument('--poll', type=float, default=0.3)
    args = ap.parse_args()
    try:
        Monitor(args).run()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
