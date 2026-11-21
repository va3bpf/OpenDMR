#!/usr/bin/env python3
"""P25 C4FM in software from the 8 kHz discriminator recording (capture.py), against what was sent.

  py p25_soft/py/demod.py strong1 [--filter none|rrc] [--relevel] [--eq]

4800 symbols/s: 5/3 samples a symbol, read by cubic interpolation. The frame syncs (24 symbols, 0x5575F5FF77FF) found by
correlation every sixth of a sample, refined, gain and DC by least squares; the symbols between two syncs on a time line
from one to the next; each sync's frame matched in the sent stream (p25_txcheck.py sent_stream()) and the wrong symbols
counted.
"""
import argparse
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..', '..')
sys.path.insert(0, os.path.join(ROOT, 'tools', 'debug'))
from p25_txcheck import sent_stream  # noqa: E402

SPS = 8000 / 4800
SYNC_D = [1, 1, 1, 1, 1, 3, 1, 1, 3, 3, 1, 1, 3, 3, 3, 3, 1, 3, 1, 3, 3, 3, 3, 3]  # dibits (01 +3, 11 -3)
LEVEL = {1: 3, 0: 1, 2: -1, 3: -3}
SYNC = np.array([LEVEL[d] for d in SYNC_D], float)


def cubic(x, t):
    """x at times t (samples), Catmull-Rom"""
    i = np.floor(t).astype(int)
    f = t - i
    i = np.clip(i, 1, len(x) - 3)
    p0, p1, p2, p3 = x[i - 1], x[i], x[i + 1], x[i + 2]
    return p1 + 0.5 * f * (p2 - p0 + f * (2 * p0 - 5 * p1 + 4 * p2 - p3 + f * (3 * (p1 - p2) + p3 - p0)))


def rrc(alpha=0.2, span=6):
    t = np.arange(-span * SPS, span * SPS + 1) / SPS
    h = np.zeros_like(t)
    for k, x in enumerate(t):
        if abs(x) < 1e-9:
            h[k] = 1 - alpha + 4 * alpha / np.pi
        elif abs(abs(4 * alpha * x) - 1) < 1e-6:
            h[k] = alpha / np.sqrt(2) * ((1 + 2 / np.pi) * np.sin(np.pi / (4 * alpha)) + (1 - 2 / np.pi) * np.cos(np.pi / (4 * alpha)))
        else:
            h[k] = (np.sin(np.pi * x * (1 - alpha)) + 4 * alpha * x * np.cos(np.pi * x * (1 + alpha))) / (np.pi * x * (1 - (4 * alpha * x) ** 2))
    return h / h.sum()


def fit(v, p):
    """v ~ g p + dc by least squares: g, dc, mean squared error in levels^2"""
    A = np.stack([p, np.ones_like(p)], 1)
    (g, dc), *_ = np.linalg.lstsq(A, v, rcond=None)
    e = np.mean(((v - dc) / g - p) ** 2) if g != 0 else 99
    return g, dc, e


def find_syncs(x, err_max=0.5):
    """sync starts (samples, fractional), their gain and DC: the best within a frame's span of each"""
    steps = np.arange(5, len(x) - 24 * SPS - 5, 1 / 6)
    k = np.arange(24) * SPS
    out = []
    for t in steps:
        v = cubic(x, t + k)
        c = np.dot(v - v.mean(), SYNC) / (np.linalg.norm(v - v.mean()) * np.linalg.norm(SYNC) + 1e-9)
        if abs(c) > 0.85:
            g, dc, e = fit(v, SYNC)
            if e < err_max:
                if out and t - out[-1][0] < 10 * SPS:
                    if e < out[-1][3]:
                        out[-1] = (t, g, dc, e)
                    continue
                out.append((t, g, dc, e))
    return out


def decide(z):
    return np.where(z >= 2, 1, np.where(z >= 0, 0, np.where(z >= -2, 2, 3)))


def relevel(z):
    d = decide(z)
    m = {}
    for dib, lv in LEVEL.items():
        sel = z[d == dib]
        m[lv] = sel.mean() if len(sel) else lv
    lv = sorted(m)
    th = [(m[lv[i]] + m[lv[i + 1]]) / 2 for i in range(3)]  # thresholds -2, 0, +2 equivalents
    out = np.interp(z, [m[-3], th[0], th[1], th[2], m[3]], [-3, -2, 0, 2, 3], left=None, right=None)
    out = np.where(z < m[-3], -3 + (z - m[-3]) * 1.0, out)
    out = np.where(z > m[3], 3 + (z - m[3]) * 1.0, out)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('name')
    ap.add_argument('--filter', default='none')
    ap.add_argument('--relevel', action='store_true')
    ap.add_argument('--eq', type=float, default=0.0, help='3-tap equaliser side tap (e.g. -0.05)')
    ap.add_argument('--alpha', type=float, default=0.2)
    ap.add_argument('--span', type=int, default=6)
    ap.add_argument('-v', action='store_true')
    a = ap.parse_args()
    base = os.path.join(HERE, '..', 'data', a.name)
    meta = json.load(open(base + '.json'))
    x = np.fromfile(base + '.raw', '<i2').astype(float)
    if a.filter == 'iad':  # integrate and dump over one symbol (TIA-102.BAAA receive filter)
        h = np.array([1 / 3, 1, 1 / 3])  # 5/3 samples: the middle sample whole, a third of each neighbour
        x = np.convolve(x, h / h.sum(), 'same')
    if a.filter == 'rrc':
        x = np.convolve(x, rrc(a.alpha, a.span), 'same')
    sent = sent_stream(meta['fn0'], meta['src'], 60, meta['lead'], meta.get('tg', 1))
    # the sent stream's syncs: where each frame starts
    sp = [i for i in range(len(sent) - 24) if np.array_equal(sent[i:i + 24], SYNC_D)]
    syncs = find_syncs(x)
    if a.v:
        print('syncs found at', [round(s[0], 1) for s in syncs])
    total = wrong = 0
    frames = 0
    for j in range(len(syncs) - 1):
        t0, g, dc, _ = syncs[j]
        t1 = syncs[j + 1][0]
        n = int(round((t1 - t0) / SPS))  # symbols from this sync to the next
        per = (t1 - t0) / n
        z = (cubic(x, t0 + np.arange(n) * per) - dc) / g
        if a.relevel:
            z = relevel(z)
        if a.eq:  # z[k] (1 - 2 eq) ... as DMR's (-0.05, 0.95... with eq = -0.05: 1.1 z[k] - 0.05 (z[k-1] + z[k+1]))
            y = z.copy()
            y[1:-1] = (1 - 2 * a.eq) * z[1:-1] + a.eq * (z[:-2] + z[2:])
            z = y
        d = decide(z)
        # this frame in the sent stream: the sent sync whose following symbols match best
        best = None
        for p in sp:
            if p + n <= len(sent):
                e = int(np.sum(sent[p:p + n] != d))
                if best is None or e < best[0]:
                    best = (e, p)
        if best is None or best[0] > n // 3:
            if a.v:
                print('frame at %.1f: %d symbols, not matched' % (t0, n))
            continue
        frames += 1
        total += n
        wrong += best[0]
        if a.v:
            print('frame at %8.1f: %4d symbols, sent at %5d, wrong %d' % (t0, n, best[1], best[0]))
    print('%s: %d frames, %d symbols, %d wrong (%.3f %%)' % (a.name, frames, total, wrong, 100.0 * wrong / max(total, 1)))


if __name__ == '__main__':
    main()
