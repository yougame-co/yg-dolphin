#!/usr/bin/env python3
"""Read-only evaluator source check and replay of captured gtest logger output."""
import argparse
from functools import lru_cache
import importlib.util
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
RULE = Path('/Users/luis/Code/brawl-dolphin/notes/lead-records/orca-split-2026-10-12/staging-03100/gates/rule-0.3.101')
sys.path.insert(0, str(RULE))
spec = importlib.util.spec_from_file_location('gate_evaluator', RULE / 'evaluate.py')
evaluator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(evaluator)

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=Path)
parser.add_argument('--candidate', default='HEAD')
args = parser.parse_args()

@lru_cache(maxsize=None)
def source(commit, path):
    if commit == 'WORKTREE':
        return Path(path).read_text()
    result = subprocess.run(['git', 'show', f'{commit}:{path}'], capture_output=True)
    evaluator.require(result.returncode == 0, 'engine source unavailable at commit: ' + path)
    return result.stdout.decode()

base = evaluator.source_hashes('e7b886d', source)
candidate = evaluator.source_hashes(args.candidate, source, raw=True)
paths = {region[1] for region in evaluator.SOURCE_REGIONS}
cleaned = {(commit, path): evaluator.strip_emits(source(commit, path))
           for commit in ('e7b886d', args.candidate) for path in paths}
masks = {key: evaluator.cpp_mask(value) for key, value in cleaned.items()}
for region, path, kind, name, discriminator in evaluator.SOURCE_REGIONS:
    if kind == 'raw_emitter':
        continue
    before = evaluator.extract(cleaned['e7b886d', path], kind, name, discriminator,
                               _mask=masks['e7b886d', path])
    after = evaluator.extract(cleaned[args.candidate, path], kind, name, discriminator,
                              _mask=masks[args.candidate, path])
    assert before == after and base[region] == candidate[region], region
    print(f'PASS {region}: byte-identical after strip_emits')
sources = evaluator.pinned_sources(args.candidate, source, raw=True)
evaluator.emitter_guard(sources, required=True)
region = evaluator.extract(sources, 'raw_emitter', evaluator.RAW_EMIT_MACRO, evaluator.RAW_EMITTER_FUNCTION)
start, end = evaluator.emitter_definition(sources[evaluator.GATE_RAW])
assert region == sources[evaluator.GATE_RAW][start:end].encode() + b'\n' + evaluator.extract(
    sources[evaluator.SESSION], 'function', evaluator.RAW_EMITTER_FUNCTION)
print('PASS raw_emitter: found once; GateRaw.h define + Session.cpp EmitRaw')
print('PASS guard: one plain define, no undef, no conditional define, no GateRaw.h cpp include')
print('PASS strip_emits: no refused lines in any source file')
if args.logs:
    text = args.logs.read_text()
    # Unit-test markers delimit independent captured sessions. Times retain the
    # logger envelope; gtest output itself is not part of the replay.
    chunks = text.split('GATE_RAW_CAPTURE_BEGIN\n')[1:]
    assert chunks, 'no captured unit-test log'
    saw_tail = False
    for index, chunk in enumerate(chunks):
        log = chunk.split('GATE_RAW_CAPTURE_END', 1)[0]
        # Use the evaluator's unchanged summary reader, with a fixed local hour.
        fixture = Path('build') / f'gate-raw-capture-{index}.log'
        fixture.write_text(log)
        start = evaluator.windows.time.mktime(
            evaluator.windows.time.strptime('2026-11-03 00:00:00', '%Y-%m-%d %H:%M:%S'))
        parsed = [counts for _, counts in evaluator.windows.seat_lines(fixture, start)]
        assert parsed, 'no captured summaries'
        tail = {}
        raw, windows = evaluator.reconcile(log, parsed, tail=tail)
        assert raw
        saw_tail |= any(tail.values())
        print(f'PASS capture {index}: {len(windows)} windows reconciled: {windows}; tail={tail}')
    assert saw_tail, 'no capture ends with a validated tail after the final summary'
    print('PASS session tail: captured teardown after final summary, within 1800 frames')
