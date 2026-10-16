#!/usr/bin/env python3
"""Run one Project+ proof under the shared lock, using a disposable writable APFS clone."""
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time

root = Path(__file__).resolve().parents[3]
os.chdir(root)
case, attempt = sys.argv[1:]
if case not in ('1', '2'):
    raise SystemExit('choose case 1 or 2')
subprocess.run(['uptime'], check=True)
if os.getloadavg()[0] >= 14:
    raise SystemExit('Load must be below 14 before starting three peers (ceiling 18).')
controls = Path('notes/friendlies-0348-runs/controls')
target = Path(f'notes/friendlies-0348-runs/pplus-case-{case}-attempt-{attempt}')
target.mkdir(parents=True, exist_ok=False)
clone = root / 'build-orca-tests' / f'finish-0348-pplus-{case}-{attempt}'
clone.mkdir(exist_ok=False)
source = Path('/Volumes/YGWorktrees/OSCOO/pplus-3.2/run')
launcher = 'Project+ Netplay Launcher.dol'
process = None
try:
    subprocess.run(['cp', '-c', str(source / launcher), str(source / 'sd.raw'), str(clone)], check=True)
    for path in clone.iterdir():
        path.chmod(path.stat().st_mode | 0o200)
    prefix = 'pplus-rotation' if case == '2' else 'pplus-basic'
    args = ['python3', 'Tools/orca/doubles-rooms.py', '--run', '--local', '--doubles',
            '--roles', '0,1,2', '--expected', '3', '--seconds', '160',
            '--binary', 'build-orca-tests/Binaries/dolphin-emu-nogui',
            '--disc', '/Volumes/YGWorktrees/OSCOO/ROMs/Wii/Super Smash Bros. Brawl (USA, Canada) (Rev 2).wbfs',
            '--pplus', str(clone / launcher), '--probe', str(controls / 'one-stock.probe'),
            '--keep-logs', str(target), '--expect-fight', '3']
    if case == '2':
        args += ['--expect-fight', '5']
    for role in range(3):
        args += ['--input-role', f'{role}={controls / (prefix + "-role-" + str(role) + ".txt")}']
    (target / 'arguments.json').write_text(json.dumps(args, indent=2) + '\n')
    with (target / 'run.log').open('w') as log:
        process = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT)
        print(f'Own fixture PID: {process.pid}; evidence: {target}', flush=True)
        while process.poll() is None:
            if os.getloadavg()[0] >= 18:
                (target / 'load-stop.txt').write_text('Stopped own fixture at load >=18.\n')
                process.send_signal(signal.SIGINT)
                break
            time.sleep(2)
        result = process.wait(timeout=60)
    print(f'{target}: exit {result}', flush=True)
    raise SystemExit(result)
finally:
    if process is not None and process.poll() is None:
        process.send_signal(signal.SIGINT)
        process.wait(timeout=60)
    shutil.rmtree(clone)
