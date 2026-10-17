#!/usr/bin/env python3
"""Compile this train's changed translation units with their real flags plus -Werror."""
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import shlex
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
build = root / 'build-orca-tests'
entries = json.loads((build / 'compile_commands.json').read_text())
changed = sys.argv[1:] or subprocess.check_output(
    ['git', 'diff', '--name-only', 'ce0a4d906a', '--', 'Source'], cwd=root, text=True).splitlines()
output = build / 'train-0350-werror'
output.mkdir(exist_ok=True)

def compile_one(relative):
    entry = next((item for item in entries if Path(item['file']) == root / relative), None)
    if entry is None:
        # The Mac build excludes Windows' native window implementation.
        if relative == 'Source/Core/DolphinNoGUI/PlatformWin32.cpp':
            print('PLATFORM: PlatformWin32.cpp excluded from the macOS compilation database', flush=True)
            return
        raise RuntimeError(f'No compilation command: {relative}')
    original = shlex.split(entry['command'])
    command = []
    index = 0
    while index < len(original):
        arg = original[index]
        if arg in ('-MF', '-MT', '-MQ'):
            index += 2
            continue
        if arg not in ('-MD', '-MMD'):
            command.append(arg)
        index += 1
    if Path(command[0]).name == 'ccache':
        command.pop(0)
    command[command.index('-o') + 1] = str(output / (relative.replace('/', '_') + '.o'))
    command.append('-Werror')
    result = subprocess.run(command, cwd=entry['directory'], capture_output=True, text=True)
    if result.stdout:
        print(result.stdout, end='', flush=True)
    if result.stderr:
        print(result.stderr, end='', flush=True)
    result.check_returncode()
    print(f'PASS: {relative} (-Werror object compile)', flush=True)

with ThreadPoolExecutor(max_workers=2) as pool:
    list(pool.map(compile_one, (path for path in changed if path.endswith(('.cpp', '.mm')))))
