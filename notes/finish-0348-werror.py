#!/usr/bin/env python3
"""Compile the finish-0348 C++ change with its real flags and warnings as errors."""
import json
from pathlib import Path
import shlex
import subprocess

root = Path(__file__).resolve().parents[1]
build = root / 'build-orca-tests'
source = root / 'Source/UnitTests/Core/Orca/KeyframeTest.cpp'
entries = json.loads((build / 'compile_commands.json').read_text())
entry = next(item for item in entries if Path(item['file']) == source)
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
command[command.index('-o') + 1] = str(build / 'finish-0348-KeyframeTest.cpp.o')
command.append('-Werror')
subprocess.run(command, cwd=entry['directory'], check=True)
print('PASS: KeyframeTest.cpp (-Werror actual object compile)', flush=True)
