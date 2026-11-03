#!/usr/bin/env python3
"""Measure friend joins with an existing Orca binary; never modifies engine code.

Run through run-lock.py perf. All users, HOME, temporary files and evidence live in --out.
One plan entry is a fresh pair. A/AL: near-origin (running/launch join); B: 300 frames;
C/D: 18000/108000 frames; E: CPU fight at 18000; GC/GD: C/D with fresh starts off.
"""
import argparse
import datetime
import json
import os
from pathlib import Path
import random
import re
import shlex
import subprocess
import threading
import time


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('binary')
    p.add_argument('disc')
    p.add_argument('--pplus')
    p.add_argument('--profile', default='PPLUS321')
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--plan', default='A,A,A,AL,B,C,C,D,E,GC,GD')
    p.add_argument('--origin', type=int, default=230)
    p.add_argument('--timeout', type=float, default=300)
    p.add_argument('--fast-offer-off', action='store_true')
    p.add_argument('--fight-hold-off', action='store_true')
    a = p.parse_args()
    a.out = a.out.resolve()
    a.out.mkdir(parents=True, exist_ok=True)
    (a.out / ('invocation-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S') + '.json')).write_text(json.dumps({'argv': __import__('sys').argv,
        'shell': shlex.join(__import__('sys').argv), 'utc': utc()}, indent=2))
    origin = a.origin
    failures = 0
    for scenario in a.plan.split(','):
        while os.getloadavg()[0] >= 20:
            print(f'Waiting for load <20: {os.getloadavg()[0]:.2f}', flush=True)
            time.sleep(10)
        stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
        work = a.out / f'{stamp}-{scenario}'
        work.mkdir()
        result = {'scenario': scenario, 'work': str(work), 'utc': utc(),
                  'load_start': os.getloadavg(), 'origin_assumed': origin}
        print(f'START {scenario} {work.name} load={result["load_start"][0]:.2f}', flush=True)
        events = []
        lock = threading.Lock()
        log = (work / 'events.jsonl').open('w', buffering=1)
        t0 = time.monotonic_ns()

        def record(who, stream, line):
            e = {'ms': (time.monotonic_ns()-t0)/1e6, 'utc': utc(),
                 'who': who, 'stream': stream, 'line': line}
            with lock:
                events.append(e)
                log.write(json.dumps(e)+'\n')
            return e

        pair = []
        try:
            room = 'jt' + ''.join(random.choices('abcdefghijklmnopqrstuvwxyz0123456789', k=8))
            off = scenario.startswith('G')
            target = {'B': 300, 'C': 18000, 'D': 108000, 'E': 18000, 'EL': 18000,
                      'GC': 18000, 'GD': 108000}.get(scenario, 0)
            friend = None
            if scenario != 'AL':
                friend = Orca('Friend', work, a, {}, '', record, events)
                pair.append(friend)
                friend.wait('Drop-in: origin at frame', a.timeout)
                friend.wait('orca state playing', a.timeout)
            extra = {'ORCA_ROOM': room}
            if a.fast_offer_off:
                extra['ORCA_TEST_JOIN_FAST_OFFER'] = 'off'
            if a.fight_hold_off:
                extra['ORCA_TEST_FIGHT_HOLD'] = 'off'
            if off:
                extra['ORCA_TEST_FRESH'] = 'off'
            if target >= 18000:
                extra['ORCA_TEST_FAST_UNTIL'] = str(origin + target)
            inp = ''
            if scenario in ('E', 'EL'):
                # Age on the main menu, then enter the first CPU fight near five minutes.
                # This avoids repeated-match fuzz/JIT history contaminating a join measurement.
                lines = (Path(__file__).resolve().parent / 'inputs/pplus-cpu-loop.txt').read_text().splitlines()
                presses = []
                for line in lines:
                    if line.startswith(('@scMelee', '@scVsResult')):
                        continue
                    if line.startswith('@muMenuMain:1 '):
                        parts = line.split()
                        parts[1:3] = [str(int(v) + 17400) for v in parts[1:3]]
                        line = ' '.join(parts)
                    presses.append(line)
                # Let the first fight finish through ordinary controller inputs, then leave results.
                presses += ['@scMelee 600 99999 1 SX=0',
                            '@scVsResult mash 240 99999 1 40 A',
                            '@scVsResult mash 600 99999 1 40 START']
                inp = '\n'.join(presses) + '\n'
                watch = work / 'cpu-watch.txt'
                watch.write_text('@* 0 watch [[[[805a0060]+4]+400]+48]+1b4 a2-kind\n')
                extra['ORCA_UX_PROBE'] = str(watch)
            host = Orca('Host', work, a, extra, inp, record, events)
            pair.append(host)
            h_origin = host.wait('Drop-in: origin at frame', a.timeout)
            origin = int(re.search(r'origin at frame (\d+)', h_origin['line'])[1])
            result['origin'] = origin
            result['target_history_frames'] = target
            if target >= 18000:
                host.wait('frames run at their own pace', 900)
            elif target:
                host.wait_frame(origin + target, a.timeout)
            if scenario in ('E', 'EL'):
                host.wait_current_scene('scMelee', a.timeout)
            result['load_accept'] = os.getloadavg()
            result['host_last_stats_at_accept'] = host.stats
            result['host_scene_at_accept'] = host.scene
            accept = record('Driver', 'mark', 'Accept launch' if scenario == 'AL' else 'Accept join')
            result['accept_ms'] = accept['ms']
            if scenario == 'AL':
                friend = Orca('Friend', work, a, {'ORCA_ROOM': room, 'ORCA_JOIN': '1'}, '', record, events)
                pair.append(friend)
            else:
                friend.send(f'join {room}')
            if scenario == 'EL':
                host.wait('the join waits for this match to end', 30, after=accept['ms'])
                friend.wait('orca state friend-holding', 30, after=accept['ms'])
                record('Driver', 'mark', 'Fight hold observed; let-in')
                host.send('let-in')
            friend.wait('Drop-in: plugged into port', 900 if scenario == 'E' else a.timeout,
                        after=accept['ms'])
            host.wait('orca state friend-joined', 60, after=accept['ms'])
            record('Driver', 'mark', 'Both live observed')
            # Frozen binary exposes cumulative checksum matches on session end, not each exchange.
            time.sleep(4)
            friend.send('leave')
            for o in pair:
                o.wait('Drop-in: back to solo play at frame', 45, after=accept['ms'])
            result['outcome'] = 'PASS'
        except Exception as ex:
            result['outcome'] = 'FAIL'
            result['reason'] = str(ex)
            print(f'FAIL {scenario}: {ex}', flush=True)
        finally:
            for o in reversed(pair):
                o.stop()
            bad = [e for e in events if re.search(r'Desync|orca error', e['line'])]
            result['error_lines'] = bad
            if bad:
                result['outcome'] = 'FAIL'
            counts = {}
            for o in pair:
                ends = [re.search(r'(\d+) checksums matched', e['line']) for e in events
                        if e['who'] == o.name and 'back to solo play' in e['line']]
                counts[o.name] = [int(m[1]) for m in ends if m]
            result['checksum_counts'] = counts
            if result['outcome'] == 'PASS' and (len(counts) != 2 or
                    any(not n or min(n) <= 0 for n in counts.values())):
                result['outcome'] = 'FAIL'
                result['reason'] = 'Missing positive session checksum count'
            result['load_end'] = os.getloadavg()
            result['phases'] = phases(result, events)
            milestones = result['phases'].get('milestones', {})
            if scenario == 'E' and milestones.get('fight_hold'):
                end = milestones.get('results_end')
                fresh = milestones.get('fresh_restore_end')
                scheduled = next((e for e in events if e['who'] == 'Host' and
                                  e['ms'] >= result['accept_ms'] and
                                  'fresh start at the next boundary' in e['line']), None)
                result['fight_finished_before_fresh'] = bool(
                    end and fresh and scheduled and end['ms'] < scheduled['ms'] < fresh['ms'])
                if not result['fight_finished_before_fresh']:
                    result.update(outcome='FAIL', reason='Fight/results did not finish before fresh start')
            log.close()
            (work / 'result.json').write_text(json.dumps(result, indent=2)+'\n')
            print(f'END {scenario} {result["outcome"]} checksums={counts}', flush=True)
            failures += result['outcome'] != 'PASS'
    return int(bool(failures))


def phases(result, events):
    """Preserve measured milestones and durations; missing instrumentation stays null."""
    accept = result.get('accept_ms')
    if accept is None:
        return {}
    def first(who, text, after=accept):
        return next((e for e in events if e['who'] == who and e['ms'] >= after
                     and text in e['line']), None)
    def duration(start, end):
        return round(end['ms'] - start['ms'], 3) if start and end else None
    launch = first('Friend', 'Process launch', 0)
    origin = first('Friend', 'Drop-in: origin at frame', 0)
    welcome = first('Friend', 'Orca room: in room')
    arrival = first('Host', 'arrived for port')
    fresh = first('Host', 'Drop-in: fresh start at frame')
    tail = first('Host', "the fresh start's tail ran frames")
    packed = first('Host', 'Drop-in: keyframe of frame')
    sent = first('Host', 'replays from keyframe frame')
    received = first('Friend', 'the host offers keyframe')
    rebuilt = first('Friend', 'rebuilt replay frame')
    plugged = first('Friend', 'Drop-in: plugged into port')
    host_live = first('Host', 'orca state friend-joined')
    hold = first('Host', 'the join waits for this match to end')
    fight_end = first('Host', 'scMelee ->')
    results_end = first('Host', 'scVsResult ->')
    marks = {'fight_hold': hold, 'fight_end': fight_end, 'results_end': results_end,
             'friend_launch': launch, 'friend_origin': origin, 'room_welcome': welcome,
             'host_arrival_hello_complete': arrival, 'fresh_restore_end': fresh,
             'fresh_tail_end': tail, 'offer_packed': packed, 'offer_sent': sent,
             'offer_received': received, 'rebuild_checksum_K_match': rebuilt,
             'friend_live': plugged, 'host_live': host_live}
    out = {'milestones': marks, 'accept_to_welcome_ms':
           round(welcome['ms']-accept, 3) if welcome else None,
           'accept_to_host_hello_ms': round(arrival['ms']-accept, 3) if arrival else None,
           'friend_process_to_origin_ms': duration(launch, origin),
           'host_hello_to_offer_ms': duration(arrival, sent),
           'offer_wire_ms': duration(sent, received),
           'rebuild_ms': None, 'rebuild_frames': None, 'rebuild_fps': None,
           'receive_to_rebuild_start_ms': None, 'fresh_restore_ms': None,
           'host_picture_hold_restore_plus_tail_ms': None, 'fresh_tail_only_ms': None,
           'download_done_ms': None, 'first_live_checksum_exchange_ms': None,
           'fight_hold_to_release_ms': duration(hold, first('Host', 'the join goes on')),
           'fight_end_to_both_live_ms':
               round(max(plugged['ms'], host_live['ms'])-fight_end['ms'], 3)
               if fight_end and plugged and host_live else None,
           'tail_end_to_offer_ms': duration(tail, sent),
           'both_live_ms': round(max(plugged['ms'], host_live['ms'])-accept, 3)
               if plugged and host_live else None}
    if rebuilt:
        m = re.search(r'frame (\d+) locally in (\d+) ms', rebuilt['line'])
        if m:
            n, ms = int(m[1])-result['origin'], int(m[2])
            out.update(rebuild_ms=ms, rebuild_frames=n,
                       rebuild_fps=round(n*1000/ms, 2) if ms else None,
                       receive_to_rebuild_start_ms=
                           round(rebuilt['ms']-ms-received['ms'], 3) if received else None,
                       rebuild_to_friend_live_ms=duration(rebuilt, plugged))
    if fresh:
        m = re.search(r'in (\d+) ms', fresh['line'])
        out['fresh_restore_ms'] = int(m[1]) if m else None
    if tail:
        m = re.search(r'in (\d+) ms', tail['line'])
        out['host_picture_hold_restore_plus_tail_ms'] = int(m[1]) if m else None
        if m and out['fresh_restore_ms'] is not None:
            out['fresh_tail_only_ms'] = int(m[1])-out['fresh_restore_ms']
    if packed:
        m = re.search(r'(\d+) bytes raw, (\d+) bytes (.*); capture ([\d.]+) ms, '
                      r'pack and encrypt ([\d.]+) ms, store ([\d.]+) ms', packed['line'])
        if m:
            out.update(raw_bytes=int(m[1]), packed_bytes=int(m[2]), transport=m[3],
                       capture_ms=float(m[4]), pack_encrypt_ms=float(m[5]), store_ms=float(m[6]))
    return out


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='milliseconds')


class Orca:
    def __init__(self, name, work, a, extra, inp, record, events):
        self.name, self.record, self.events = name, record, events
        self.stats = None
        self.scene = None
        user = work / ('user-' + name)
        user.mkdir()
        home = work / ('home-' + name)
        home.mkdir()
        tmp = work / ('tmp-' + name)
        tmp.mkdir()
        script = work / (name + '.txt')
        script.write_text(inp or '# no input\n')
        environment = {'HOME': str(home), 'TMPDIR': str(tmp), 'PATH': '/usr/bin:/bin',
            'ORCA_SESSION': '1', 'ORCA_TEST_COMMANDS': '1', 'ORCA_TEST_DEV_GAME': 'orca-dropin-test',
            'ORCA_NAME': name,
            'ORCA_PROFILE': a.profile if a.pplus else 'RSBE01', 'YG_INPUT': str(script), 'YG_SCENES': '1',
            'YG_THROTTLE': '1', 'SDL_JOYSTICK_HIDAPI': '0', 'SDL_HIDAPI_LIBUSB': '0'}
        environment.update(extra)
        args = ['nice', '-n', '10', a.binary, '-p', 'headless', '-u', str(user), '-v', 'Null',
            '-C', 'Dolphin.DSP.Backend=No Audio Output',
            '-C', 'Logger.Logs.ROLLBACK=True', '-C', 'Logger.Logs.NETPLAY=True',
            '-C', 'Logger.Logs.CORE=True', '-C', 'Logger.Options.Verbosity=2',
            '-C', 'Logger.Options.WriteToConsole=True',
            *(['-C', f'Dolphin.Core.DefaultISO={a.disc}', '-e', a.pplus]
              if a.pplus else ['-e', a.disc])]
        (work / (name + '-command.json')).write_text(json.dumps({'argv': args,
            'environment': environment}, indent=2))
        while os.getloadavg()[0] >= 20:
            time.sleep(5)
        self.record(name, 'mark', 'Load before launch ' + str(os.getloadavg()))
        self.record(name, 'mark', 'Process launch')
        self.proc = subprocess.Popen(args, env=environment, cwd=work, stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
        self.threads = []
        for stream in ('stdout', 'stderr'):
            t = threading.Thread(target=self.read, args=(stream,), daemon=True)
            t.start()
            self.threads.append(t)

    def read(self, stream):
        for line in getattr(self.proc, stream):
            line = line.rstrip('\n')
            self.record(self.name, stream, line)
            if line.startswith('orca caps'):
                self.send('caps join leave stats')
            if line.startswith('orca stats '):
                self.stats = json.loads(line[11:])
            m = re.search(r'Scene frame \d+:.* -> (\w+)', line)
            if m:
                self.scene = m[1]

    def send(self, line):
        self.record(self.name, 'stdin', line)
        self.proc.stdin.write(line+'\n')
        self.proc.stdin.flush()

    def wait(self, text, timeout, after=0):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            found = next((e for e in self.events if e['who'] == self.name and
                          e['ms'] >= after and text in e['line']), None)
            if found:
                return found
            if self.proc.poll() is not None:
                raise RuntimeError(f'{self.name} exited {self.proc.returncode} waiting for {text}')
            err = next((e for e in self.events if e['who'] == self.name and
                        e['ms'] >= after and e['line'].startswith('orca error')), None)
            if err:
                raise RuntimeError(err['line'])
            time.sleep(.005)
        raise RuntimeError(f'{self.name} timed out waiting for {text}')

    def wait_frame(self, frame, timeout):
        end = time.monotonic() + timeout
        while not self.stats or self.stats['f'] < frame:
            if time.monotonic() > end or self.proc.poll() is not None:
                raise RuntimeError(f'{self.name} did not reach frame {frame}')
            time.sleep(.01)

    def wait_current_scene(self, scene, timeout):
        end = time.monotonic() + timeout
        while self.scene != scene:
            if time.monotonic() > end or self.proc.poll() is not None:
                raise RuntimeError(f'{self.name} did not reach scene {scene}, last={self.scene}')
            time.sleep(.01)

    def stop(self):
        if self.proc.poll() is None:
            try:
                self.send('quit')
                self.proc.wait(timeout=20)
            except (OSError, subprocess.TimeoutExpired):
                self.proc.kill()
                self.proc.wait(timeout=10)
        for t in self.threads:
            t.join(timeout=2)


if __name__ == '__main__':
    raise SystemExit(main())
