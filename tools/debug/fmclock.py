#!/usr/bin/env python3
"""Try the reference clock settings of the RDA5807M (register 0x02: CLK_MODE bits 6-4, RCLK direct input bit 10):
for each, tune a frequency and read the RSSI and the stereo flag. The right clock receives the station.

  py tools/debug/fmclock.py 96.3
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dm32dbg  # noqa: E402

CLOCKS = {0: '32.768 kHz', 1: '12 MHz', 5: '24 MHz', 2: '13 MHz', 6: '26 MHz', 3: '19.2 MHz', 7: '38.4 MHz'}


def main():
    mhz = float(sys.argv[1]) if len(sys.argv) > 1 else 96.3
    chan = int(round((mhz - 87.0) * 10))
    radio = dm32dbg.Radio('COM6')

    def w(reg, value):
        radio.debug(b'Z', bytes([reg, value >> 8, value & 0xFF]), 3)

    def r(reg):
        x = radio.debug(b'Y', bytes([reg]), 5)
        return (x[3] << 8) | x[4]

    for direct in (0, 1):
        for mode, name in CLOCKS.items():
            ctrl = 0xC001 | (mode << 4) | (direct << 10)
            w(0x02, 0x0002)
            time.sleep(0.05)
            w(0x02, ctrl)
            time.sleep(0.5)
            w(0x03, (chan << 6) | 0x10)
            time.sleep(0.4)
            rssi = sum(r(0x0B) >> 9 for _ in range(4)) / 4
            st = r(0x0A)
            print('%-10s direct=%d  ctrl=%04X  RSSI %.1f  %s  chan %d' % (name, direct, ctrl, rssi,
                  'stereo' if st & 0x0400 else 'mono  ', st & 0x3FF))


if __name__ == '__main__':
    main()
