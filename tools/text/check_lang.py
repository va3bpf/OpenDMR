#!/usr/bin/env python3
"""Checks a language file before it is written to the radio or sent in a PR (docs/i18n.en.md):

    py tools/text/check_lang.py tools/text/languages/de.json

Errors (the CPS will not write it): fields, format specifiers different from English, too big for a slot.
Warnings: strings missing (shown in English), keys English no longer has.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import langfile  # noqa: E402


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    doc = langfile.load(sys.argv[1])
    english = langfile.load(os.path.join(HERE, 'languages', 'en.json'))
    errors, warnings = langfile.check(doc, english)
    for line in errors:
        print('error: ' + line)
    for line in warnings:
        print(line)
    if errors:
        raise SystemExit(1)
    slot = langfile.build_slot(doc)
    print('%s (%s): %d strings, %d of %d bytes of a slot' % (doc['code'], doc['english_name'], len(doc['strings']),
                                                              len(slot), langfile.SLOT_SIZE))


if __name__ == '__main__':
    main()
