#!/usr/bin/env python3
"""The test interface of the firmware over the programming cable (src/app/test_link.c): keys as the keypad gives them
and the state as text, so a test needs no screenshots and no ELF symbols (any firmware with 'D' 'k' / 'D' 'q').

    from radio import Radio
    r = Radio('COM22')
    r.state()                      # {'screen': 'vfo', 'rx': '14410000', 'mode': 'FM', 'scan': 'off', ...}
    r.key('star')                  # a press and release; r.key('star', long=True); r.key('up', mod='sk1')
    r.ptt(True); ...; r.ptt(False)
    r.home()                       # back to the channel or VFO screen
    r.main_menu('Scan lockouts')   # the main menu, then that row (by its name as shown), opened
    r.quick_menu('Scan lockout')   # the quick menu, on that row
    r.tune(144.1)                  # VFO: the receive frequency typed in, checked
    r.wait(lambda s: s['scan'] == 'on', 3)

    py tools/debug/radio.py COM22 [state | key star | long star | ptt on | ptt off | home]
"""
import struct
import sys
import time

import serial

KEYS = ['green', '3', '2', '1', 'left', '6', '5', '4', 'right', '9', '8', '7', 'red', 'hash', '0', 'star', 'up',
        'down', 'sk1', 'sk2', 'ptt']  # src/ui/ui.h UI_KEY_x
MODIFIERS = {None: 0, 'sk1': 1, 'sk2': 2, 'sk1+sk2': 3}
PRESS, LONG, REPEAT, RELEASE, CLICK, LONG_CLICK = range(6)
HOME_SCREENS = ('vfo', 'channel')


class Radio:
    def __init__(self, port, baud=115200):
        self.port = serial.Serial(port, baud, timeout=0.5)
        time.sleep(0.05)

    def close(self):
        self.port.close()

    def _request(self, data, header):
        for _ in range(3):
            self.port.reset_input_buffer()
            self.port.write(data)
            got = self.port.read(len(header))
            if got == header:
                return True
            time.sleep(0.05)
        raise RuntimeError('no answer to %r (a firmware without the test interface, locked, or busy?)' % data[:2])

    # ---- keys

    def event(self, key, kind, mod=None):
        self._request(b'Dk' + bytes([KEYS.index(key), kind, MODIFIERS[mod]]), b'Dk')
        self.port.read(1)

    def key(self, name, long=False, mod=None, settle=0.15):
        """A press and release ('star', 'up' ...), held long with long=True, SK1 / SK2 held with mod='sk1'"""
        self.event(name, LONG_CLICK if long else CLICK, mod)
        time.sleep(settle)

    def keys(self, *names, settle=0.12):
        for n in names:
            self.key(n, settle=settle)

    def ptt(self, on):
        self.event('ptt', PRESS if on else RELEASE)

    def beacon(self, frequency=0):
        """A test APRS beacon (400 bits of flags) through the radio task, on the channel (0) or on frequency (10 Hz
        units); True: accepted"""
        self._request(b'Da' + frequency.to_bytes(4, 'big'), b'Da')
        return self.port.read(1) == b''

    def data(self, ms, fail=False):
        """A test data burst through the radio task (no DMR engine, no RF): sent for ms, or failing; True: accepted"""
        self._request(b'Dd' + bytes([(ms >> 8) & 0xFF, ms & 0xFF, 1 if fail else 0]), b'Dd')
        return self.port.read(1) == b''

    # ---- the state

    def state(self):
        self._request(b'Dq', b'Dq')
        n = struct.unpack('>H', self.port.read(2))[0]
        text = self.port.read(n).decode('utf-8', 'replace')
        return dict(line.split('=', 1) for line in text.splitlines() if '=' in line)

    def wait(self, condition, timeout=3.0, every=0.05):
        """The state once condition(state) holds; RuntimeError (with the last state) after timeout s"""
        end = time.time() + timeout
        while True:
            s = self.state()
            if condition(s):
                return s
            if time.time() > end:
                raise RuntimeError('timed out; state: %s' % s)
            time.sleep(every)

    # ---- getting somewhere

    def home(self):
        """The channel or VFO screen (the back key held: home)"""
        if self.state()['screen'] not in HOME_SCREENS:
            self.key('red', long=True)
        return self.wait(lambda s: s['screen'] in HOME_SCREENS)

    def row(self, name, timeout=3.0):
        """In the list on top, the selection moved to the row of that name (as shown, its language)"""
        s = self.state()
        for _ in range(int(s.get('count', '0') or 0) + 1):
            if s.get('row') == name:
                return s
            self.key('down', settle=0.05)
            s = self.state()
        raise RuntimeError('no row %r in %s' % (name, s.get('title')))

    def main_menu(self, *rows):
        """The main menu, then each row by name opened in turn: main_menu('Options', 'Display options')"""
        self.home()
        self.key('green', long=True)
        s = self.wait(lambda s: s['screen'] == 'list')
        for name in rows:
            title = s.get('title')
            self.row(name)
            self.key('green')
            s = self.wait(lambda s: s.get('title') != title or s['screen'] != 'list')
        return s

    def quick_menu(self, row=None):
        """The quick menu of the home screen, on that row if given"""
        self.home()
        self.key('green')
        s = self.wait(lambda s: s['screen'] == 'quick_menu')
        return self.row(row) if row else s

    def tune(self, mhz, attempts=3):
        """VFO: the receive frequency typed in, all its digits, again if it did not take (10 Hz units in the state)"""
        units = int(round(mhz * 100000))
        self.home()
        for _ in range(attempts):
            self.keys(*('%08d' % units), settle=0.15)  # 8 digits: the entry ends by itself
            time.sleep(0.2)
            s = self.state()
            if s.get('rx') == str(units):
                return s
            self.home()
        raise RuntimeError('tune %.5f: the VFO is on %s' % (mhz, s.get('rx')))


def main():
    sys.stdout.reconfigure(encoding='utf-8')  # channel names in Chinese
    r = Radio(sys.argv[1])
    args = sys.argv[2:] or ['state']
    if args[0] == 'state':
        for k, v in r.state().items():
            print('%s=%s' % (k, v))
    elif args[0] in ('key', 'long'):
        r.key(args[1], long=(args[0] == 'long'))
    elif args[0] == 'ptt':
        r.ptt(args[1] == 'on')
    elif args[0] == 'home':
        print(r.home())


if __name__ == '__main__':
    main()
