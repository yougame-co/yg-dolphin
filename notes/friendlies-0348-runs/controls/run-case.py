#!/usr/bin/env python3
"""Reproduce a train-0348 proof case (one fixture under the shared lock)."""
import json
import os
from pathlib import Path
import subprocess
import sys
import time

root = Path(__file__).resolve().parents[3]
os.chdir(root)
controls = Path('notes/friendlies-0348-runs/controls')
case = sys.argv[1]
attempt = sys.argv[2] if len(sys.argv) > 2 else '1'
target = Path(f'notes/friendlies-0348-runs/case-{case}-attempt-{attempt}')
target.mkdir(parents=True, exist_ok=False)
roles = '0,1,2'
expected = 3
seconds = 100
extras = []
prefix = 'basic'
probe = 'one-fight.probe'
if case == '2':
    seconds = 130
    prefix = 'rotation'
    probe = 'one-stock.probe'
    extras = ['--expect-fight', '3', '--expect-fight', '5']
elif case == '3':
    roles = '0,1,2,3'
    expected = 4
    seconds = 150
    prefix = 'four'
    probe = 'four.probe'
    extras = ['--expect-fight', '3', '--expect-fight', '15']
elif case in ('4a', '4b', '5', '6', '7'):
    extras = ['--expect-fight', '3', '--events', str(controls / f'case-{case}-events.json')]
    if case in ('5', '6'):
        extras += ['--defer-role', '2']
    if case == '7':
        roles = '0,1,2,3'
        expected = 4
elif case == '4c':
    extras = ['--expect-fight', '3', '--blackout-role', '2=50-56', '--dump-stacks']
elif case == '8':
    expected = 2
    extras = ['--ordinary', '--refused-role', '2', '--expect-fight', '3']
else:
    raise SystemExit('choose 2, 3, 4a, 4b, 4c, 5, 6, 7 or 8')
args = ['python3', 'Tools/orca/doubles-rooms.py', '--run', '--local', '--doubles',
        '--roles', roles, '--expected', str(expected), '--seconds', str(seconds),
        '--binary', 'build-orca-tests/Binaries/dolphin-emu-nogui',
        '--disc', '/Volumes/YGWorktrees/OSCOO/ROMs/Wii/Super Smash Bros. Brawl (USA, Canada) (Rev 2).wbfs',
        '--probe', str(controls / probe), '--keep-logs', str(target)]
for role in roles.split(','):
    args += ['--input-role', f'{role}={controls / (prefix + "-role-" + role + ".txt")}']
args += extras
(target / 'arguments.json').write_text(json.dumps(args, indent=2) + '\n')
limit = 14 if len(roles.split(',')) <= 3 else 12
while os.getloadavg()[0] >= limit:
    print(f'Waiting for 1-minute load <{limit} to leave headroom below 18', flush=True)
    time.sleep(30)
subprocess.run(['uptime'], check=True)
while True:
    with (target / 'run.log').open('a') as log:
        result = subprocess.run(['flock', '/Users/luis/Code/brawl-dolphin/build.lock',
                                 'nice', '-n', '10', 'python3', str(controls / 'locked-command.py')] + args,
                                stdout=log, stderr=subprocess.STDOUT)
    if result.returncode != 75:
        break
    time.sleep(30)
print(f'{target}: exit {result.returncode}', flush=True)
raise SystemExit(result.returncode)
