# pace-telemetry

Base check before edits:

```text
37a8093135 Orca: initialize the disc layout parser's locals (MSVC C4701 under /WX)
BASE-OK
```

Local branch `pace-telemetry` starts at
`37a809313553521803dac97e5e3f3a75d279328d`. No push, release or signing step.
The pre-existing `.scratch-doubles-script-0391/` is untouched.

`win` serialization and its existing golden tests are unchanged. `pace` uses the same running-time
window, with no match warm-up filter. Completed game-image swaps feed a fixed SPSC queue; host UI
redraws of the previous game image do not conceal stalls. The graphics path adds no lock or
per-frame allocation. The CPU thread computes percentiles and writes protocol lines. Scene names
are read only when their guest scene identity changes. A pause discards queued paused redraws and
breaks the interval anchor. New-boot state is reset independently of emulation.

Synchronous ShaderCache miss scopes cover GX/uber pipelines, EFB copy to RAM/VRAM and texture
reinterpret pipelines. Counted duration includes dependent shader generation/compilation and
pipeline creation, including failed attempts and cache bookkeeping. Async/background workers,
shared startup pipelines and other shader caches are not counted. This is host blocked time,
not a driver's internal compiler event count.

`pace` fields: `v` version; `k` kind; `s` latest presented scene code; `n` interval count;
`p50/p95/p99/max` interval ms; `h25/h33/h50` overlapping strict hitch counts; `rb` rollback
event buckets [1,2,3,4+]; `rm` deepest rollback in frames; `sc/sm` synchronous cache-miss build
count/blocked ms; `d` lost queue events or samples; `h` headless marker. `scene` fields:
`v/k`, `from/to` scene codes, `ms` last old to first new presentation, `h` headless marker.
See ORCA.md, “This machine's speed,” for bounds and measurement details.

Desktop parser checked read-only with `git show origin/claude/v2:...` in YouGame:

- `desktop/src/orca-policy.ts:1340`, `parseOrcaLine`: generic `orca ` handling at line 1375
  forwards the line as status, bounded to 300 characters. It does not interpret `k`.
- `web/src/lib/orca-perf.ts:117`, `parseOrcaPerfLine`: accepts `info`, `win`, `auto` and returns
  `null` for an unknown kind at line 169. New kinds are safe: they reach logs/status and are
  ignored by the existing perf consumer. No YouGame file was edited.

Build and prepared test commands use the required heavy pool and shared `build.lock`, nice 10,
two build workers. The prepared runtime script uses the GPU pool, `-p headless -v Null` and
`-C "Dolphin.DSP.Backend=No Audio Output"`, task-owned user directories (reused for warm boots) and
disabled local controller discovery. Runtime fixture inputs are project disc/card files; profile
settings disable SD writes. No engine instance or external-service request was started.

Validation stopped on 2026-10-26 under the task's hard blocker rule:

```text
heavy-slot: 0s, waiting for a heavy slot (load 20, free memory 55%, held: internal disk 24.7667 GB free (min 25), head: brawl-rollback-lead:staging-deploy-b4803211f, queue position 2)
```

- Mac configure and `cmake --build build-pace --target tests dolphin-nogui --parallel 2`:
  **exit 0**, including the final incremental build of the committed source at `f7b62e2c11`.
  Logs: `build-pace/evidence/configure.log`, `build.initial.log`, `build.final.log`.
- Nine new `OrcaPace` math/protocol tests compiled successfully. The focused run
  (`--gtest_filter=OrcaPace.*:OrcaPerf.*`) never acquired its heavy slot and **did not execute**.
  Its exact task-owned wrapper PID 81096 was verified and terminated; the command exited 143.
  The blocked-slot message is retained in `build-pace/units-focused.log`.
- Full `python3 -B Tools/orca/room-fix-tests.py build-pace/Binaries/Tests/tests`: **not run**.
- `keyCode`, runtime caps equality, RSBE01/PPLUS32/PPLUS321 cold/warm frames 0–600 comparisons,
  and the muted headless protocol samples: **not run**. No sample lines are presented as measured.
- Prepared `build-pace/runtime-gates.py` remains available for resume. It calculates the key
  fingerprint, checks all 24 caps and their final three words, compares 601 hashes per run for
  all three profiles against the existing 0.3.95 dev binary, and then collects three lines of
  each kind from a longer muted Null run. Its reference build records revision
  `60f3a9f249139b4874f2b74236e3e710177e7ec7` in `build-0395/Source/Core/Common/scmrev.h`.

No disk cleanup, lock bypass, build-directory deletion, push or release was attempted. Resume
with the required wrappers when the shared heavy-slot disk minimum is satisfied, then complete
the focused suite, full suite and runtime gates. Headless pacing samples must have `n:0`; `h:1`
scene samples describe submitted frames and cannot establish physical display pacing.
