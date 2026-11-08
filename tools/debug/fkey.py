#!/usr/bin/env python3
"""Key events for our firmware (src/) over the cable: posted to the UI queue (ui_post()) as the input task would,
through a scratch ui_event_t written into the hotspot's frame buffer (unused while the hotspot is off).

  py tools/debug/fkey.py down down green          a press and a release each (UP / DOWN / LEFT / RIGHT / GREEN / RED /
                                                  0 - 9 / STAR / HASH / SK1 / SK2 / PTT)
  py tools/debug/fkey.py long:green               press, long, release (long)
  py tools/debug/fkey.py sk2+down                 with SK2 held (modifiers)
  py tools/debug/fkey.py rot:3 rot:-1             the knob
"""
import os
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dm32dbg  # noqa: E402

ELF = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'src', 'build',
                   'opendmr.elf')
KEYS = ['green', '3', '2', '1', 'left', '6', '5', '4', 'right', '9', '8', '7', 'red', 'hash', '0', 'star', 'up',
        'down', 'sk1', 'sk2', 'ptt']
PRESS, LONG, REPEAT, RELEASE, ROTARY = 0, 1, 2, 3, 4
MOD = {'sk1': 1, 'sk2': 2}


def main():
    sym = {}
    for line in subprocess.run([dm32dbg.NM, ELF], capture_output=True, text=True).stdout.splitlines():
        p = line.split()
        if len(p) == 3:
            sym[p[2]] = int(p[0], 16)
    radio = dm32dbg.Radio(os.environ.get('DM32_PORT', 'COM6'))
    scratch = sym['rx_frames']

    def post(kind, key=0, modifiers=0, long_press=False, steps=0):
        radio.mem_write_bytes(scratch, struct.pack('<IBBBxhH', kind, key, modifiers, int(long_press), steps, 0))
        radio.call(sym['ui_post'], [scratch])
        time.sleep(0.15)

    for arg in sys.argv[1:]:
        arg = arg.lower()
        if arg.startswith('rot:'):
            post(ROTARY, steps=int(arg[4:]))
            continue
        long_press = arg.startswith('long:')
        arg = arg[5:] if long_press else arg
        modifiers = 0
        while '+' in arg:
            m, arg = arg.split('+', 1)
            modifiers |= MOD[m]
        key = KEYS.index(arg)
        post(PRESS, key, modifiers)
        if long_press:
            post(LONG, key, modifiers)
        post(RELEASE, key, modifiers, long_press)


if __name__ == '__main__':
    main()
