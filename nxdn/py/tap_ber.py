"""The C receiver (src/services/nxdn_core.c, built for the PC by nxdn/c/run.py) on tap snapshots (tap_grab.py): the
voice frames it decodes against the frames sent (.bit), each matched to the nearest; the bit errors.

  python nxdn/py/tap_ber.py vocoder/data/dvsi/r33/clean.bit dvsi
"""
import glob
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..', '..')
DATA = os.path.join(ROOT, 'nxdn', 'data', 'radio')


def main():
    sent_path, name = sys.argv[1], sys.argv[2]
    d = open(sent_path, 'rb').read()
    sent = np.array([np.unpackbits(np.frombuffer(d[i:i + 9], np.uint8)) for i in range(0, len(d) - 8, 9)])
    subprocess.run([sys.executable, os.path.join(ROOT, 'nxdn', 'c', 'run.py'), 'none_at_all'], capture_output=True)
    exe = os.path.join(DATA, 'test_core.exe')
    out = os.path.join(DATA, 'tap_vch.bin')
    if os.path.exists(out):
        os.remove(out)
    raws = []
    for p in sorted(glob.glob(os.path.join(DATA, 'tap', name + '_*.npy'))):
        raw = p[:-4] + '.raw'
        np.load(p).astype('<i2').tofile(raw)
        raws.append(raw)
    env = dict(os.environ, NXDN_VCH_OUT=out, NXDN_OPT=os.environ.get('NXDN_OPT', '1'), PATH='C:/msys64/ucrt64/bin' + os.pathsep + os.environ['PATH'])
    subprocess.run([exe] + raws, env=env, capture_output=True)
    got = open(out, 'rb').read() if os.path.exists(out) else b''
    errs = []
    for i in range(0, len(got), 9):
        g = np.unpackbits(np.frombuffer(got[i:i + 9], np.uint8))
        errs.append(int((sent != g).sum(axis=1).min()))
    errs = np.array(errs)
    if len(errs) == 0:
        print('no voice frames')
        return
    print('%d snapshots, %d voice frames: bit errors per frame mean %.2f (BER %.2f %%), frames with errors %d, > 3: %d'
          % (len(raws), len(errs), errs.mean(), 100 * errs.mean() / 72, (errs > 0).sum(), (errs > 3).sum()))


if __name__ == '__main__':
    main()
