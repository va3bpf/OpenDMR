#!/usr/bin/env python3
"""Level of the mic in the last DMR transmission. The cable's plug switches the radio's own mic off, so the firmware
records the level of each 10 ms mic buffer while transmitting (dmr/dmr_sound.c mic_levels, about 10 s): unplug the
cable, transmit on a DMR channel and talk normally, plug the cable back in, then

  py tools/debug/miclevel.py [--profile] [--port COM6] [--elf src/build/opendmr.elf]

The first 200 ms are left out (the ADC settles from about 12000 to the mic's DC of about 2400 when the capture starts).
For reference, speech material at the usual level (vocoder/out/dam_input.wav): loud 10 ms blocks (90th percentile of
the speech) about -19 dBFS, peak about -1 dBFS.
"""
import argparse
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dm32dbg  # noqa: E402

MIC_LEVELS = 1024
SETTLE = 20  # buffers left out at the start
CHUNK = 512  # bytes a read
GCR_MIC1 = 0x160009E3  # the codec's analog mic boost, 4 dB a step
GCR_ADCL = 0x160009E5  # its digital gain, 1 dB a step (bits 5-0)
SPEECH_DB = 15  # blocks this far above the quietest tenth are speech


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', default='COM6')
    ap.add_argument('--elf', default=dm32dbg.ELF)
    ap.add_argument('--profile', action='store_true', help='the level every 100 ms')
    args = ap.parse_args()

    symbols = dm32dbg.Symbols(args.elf)
    radio = dm32dbg.Radio(args.port)
    boost = radio.value_read(GCR_MIC1, 1) & 0x07
    gain = radio.value_read(GCR_ADCL, 1) & 0x3F
    print('mic gain now: analog boost %d dB, digital %d dB' % (min(boost, 5) * 4, gain))
    count = radio.value_read(symbols.resolve('mic_level_count'), 4)
    n = min(count, MIC_LEVELS)
    if n <= SETTLE:
        print('nothing recorded: transmit on a DMR channel with the cable out first')
        return
    base = symbols.resolve('mic_levels')
    raw = b''
    while len(raw) < MIC_LEVELS * 8:
        raw += radio.mem_read(base + len(raw), min(CHUNK, MIC_LEVELS * 8 - len(raw)))
    entries = [struct.unpack_from('<hhI', raw, i * 8) for i in range(MIC_LEVELS)]
    entries = [entries[(count - n + i) % MIC_LEVELS] for i in range(n)]  # oldest first
    if count <= MIC_LEVELS:
        entries = entries[SETTLE:]
    low = np.array([e[0] for e in entries], dtype=float)
    high = np.array([e[1] for e in entries], dtype=float)
    levels = 10 * np.log10(np.maximum(np.array([e[2] for e in entries], dtype=float) * 16, 1) / 32768.0 ** 2)

    if args.profile:
        for i in range(0, len(levels), 10):
            print('%5.1f s %6.1f dBFS' % ((i + SETTLE) / 100, levels[i:i + 10].max()))
    dc = np.median((low + high) / 2)
    quiet = np.percentile(levels, 10)
    speech = levels[levels > quiet + SPEECH_DB]
    print('%.1f s recorded; background %.1f dBFS' % (len(levels) / 100, quiet))
    if len(speech):
        print('speech (%.1f s): loud blocks (90th percentile) %.1f, loudest %.1f dBFS' %
              (len(speech) / 100, np.percentile(speech, 90), speech.max()))
    peak = max(high.max() - dc, dc - low.min())
    print('peak %.1f dBFS%s' % (20 * np.log10(peak / 32768),
                                ', CLIPPING' if high.max() > 32000 or low.min() < -32000 else ''))


if __name__ == '__main__':
    main()
