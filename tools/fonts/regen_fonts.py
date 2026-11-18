#!/usr/bin/env python3
"""Regenerates the UI Roboto fonts with Latin-1 supplement characters (accented letters).

  python tools/fonts/regen_fonts.py
"""
import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
TTF = os.path.join(HERE, 'ttf', 'RobotoCondensed-VF.ttf')
MKFONT = os.path.join(HERE, 'mkfont.py')

# (variable_name, size, weight, extra_args)
FONTS = [
    ('font_roboto12', 12, 'Regular', ['--latin1']),
    ('font_roboto12_bold', 12, 'Bold', ['--latin1']),
    ('font_roboto14', 14, 'Regular', ['--latin1']),
    ('font_roboto17', 17, 'Regular', ['--latin1']),
    ('font_roboto17_bold', 17, 'Bold', ['--latin1']),
    ('font_roboto20', 20, 'Regular', ['--latin1']),
    ('font_roboto22', 22, 'Regular', ['--latin1']),
    ('font_roboto32_bold', 32, 'Bold', ['--latin1']),
    ('font_roboto44_bold', 44, 'Bold', ['--latin1']),
    ('font_roboto42_bold', 42, 'Bold', ['--chars', '0123456789. ']),
]

for name, size, weight, extra in FONTS:
    cmd = ['python3', MKFONT, TTF, str(size), name, '--weight', weight] + extra
    subprocess.check_call(cmd)
