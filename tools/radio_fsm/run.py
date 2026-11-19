"""Builds tools/radio_fsm/test_fsm.c with src/services/radio_fsm.c (gcc on the PC) and runs it: every state of the
radio task's state machine against every event, then sequences.

  python tools/radio_fsm/run.py          GCC=... to use another gcc
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', '..', 'src', 'services')
GCC = os.environ.get('GCC', 'C:/msys64/ucrt64/bin/gcc.exe')


def main():
    exe = os.path.join(tempfile.gettempdir(), 'test_fsm.exe')
    env = dict(os.environ, PATH=os.path.dirname(GCC) + os.pathsep + os.environ['PATH'])
    subprocess.run([GCC, '-O2', '-Wall', '-Wextra', '-Werror', '-I', SRC, '-o', exe, os.path.join(HERE, 'test_fsm.c'),
                    os.path.join(SRC, 'radio_fsm.c')], check=True, env=env)
    sys.exit(subprocess.run([exe], env=env).returncode)


if __name__ == '__main__':
    main()
