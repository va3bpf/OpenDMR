#!/usr/bin/env python3
"""The voice delay from one radio's PTT to the other's speaker, from both radios' delay logs (p25_ev, a DEBUG=1 build:
src/services/p25_radio.c), their tick counters aligned over the cables.

  py tools/debug/voice_delay.py --tx COM23 --rx COM22 [--hold 3] [--runs 3]

Both radios on the same simplex frequency in the same mode (M17, NXDN, P25, DMR). The script presses the sender's PTT
over the test interface, holds it, releases it, and reads both logs once at the end (no polling while it transmits).

Events: 29 a transmission begins (any mode). The playback (voice_play.c: M17, DMR, NXDN): 50 enough queued (arg frames),
51 the amplifier on (arg frames queued), 52 the first voice to the speaker (arg frames queued), 53 out of voice (arg
frames played so far). M17: 30 transmit() (the mic on), 31 keyed, 32 a stream frame into the FIFO (arg its number);
40 an LSF received, 41 a stream frame received (arg its number). DMR (dmr_soft.c): 21 its transmission starts, 33 a
voice LC header into the FIFO, 34 a voice burst into the FIFO; 42 a voice LC header received, 43 a voice burst received
(the first three; all of them with dmr_soft_log_all_voice 1); 20 the chip's playback starts (repeater mode). NXDN: 36 a
voice frame into the FIFO; 44 a call begins, 45 a voice frame received, 46 a hunt's candidates, 47 none of them
decoded, 48 the lock lost. P25: 1 the key, 2 an LDU into the FIFO, 3 RF on, 10 a data unit received, 11 playback on, 12
playback starts.
"""
import argparse
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from dm32dbg import ELF, Radio, Symbols  # noqa: E402
import radio as testlink  # noqa: E402

NAMES = {29: 'tx begins', 50: 'play: enough queued', 51: 'play: amp on', 52: 'play: first voice out',
         53: 'play: out of voice', 30: 'M17 transmit() / mic on', 31: 'M17 keyed', 32: 'M17 stream frame -> FIFO',
         40: 'M17 LSF received', 41: 'M17 stream frame received', 21: 'DMR tx starts', 33: 'DMR LC header -> FIFO',
         34: 'DMR voice burst -> FIFO', 35: 'DMR keyed', 37: 'DMR frame encoded (0.1 ms)', 39: 'AMBE encode (0.1 ms)', 38: 'DMR realigned', 42: 'DMR LC header received', 43: 'DMR voice burst received',
         20: 'DMR playback starts', 36: 'NXDN voice frame -> FIFO', 44: 'NXDN call begins',
         45: 'NXDN voice frame received', 46: 'NXDN hunt: candidates', 47: 'NXDN false lock', 48: 'NXDN lock lost',
         1: 'P25 key', 2: 'P25 LDU -> FIFO', 3: 'P25 RF on', 10: 'P25 DU received', 11: 'P25 playback on',
         12: 'P25 playback starts'}


class Side:
    def __init__(self, port, sym):
        self.r = Radio(port)
        self.ev = sym.resolve('p25_ev')
        self.n = sym.resolve('p25_ev_n')
        self.tick = sym.resolve('xTickCount')
        self.dmr_stats = sym.resolve('dmr_soft_stats')
        self.m17_stats = sym.resolve('m17_radio_stats')
        self.link = testlink.Radio.__new__(testlink.Radio)
        self.link.port = self.r.p    # the test interface on the same serial port

    def offset(self, k=15):
        """the radio's tick (ms) minus the host's ms: from the read with the shortest round trip"""
        best = None
        for _ in range(k):
            t0 = time.perf_counter()
            tick = self.r.value_read(self.tick, 4)
            t1 = time.perf_counter()
            rtt = t1 - t0
            if best is None or rtt < best[0]:
                best = (rtt, tick - (t0 + t1) / 2 * 1000)
        return best[1], best[0] * 1000

    def underruns(self):
        """the playback's underruns (16-sample blocks of silence played in a call): DMR's software receiver, M17"""
        return (self.r.value_read(self.dmr_stats + 4 * 16, 4), self.r.value_read(self.m17_stats + 4 * 9, 4))

    def clear(self):
        self.r.value_write(self.n, 4, 0)

    def read(self):
        n = min(self.r.value_read(self.n, 4), 64)
        raw = self.r.mem_read(self.ev, 8 * n) if n else b''
        return [struct.unpack('<IHH', raw[8 * i:8 * i + 8]) for i in range(n)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--tx', default='COM22')
    ap.add_argument('--rx', default='COM23')
    ap.add_argument('--hold', type=float, default=3.0)
    ap.add_argument('--runs', type=int, default=3)
    ap.add_argument('--elf', default=ELF)
    a = ap.parse_args()
    sym = Symbols(a.elf)
    tx, rx = Side(a.tx, sym), Side(a.rx, sym)
    summary = []
    for run in range(a.runs):
        otx, rtt_tx = tx.offset()
        orx, rtt_rx = rx.offset()
        tx.clear()
        rx.clear()
        u0 = rx.underruns()
        time.sleep(0.5)
        tx.link.ptt(True)
        time.sleep(a.hold)
        tx.link.ptt(False)
        time.sleep(2.0)
        events = [('TX', ms - otx, e, arg) for ms, e, arg in tx.read()]
        events += [('RX', ms - orx, e, arg) for ms, e, arg in rx.read()]
        u1 = rx.underruns()
        otx2, _ = tx.offset()
        orx2, _ = rx.offset()
        events.sort(key=lambda x: x[1])
        begin = next((t for s, t, e, _ in events if s == 'TX' and e == 29), None)
        if begin is None:
            print('run %d: no transmission logged' % run)
            continue
        print('\nrun %d (clock read round trips %.1f / %.1f ms, drift over the run %.1f / %.1f ms)'
              % (run, rtt_tx, rtt_rx, otx2 - otx, orx2 - orx))
        first = {}
        for s, t, e, arg in events:
            first.setdefault((s, e), t - begin)
            print('  %s %7.1f ms  %2d %-28s arg %d' % (s, t - begin, e, NAMES.get(e, '?'), arg))
        out = first.get(('RX', 52), first.get(('RX', 20), first.get(('RX', 12))))
        mic = first.get(('TX', 30))
        summary.append((out, mic))
        print('  receiver underruns this run: DMR %d, M17 %d' % (u1[0] - u0[0], u1[1] - u0[1]))
        print('  => PTT to the first voice out: %s ms; mic on to the first voice out: %s ms'
              % ('%.0f' % out if out is not None else '-', '%.0f' % (out - mic) if (out is not None and mic is not None)
                 else '-'))
    print('\nsummary (PTT -> voice out):', ', '.join('%.0f' % s[0] for s in summary if s[0] is not None))


if __name__ == '__main__':
    main()
