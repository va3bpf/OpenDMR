#!/usr/bin/env python3
"""The P25 (IMBE) encoder on the DM-32, over the programming cable (vocoder/py/radio_link.py: the debug calls on a big
stack). The radio on a P25 channel or VFO (the encoder is in the P25 overlay), the ELF of src/build on it.

  py imbe_bench.py --port COM14 time            ms a frame (100 frames of speech)
  py imbe_bench.py --port COM14 exact [frames]  frame by frame against the PC (p25/bin/imbe_enc_cli.exe), on the
                                                THCHS-30 speech of p25/out/hotspot/speech.raw
"""
import argparse
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..', '..')
sys.path.insert(0, os.path.join(ROOT, 'vocoder', 'py'))
from radio_link import Link  # noqa: E402

SPEECH = os.path.join(ROOT, 'p25', 'out', 'hotspot', 'speech.raw')
ENC = os.path.join(ROOT, 'p25', 'bin', 'imbe_enc_cli.exe')
MHZ = 168


def bits_to_bytes(u):
    return bytes(sum(u[8 * i + k] << (7 - k) for k in range(8)) for i in range(11))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', default='COM6')
    ap.add_argument('what', choices=('time', 'exact'))
    ap.add_argument('frames', nargs='?', type=int, default=200)
    a = ap.parse_args()
    pcm = open(SPEECH, 'rb').read()
    link = Link(a.port, ('vocoder_bench', 'imbe_enc_test', 'imbe_enc_ready'))
    if a.what == 'time':
        link.write(link.s['imbe_enc_ready'], b'\x00')
        link._write_long(link.scratch, pcm[320 * 150:320 * 154])  # 4 frames of speech, over and over
        ms = link.big_stack_call(link.s['vocoder_bench'], 5 << 16 | 100, link.scratch)
        if ms == 0xFFFFFFFF:
            raise SystemExit('the P25 overlay is not loaded: the radio on a P25 channel')
        print('IMBE encoder %.2f ms a frame (%d ms for 100; a frame is 20 ms; %.1f M cycles a second at %d MHz)' %
              (ms / 100, ms, ms / 100 / 20 * MHZ, MHZ))
        return
    n = a.frames
    x = pcm[:320 * n]
    with tempfile.TemporaryDirectory() as d:
        raw, out = os.path.join(d, 'in.raw'), os.path.join(d, 'out.imbe')
        open(raw, 'wb').write(x)
        subprocess.run([ENC, raw, out], check=True, capture_output=True)
        pc = open(out, 'rb').read()
    pc = [pc[11 * i:11 * i + 11] for i in range(len(pc) // 11)]
    link.write(link.s['imbe_enc_ready'], b'\x00')  # a fresh encoder, as imbe_enc_cli's
    pin, pout = link.scratch, link.scratch + 0x200
    radio = []
    for k in range(n):
        link.write(pin, x[320 * k:320 * (k + 1)])
        r = link.big_stack_call(link.s['imbe_enc_test'], pin, pout)
        if r == 0xFFFFFFFF:
            raise SystemExit('the P25 overlay is not loaded: the radio on a P25 channel')
        if r == 1:
            radio.append(bits_to_bytes(list(link.read(pout, 88))))
    m = min(len(radio), len(pc))
    bad = [k for k in range(m) if radio[k] != pc[k]]
    print('encoder: %d frames, %d the same as the PC%s' % (m, m - len(bad), ', first different %d' % bad[0] if bad
                                                           else ''))


if __name__ == '__main__':
    main()
