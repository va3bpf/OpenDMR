"""Language files (docs/i18n.en.md): reading and checking a <code>.json, and the 16 KB slot the radio keeps it in.

Shared by gen_text.py, check_lang.py and tools/lang/lang_tool.py; the CPS does the same in JavaScript (keep in step).

Slot, little endian (src/services/language.c):
  0   "DM32LANG"
  8   u32 length
  12  u32 CRC-32 of bytes 16 .. length
  16  char code[8]
  24  char name[32]          UTF-8, in its own script
  56  char english_name[24]
  80  char font[8]           the font file it needs ("" when the firmware's own fonts are enough)
  88  u16 number of strings, 2 bytes padding
  92  number x { u32 FNV-1a hash of the TEXT_x name, u16 offset of the string from the slot start }
      then the strings, UTF-8, NUL terminated
"""
import json
import re
import struct
import zlib

FORMAT = 1
MAGIC = b'DM32LANG'
SLOT_SIZE = 16384
HEADER_SIZE = 92
CODE_MAX = 7
NAME_MAX = 31
ENGLISH_NAME_MAX = 23
FONT_MAX = 7
SPEC = re.compile(r'%[-+ #0-9.*]*[a-zA-Z%]')  # the format specifiers of a string


def fnv1a(name):
    h = 0x811C9DC5
    for b in name.encode('ascii'):
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def load(path):
    with open(path, encoding='utf-8') as f:
        return json.load(f)


def check(doc, english=None):
    """Errors (the file cannot be used) and warnings (it can), as two lists of strings."""
    errors, warnings = [], []
    if doc.get('format') != FORMAT:
        errors.append('format must be %d' % FORMAT)
    for field, limit in (('code', CODE_MAX), ('name', NAME_MAX), ('english_name', ENGLISH_NAME_MAX)):
        value = doc.get(field)
        if not isinstance(value, str) or not value:
            errors.append('%s is missing' % field)
        elif len(value.encode('utf-8')) > limit:
            errors.append('%s is longer than %d bytes' % (field, limit))
    code = doc.get('code', '')
    if isinstance(code, str) and not re.fullmatch(r'[a-z]{2,3}(-[A-Za-z0-9]{2,3})?', code):
        errors.append('code "%s" is not a language code (zh, es, pt-BR, ...)' % code)
    font = doc.get('font', '')
    if not isinstance(font, str) or len(font.encode('ascii', 'replace')) > FONT_MAX:
        errors.append('font must be a name of at most %d characters' % FONT_MAX)
    strings = doc.get('strings')
    if not isinstance(strings, dict) or not strings:
        errors.append('strings is missing')
        return errors, warnings
    for key, value in strings.items():
        if not re.fullmatch(r'TEXT_[A-Z0-9_]+', key):
            errors.append('%s: not a TEXT_x name' % key)
        elif not isinstance(value, str):
            errors.append('%s: not a string' % key)
        elif '\0' in value:
            errors.append('%s: has a NUL' % key)
    if errors:
        return errors, warnings
    if len(build_slot(doc, check_size=False)) > SLOT_SIZE:
        errors.append('more than a %d byte slot' % SLOT_SIZE)
    if english is not None:
        en = english['strings']
        for key, value in strings.items():
            if key not in en:
                warnings.append('%s: not in English (removed?), ignored by the radio' % key)
            elif SPEC.findall(value) != SPEC.findall(en[key]):
                errors.append('%s: has %s, English has %s' % (key, SPEC.findall(value), SPEC.findall(en[key])))
        missing = [key for key in en if key not in strings]
        if missing:
            warnings.append('%d missing (shown in English): %s' % (len(missing), ', '.join(missing)))
    hashes = {}
    for key in strings:
        h = fnv1a(key)
        if h in hashes:
            errors.append('%s and %s have the same hash' % (hashes[h], key))
        hashes[h] = key
    return errors, warnings


def _field(text, size):
    data = text.encode('utf-8')
    return data + bytes(size - len(data))


def build_slot(doc, check_size=True):
    strings = doc['strings']
    entries = bytearray()
    blob = bytearray()
    start = HEADER_SIZE + (6 * len(strings))
    for key, value in strings.items():
        entries += struct.pack('<IH', fnv1a(key), start + len(blob))
        blob += value.encode('utf-8') + b'\0'
    body = (_field(doc['code'], 8) + _field(doc['name'], 32) + _field(doc['english_name'], 24) +
            _field(doc.get('font', ''), 8) + struct.pack('<H2x', len(strings)) + entries + blob)
    length = 16 + len(body)
    if check_size and length > SLOT_SIZE:
        raise ValueError('the language takes %d bytes, a slot %d' % (length, SLOT_SIZE))
    return MAGIC + struct.pack('<II', length, zlib.crc32(body) & 0xFFFFFFFF) + body
