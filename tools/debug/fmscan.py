#!/usr/bin/env python3
"""Look for the GPIO that connects the antenna to the RDA5807M: the RSSI of the receiver (register 0x0B, bits 15-9)
with each candidate pin set high then low, restored afterwards. Tune a station first (dm32dbg.py fmw 0x03 ...).

  py tools/debug/fmscan.py 0x0A 0x24 0x22
"""
import os
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dm32dbg  # noqa: E402

BASES = (0x14020000, 0x14100000, 0x14110000)  # GPIOA, B, C: DR at +0, DDR at +4


def main():
    pins = [int(p, 0) for p in sys.argv[1:]]
    elf = dm32dbg.ELF
    out = subprocess.run([dm32dbg.NM, elf], capture_output=True, text=True).stdout
    sym = {l.split()[2]: int(l.split()[0], 16) for l in out.splitlines() if len(l.split()) == 3}
    radio = dm32dbg.Radio('COM6')

    def call(name, *args):
        a = list(args) + [0] * (4 - len(args))
        return struct.unpack('>I', radio.debug(b'x', struct.pack('>IIIII', sym[name], *a), 6)[2:6])[0]

    def rssi():
        vals = []
        for _ in range(5):
            r = radio.debug(b'Y', bytes([0x0B]), 5)
            vals.append(((r[3] << 8) | r[4]) >> 9)
            time.sleep(0.05)
        return sum(vals) / len(vals)

    def reg(addr):
        return struct.unpack('<I', radio.mem_read(addr, 4))[0]

    print('baseline RSSI %.1f' % rssi())
    for pin in pins:
        base = BASES[pin // 32]
        bit = 1 << (pin % 32)
        dr, ddr = reg(base), reg(base + 4)
        results = []
        for level in (1, 0):
            call('GPIO_DRV_SetOutput', pin, level)
            time.sleep(0.2)
            results.append('%d: %.1f' % (level, rssi()))
        # restore the direction and the level
        if ddr & bit:
            call('GPIO_DRV_SetOutput', pin, 1 if dr & bit else 0)
        else:
            call('GPIO_DRV_SetInput', pin, 0, 0)
        print('pin 0x%02X (was %s %d)  RSSI  %s' % (pin, 'out' if ddr & bit else 'in', 1 if dr & bit else 0, '   '.join(results)))
    print('final RSSI %.1f' % rssi())


if __name__ == '__main__':
    main()
