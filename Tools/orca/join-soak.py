#!/usr/bin/env python3
"""Twenty consecutive PPLUS321 joins; invoke through run-lock.py perf.

Each join uses a new isolated pair, with ten aged menus, five fights released by let-in,
and five near-origin menus. Validate both-live, native checksums, fresh starts and fight holds.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('binary')
    p.add_argument('disc')
    p.add_argument('--pplus', required=True)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=False)
    plan = ['C', 'C', 'EL', 'A'] * 5
    command = [sys.executable, str(Path(__file__).with_name('join-timing.py')),
               a.binary, a.disc, '--pplus', a.pplus, '--out', str(a.out),
               '--origin', '229', '--plan', ','.join(plan)]
    rc = subprocess.run(command).returncode
    runs = [json.loads(p.read_text()) for p in sorted(a.out.glob('*/result.json'))]
    fresh = sum(bool(r.get('phases', {}).get('milestones', {}).get('fresh_restore_end'))
                for r in runs)
    held = sum(bool(r.get('phases', {}).get('milestones', {}).get('fight_hold'))
               for r in runs)
    passed = (rc == 0 and len(runs) == 20 and fresh >= 10 and held >= 3 and
              all(r['outcome'] == 'PASS' and not r['error_lines'] and
                  r['phases'].get('both_live_ms') is not None for r in runs))
    summary = {'outcome': 'PASS' if passed else 'FAIL', 'runs': len(runs),
               'fresh_starts': fresh, 'fight_holds': held, 'command_exit': rc,
               'plan': plan, 'results': [r['work'] for r in runs]}
    (a.out / 'soak-summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    print(json.dumps(summary, indent=2))
    return 0 if passed else 1


if __name__ == '__main__':
    sys.exit(main())
