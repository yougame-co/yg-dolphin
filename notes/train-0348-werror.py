#!/usr/bin/env python3
"""Compile each touched C++ translation unit with its real build flags plus -Werror."""
import json
from pathlib import Path
import shlex
import subprocess

root = Path(__file__).resolve().parents[1]
build = root / 'build-orca-tests'
entries = json.loads((build / 'compile_commands.json').read_text())
output = build / 'werror'
output.mkdir(exist_ok=True)
touched = (
    'Source/UnitTests/Core/Orca/KeyframeTest.cpp',
    'Source/Core/Core/Orca/Games/Brawl/OnlineSeats.cpp',
    'Source/UnitTests/Core/Orca/Games/Brawl/OnlineSeatsTest.cpp',
    'Source/UnitTests/Core/Orca/ModTest.cpp',
)
for relative in touched:
    entry = next(item for item in entries if Path(item['file']) == root / relative)
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
    command[command.index('-o') + 1] = str(output / (Path(relative).name + '.o'))
    command.append('-Werror')
    if Path(command[0]).name != 'ccache':
        command.insert(0, 'ccache')
    subprocess.run(command, cwd=entry['directory'], check=True)
    print(f'PASS: {relative} (-Werror object compile)', flush=True)
print(f'PASS: {len(touched)} translation units; touched headers checked through their includes.')
