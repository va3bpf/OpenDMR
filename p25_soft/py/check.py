#!/usr/bin/env python3
"""The C demodulator (src/services/p25_demod.c, built here) on a recording against what was sent: the dibits out, each
frame (from a sync) matched in the sent stream, the wrong ones counted.
  py p25_soft/py/check.py strong1 [-v]
"""
import json, os, subprocess, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..', '..')
sys.path.insert(0, os.path.join(ROOT, 'tools', 'debug'))
from p25_txcheck import sent_stream  # noqa: E402
GCC = os.environ.get('GCC', 'C:/msys64/ucrt64/bin/gcc.exe')
SYNC_D = [1, 1, 1, 1, 1, 3, 1, 1, 3, 3, 1, 1, 3, 3, 3, 3, 1, 3, 1, 3, 3, 3, 3, 3]
EXE = os.path.join(HERE, '..', 'c', 'test_demod.exe')


def build():
    src = os.path.join(ROOT, 'src', 'services')
    subprocess.run([GCC, '-O2', '-Wall', '-I' + src, '-o', EXE, os.path.join(HERE, '..', 'c', 'test_demod.c'),
                    os.path.join(src, 'p25_demod.c'), os.path.join(src, 'fsk4.c'), '-lm'], check=True,
                   env=dict(os.environ, PATH='C:/msys64/ucrt64/bin' + os.pathsep + os.environ['PATH']))


def check(name, verbose=False):
    base = os.path.join(HERE, '..', 'data', name)
    meta = json.load(open(base + '.json'))
    out = base + '.dibits'
    r = subprocess.run([EXE, base + '.raw', out], capture_output=True, text=True,
                       env=dict(os.environ, PATH='C:/msys64/ucrt64/bin' + os.pathsep + os.environ['PATH']))
    got = np.fromfile(out, np.uint8)
    sent = sent_stream(meta['fn0'], meta['src'], 80, meta['lead'], meta.get('tg', 1))
    sp = [i for i in range(len(sent) - 24) if np.array_equal(sent[i:i + 24], SYNC_D)]
    gp = []  # the frame syncs in the output, 3 dibits wrong at most (weak signals)
    for i in range(len(got) - 24):
        if np.sum(got[i:i + 24] != SYNC_D) <= 3 and not (gp and i - gp[-1] < 400):
            gp.append(i)
    frames = sym = wrong = 0
    for j, g0 in enumerate(gp):
        n = (gp[j + 1] - g0) if j + 1 < len(gp) else min(864, len(got) - g0)
        seg = got[g0:g0 + n]
        best = None
        for p in sp:
            if p + n <= len(sent):
                e = sent[p:p + n] != seg
                if best is None or e.sum() < best[0].sum():
                    best = (e, p)
        if best is None:
            continue
        e = best[0].copy()
        if n >= 864 and np.array_equal(sent[best[1] + 24:best[1] + 24], []):
            pass
        frames += 1
        sym += n
        wrong += int(e.sum())
        if verbose:
            print('  frame at %5d (%d symbols) sent at %5d: %d wrong' % (g0, n, best[1], int(e.sum())))
    print('%s: %s; %d frames, %d symbols, %d wrong (%.3f %%)' % (name, r.stdout.strip(), frames, sym, wrong,
                                                                 100.0 * wrong / max(sym, 1)))


if __name__ == '__main__':
    build()
    for n in [a for a in sys.argv[1:] if not a.startswith('-')]:
        check(n, '-v' in sys.argv)
