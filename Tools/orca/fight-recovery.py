#!/usr/bin/env python3
"""PPLUS321 mid-fight link blackout and in-place friend recovery; use run-lock.py check.

Uses the existing ORCA_TEST_BLACKOUT knob for both relay and direct traffic. No process is
suspended, no game state is edited, and every writable path lives below --out.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import random
import re
import threading
import time


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('binary')
    p.add_argument('disc')
    p.add_argument('--pplus', required=True)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    a.profile = 'PPLUS321'
    a.out = a.out.resolve()
    a.out.mkdir(parents=True, exist_ok=False)
    spec = importlib.util.spec_from_file_location('join_timing', Path(__file__).with_name('join-timing.py'))
    timing = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(timing)
    events, pair, lock = [], [], threading.Lock()
    t0 = time.monotonic_ns()
    result = {'outcome': 'FAIL', 'load_start': os.getloadavg()}
    with (a.out / 'events.jsonl').open('w', buffering=1) as log:
        def record(who, stream, line):
            event = {'ms': (time.monotonic_ns()-t0)/1e6, 'utc': timing.utc(),
                     'who': who, 'stream': stream, 'line': line}
            with lock:
                events.append(event)
                log.write(json.dumps(event)+'\n')
            return event
        try:
            room = 'fr' + ''.join(random.choices('abcdefghijklmnopqrstuvwxyz0123456789', k=8))
            # Same ordinary controller script as dropin-commands, with picks at 15 s instead
            # of 100 s. Remain in the first fight without fuzz/results inputs.
            lines = (Path(__file__).parent/'inputs/pplus-dropin.txt').read_text().splitlines()
            script = []
            for line in lines:
                if not line.startswith('@') or line.startswith(('@scMelee', '@scVsResult')):
                    continue
                if line.startswith('@scSelctCharacter ') and 'mash' not in line:
                    words = line.split()
                    words[1:3] = [str(int(v)-5100) for v in words[1:3]]
                    line = ' '.join(words)
                script.append(line)
            inp = '\n'.join(script)+'\n'
            host = timing.Orca('Host', a.out, a, {'ORCA_ROOM': room,
                'ORCA_TEST_BLACKOUT': '1000000-1000001'}, inp, record, events)
            pair.append(host)
            host.wait('Drop-in: origin at frame', 120)
            friend = timing.Orca('Friend', a.out, a, {'ORCA_TEST_BLACKOUT': '45-65'},
                                 inp, record, events)
            pair.append(friend)
            friend.wait('Drop-in: origin at frame', 120)
            friend.send('join '+room)
            friend.wait('Drop-in: plugged into port', 120)
            host.wait('orca state friend-joined', 60)
            for peer in pair:
                peer.wait_current_scene('scMelee', 120)
            fight = record('Driver', 'mark', 'Both in first fight')
            drop = friend.wait('close-reason=test-blackout', 90, after=fight['ms'])
            assert host.scene == friend.scene == 'scMelee', 'Blackout must happen in the fight'
            back = friend.wait('orca state room-back self', 120, after=drop['ms'])
            time.sleep(2)
            solo = next((e for e in events if e['who'] == 'Host' and e['ms'] >= drop['ms']
                         and 'Drop-in: back to solo play' in e['line']), None)
            if solo:
                recovered = friend.wait('Drop-in: plugged into port', 150, after=solo['ms'])
                host.wait('orca state friend-joined', 60, after=solo['ms'])
            else:
                # Recovery may preserve the live session; distinguish this from the stricter
                # idle-host/recovering-arrival path instead of calling that regression proven.
                recovered = back
                assert host.stats['peers'] and friend.stats['peers'], 'Session did not resume'

            assert host.scene == friend.scene == 'scMelee', 'Recovery must finish in the same fight'
            during = [e for e in events if drop['ms'] <= e['ms'] <= recovered['ms']]
            forbidden = ('fresh start at', 'friend-holding', 'friend-waiting',
                         'the join waits for this match', '-> scVsResult')
            assert not any(any(s in e['line'] for s in forbidden) for e in during), 'Hold/reset/results during recovery'
            time.sleep(4)
            friend.send('leave')
            for peer in pair:
                peer.wait('Drop-in: back to solo play', 45, after=recovered['ms'])
            result.update(outcome='PASS' if solo else 'SESSION_RETAINED',
                          exact_recovery_verified=bool(solo), fight_ms=fight['ms'],
                          blackout_ms=drop['ms'], solo_ms=solo['ms'] if solo else None,
                          recovered_ms=recovered['ms'])
        except Exception as ex:
            result['reason'] = str(ex)
        finally:
            for peer in reversed(pair):
                peer.stop()
            result['error_lines'] = [e for e in events if re.search(r'Desync|orca error', e['line'])]
            result['checksums'] = {peer.name: [int(m[1]) for e in events
                if e['who'] == peer.name and (m := re.search(r'(\d+) checksums matched', e['line']))]
                for peer in pair}
            if result['error_lines'] or any(not v or max(v) <= 0 for v in result['checksums'].values()):
                result['outcome'] = 'FAIL'
            result['load_end'] = os.getloadavg()
    (a.out/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))
    return 0 if result['outcome'] in ('PASS', 'SESSION_RETAINED') else 1


if __name__ == '__main__':
    raise SystemExit(main())
