#!/usr/bin/env python3
"""Reproduce the finish-0348 gates, one command per shared lock."""
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
os.chdir(root)
checks = [
    ('build', ['cmake', '--build', 'build-orca-tests', '--target', 'tests',
               'dolphin-emu-nogui', '--parallel', '2']),
    ('werror', ['python3', 'notes/finish-0348-werror.py']),
    ('fixture-regressions', ['python3', '-Werror', 'Tools/orca/test_doubles_rooms.py']),
    ('doubles-selftest', ['python3', '-Werror', 'Tools/orca/doubles-rooms.py', '--doubles', '--self-test']),
    ('full-tests', ['python3', 'Tools/orca/room-fix-tests.py', 'python3',
                    'notes/train-0348-tests.py', '--gtest_output=xml:notes/finish-0348-full-tests.xml']),
]
environment = {'PATH': os.environ.get('PATH', ''), 'CCACHE_DISABLE': '1'}
for name, command in checks:
    subprocess.run(['uptime'], check=True)
    with Path(f'notes/finish-0348-{name}.log').open('w') as log:
        result = subprocess.run(
            ['flock', '/Users/luis/Code/brawl-dolphin/build.lock', 'nice', '-n', '10',
             'python3', 'notes/friendlies-0348-runs/controls/locked-command.py'] + command,
            stdout=log, stderr=subprocess.STDOUT, env=environment)
    print(f'{name}: exit {result.returncode}', flush=True)
    if result.returncode:
        raise SystemExit(result.returncode)
