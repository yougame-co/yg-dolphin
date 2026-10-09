# Orca engineering notes (private)

The full internal log behind ORCA.md: dated checks, measurements, investigations and reverse-engineering
detail. This file stays in the private repo; `Tools/orca-publish-public.sh` leaves it out of public
snapshots. Keep personal information out of ORCA.md and the code; it may appear here.

# Orca

Orca is YouGame's GameCube and Wii rollback netcode core: a fork of the
[Dolphin](https://github.com/dolphin-emu/dolphin) emulator (GPL-2.0-or-later). Code we add may be
GPL-3.0-or-later, so Orca builds are distributed under GPLv3. Orca is not affiliated with or endorsed
by the Dolphin project. Builds do not use Dolphin's name or logo.

- **Base:** Dolphin release `2609` (`f84df02`), tracked as branch `main` here and rebased onto later
  releases deliberately. The `upstream` remote is `dolphin-emu/dolphin`.
- **Design:**
  - an exact whole-machine snapshot at one frame boundary;
  - controller input injected at the SI device;
  - re-run frames that parse the GPU command stream but skip host rendering;
  - per-game profiles, with Super Smash Bros. Brawl (`RSBE01` rev 2) first.
- **Plan, decisions and results:** `docs/BRAWL_ROLLBACK.md` in YouGame's main repository.

No disc images, NAND dumps or save data belong in this repository.

## Release checklist

Every Orca release candidate (a `rel-0.3.x` branch) passes these on the Mac before it ships; its
dated **Checked** entry (for 0.3.14 and 0.3.15, at the end of "The queue's character select") gives
the numbers. Runs are headless (`-v Null`), muted (`No Audio Output`), nice'd, and go through the
Mac's run queue (`Tools/orca/run-lock.py check -- <command>`, `perf` for anything timed: "Sharing the
Mac: the run queue" below), which waits while Luis plays and while the machine is busy. Luis plays
when `ps -axo comm=` lists his app's Orca (ORCA.md checklist step 0; `run-lock.py status` prints
`Luis's Orca: open`); anything that starts Orca outside the queue checks the same first.

1. **Build** `dolphin-emu-nogui` and `tests` (Release, the release's CMake flags): no new warnings.
2. **Unit tests:** `Binaries/Tests/tests --gtest_filter='Orca*'`, none failing (the live, network
   and disc tests skip without their switches; `ORCA_LIVE_TEST=1` runs `OrcaLive.*` against
   yougame.co).
3. **Hashlogs** (`YG_HASHLOG=<out>`) of the default inputs equal to the live release's, frame by
   frame over the frames both logs have: Brawl `Tools/orca/inputs/bf-mario-link-results.txt` with
   `YG_EXIT_AFTER=10800`, Project+ `pplus-1v1.txt` with `YG_EXIT_AFTER=9000` (for 0.3.14:
   `notes/r314/mac-brawl.hash` and `mac-pplus.hash` beside the checkouts). A release that changes
   emulation on purpose says which frames move and why.
4. **Synctests** (`YG_SYNCTEST=7`), 0 RAM mismatches: the Brawl and the Project+ casual whole flows,
   with the env of Orca 0.3.14's checks ("The queue's character select").
5. **Two-Orca checks.** Luis, 2026-10-04, asked why two-machine bugs slip through: a synctest runs
   one game against itself, so a race between two peers' messages (the queue room's header, 0.3.15)
   or two games drifting apart never shows there. Two Orcas on this Mac go through the real flows
   over yougame.co (dev tickets, nothing recorded), for Project+ and for Brawl, and each run must
   PASS with **every session's checksums matched**: both Orcas' "Drop-in: back to solo play at frame
   N (why): R rollbacks, K checksums matched" lines with K > 0 (both tools print them on the PASS
   line), no `Desync at frame`, no failed session, no `orca error`:

   ```
   BIN=build/Binaries/dolphin-emu-nogui
   DISC=/path/to/your/brawl-rev2.wbfs   # your own Brawl (USA, Rev 2) disc image
   DOL=~/Documents/brawl-dolphin/pplus/run/"Project+ Netplay Launcher.dol"
   # Project+: queue2 casual, 2 games in the matchmade room; skip, then the quick rematch and a
   # game in the new room; a friend dropping in on the main menu's ONLINE page
   node Tools/orca/queue-e2e.mjs "$BIN" "$DISC" --pplus "$DOL" --queue casual --q2
   node Tools/orca/queue-e2e.mjs "$BIN" "$DISC" --pplus "$DOL" --queue casual --q2 --scenario rematch
   python3 Tools/orca/online-menu-dropin.py "$BIN" "$DISC" --pplus "$DOL"
   # Brawl: the same three
   node Tools/orca/queue-e2e.mjs "$BIN" "$DISC" --queue casual --q2
   node Tools/orca/queue-e2e.mjs "$BIN" "$DISC" --queue casual --q2 --scenario rematch
   python3 Tools/orca/online-menu-dropin.py "$BIN" "$DISC"
   ```

   One to three minutes each. `queue-e2e.mjs --q2` also checks that both Orcas print the same verdicts
   and come back on their own character select ready; `online-menu-dropin.py` that both games
   reach With Friends' character select at the same frame and nothing prints `orca menu`.
6. **Mac <-> PC**, by the release lead with the PC: the same kind of match with one Orca on each
   machine, `Tools/orca/xplat.py` (host on one, `--role join` on the other, the same `--room`, a
   sandbox player's cookie on each; `--pplus "<dol>"` for Project+), for Brawl and for Project+, each
   side's `XPLAT` summary `"ok": true` with `checksums_matched` above 0. Two Orcas on one Mac share
   a compiler, an architecture and a JIT, so only this run can see the two builds disagree
   (**Host arithmetic** under "Drop-in": Apple clang's fused multiply-adds against MSVC's x64).
   The Mac side runs under `Tools/orca/run-lock.py perf`, with Project+ from the internal disk
   (`--pplus ~/Documents/brawl-dolphin/pplus-internal/run/"Project+ Netplay Launcher.dol"`; "The
   external SSD" below).

## Sharing the Mac: the run queue

2026-10-06. Every scripted Orca run on the Mac took `orca-run-solo.lock` (two-Orca runs
`orca-run.lock` too) exclusively, so runs went one at a time: that afternoon 20-25 of them queued for
over an hour, blocking releases and the crash investigations, while the load sat at 3-6 on 14 cores.
Only two things need a run to be alone: a measurement needs a quiet machine, and two `queue-e2e.mjs`
dev-ticket runs shared one dev game's queue (`orca-queue-e2e-<profile>`), so they could match each
other. Now:

- `queue-e2e.mjs` gives each run a dev game of its own, `orca-queue-e2e-<profile>-<8 hex>` (its
  first log line names it; `QE2E_DEV_SUFFIX` picks the suffix, empty is the old shared game).
- `Tools/orca/run-lock.py` is the queue. `check -- <command>` is for correctness runs (queue-e2e, the
  drop-in tools, synctests, hashlogs): up to three at once while the 1-minute load is under 12 and
  more than 30% of memory is free, one at a time under 20 and 20%, none above. `perf -- <command>`
  is for anything timed (two-Orca perf gates, frame-time A/Bs, `perf.mjs`, `selfmatch.mjs`): it has
  the machine to itself, waiting for the checks to finish, and no check starts while it waits or
  runs. Both wait while Luis's Orca is open (`ps -axo comm=`, whole paths only: the Orca bundled in
  `/Applications/YouGame.app` or a version in `~/Library/Application Support/yougame-desktop/orca/apps/`;
  a packaged test copy's bundled Orca and a test copy's `--user-data-dir` downloads are not his, so a
  check no longer yields to an agent's own test copy), printing `waiting: Luis's Orca
  is open`, and go first come, first served: one ticket per run in `orca-run-queue/` beside the
  locks, locked by its run, so a run that dies leaves none behind. A check that is running when Luis
  opens his Orca stops (exit 10: three checks are six test Orcas beside his game, enough to make it
  stutter); `--no-yield` keeps one going that must finish, and `--yield` gives a perf run the same
  stop (without it a perf run notes the moment in its output). `--wait-max S` gives up (exit 9),
  `--serial NAME` keeps runs with the same NAME apart (a check waiting only for its key lets later
  checks go past it).
- Runs that share accounts or a queue are kept apart without asking: `queue-e2e.mjs --slug` and
  `page-e2e.mjs` (two signed-in players), a `queue-e2e.mjs` from a checkout older than this one (the
  shared dev game), and runs given the same `QE2E_DEV_SUFFIX`.
- Old scripts keep working beside it: a check holds both old locks shared and a perf run holds them
  exclusive, so a plain `flock orca-run-solo.lock ...` acts as a perf run, and while one waits no
  new check starts. `luis-orca-guard.sh` works as before. Do not wrap a script that takes the old
  locks itself: the run stops with exit 11, since it would wait for itself forever.
- `run-lock.py status` shows who runs, who waits and why (`--json`; `--luis` exits 0 while runs
  wait for Luis's Orca to close, for a lead to tell him). Tests: `python3
  Tools/orca/run-lock-test.py` (a scratch lock folder, fake load and a fake "playing", ~50 s).

### The external SSD

2026-10-06. The worktrees (`~/ClaudeWorktrees`, an APFS image) and the discs live on an external
USB SSD. While other agents copied GBs or ran npm installs there, the image's APFS transactions took
10-180 s to flush (the kernel logs each one: `disk11 tx xid N took M us to flush`), and Orca's reads
from that drive waited as long: Project+ streams its music from `sd.raw` on the emulation thread, so
0.3.28's Mac <-> PC run froze for 36-44 s with the SD image there. In a 117 s stall the discs' reads
on the drive's exFAT side took 2-6 s per MB (25 ms normally). So:

- Timed runs read Project+ from `~/Documents/brawl-dolphin/pplus-internal/run` (a copy of
  `pplus/run`, same SHA-256: `sd.raw` cfe5d869...2d2d, the launcher 74a0decf...c0da). A perf run
  whose command names the launcher on an external disk gets the internal copy instead when it is the
  same release, and says so (`ORCA_PERF_PPLUS_DIR=` keeps the path as given). Checks may keep the
  SSD paths. The discs stay on the SSD.
- A perf run waits until the SSD has gone 60 s without a stall over 2 s: the kernel's APFS lines for
  the image's disk (written when a stall ends) and a 1 MB read around the page cache, timed, of
  `~/ClaudeWorktrees/.orca-ssd-probe` and of each disc the command names (a read stuck over 2 s is
  a stall going on now). `status` shows that wait. It keeps timing those reads every 5 s while it
  runs and ends with `SSD: TAINTED: ...` and every stall that overlapped it (rerun it), or `SSD: no
  stall`. `ORCA_SSD_QUIET_S`, `ORCA_SSD_STALL_S`, `ORCA_SSD_WATCH`, `ORCA_SSD_PROBE`,
  `ORCA_SSD_GUARD=0` change it; checks never look.
- While `status` shows a perf run, nobody copies GBs onto the SSD or runs npm installs in worktrees
  on it.
- A new build's first launch waits for `syspolicyd`, which can take minutes when the SSD stalls:
  `queue-e2e.mjs` and `page-e2e.mjs` give a first `orca caps` 120 s, the Python drop-in tools 30 s.
  Launch a new build once before timing it.

### macOS's check of new programs

2026-10-06. macOS checks each program the first time it runs (`syspolicyd`, "GK performScan" in its
log), one at a time, and holds the program at its launch until the check is done: the process sits at
32 KB and 0% CPU. That evening agents' fresh npm installs on the SSD image (next-swc, esbuild, workerd,
a.out) took 7-125 s each to check, and Luis's Orca 0.3.29 waited 171 s in that line before its own
0.2 s check. So `run-lock.py` (every run, check or perf) waits while a check has gone on 5 s or more:
it reads the last 30 minutes of syspolicyd's log for a "GK performScan" with no result ("GK
evaluateScanResult", "scan failed", "Could not start XProtect scan") and no later scan on the same
thread, says `waiting: macOS is still checking a new program (...)`, and `status` shows `macOS: a
new-program check running ...`. One look (`log show`, about 1 s) is shared by every run for 15 s.
After 20 minutes a run goes anyway with a note. `ORCA_GK_BUSY_S`, `ORCA_GK_WAIT_MAX_S`,
`ORCA_GK_GUARD=0` change it. A stuck check usually means the SSD image is stalling: keep npm installs
and big copies off it while Orca runs are queued.

## Running a session

A session is `ORCA_SESSION=1` plus a disc whose profile is in `Data/Sys/Orca/` (boot refuses anything
else, and anything that would change what the game computes: see `Source/Core/Core/Orca/Profile.h`).
Online play is drop-in, through a private YouGame room of up to four (`Source/Core/Core/Orca/Session/Online.h`,
"Drop-in" below): the host plays at once, and a friend who joins plugs into the next port.

| Variable | Meaning |
|---|---|
| `YOUGAME_BRIDGE`, `YOUGAME_TOKEN` | Set by the YouGame desktop app: tickets come from its loopback bridge. |
| `ORCA_ROOM` | Room code (6-12 of `a-z0-9`). Default: the app's invite, else a new code. |
| `ORCA_JOIN` | `1`: join the friend's game in that room (also implied when the app opened an invite). |
| `ORCA_TEST_KEYFRAME_DIR` | Tests only: keyframes go through this folder instead of YouGame. |
| `ORCA_TEST_LEAVE_AT` | Tests only: a joined friend leaves at this frame. |
| `ORCA_TEST_COMMANDS` | Tests only: `1` takes the embed protocol's stdin commands without embedding, `test-drop-room` (the room's connection drops once, as a network failure would), `test-direct off\|on\|in\|out\|rebuild` (direct links drop every datagram, only incoming or outgoing ones, none again, or are built anew; "Direct links") and `test-shot <name>` (a screenshot `ScreenShots/<name>.png` at the next present, a stall's redraws included; with `ORCA_TEST_PRESENT` the overlay is in it). |
| `ORCA_DIRECT` | `0`: no direct links, the relay only (the hello stops offering them, so the friend's Orca uses the relay too). Default: on. |
| `ORCA_DIRECT_FORCE` | Tests only: `turn` offers and accepts relayed candidates only (a link through Cloudflare TURN); `srflx` nothing on a host interface. |
| `ORCA_DIRECT_ICE_LOG` | Tests only: `1` logs every libjuice line (its debug level, no per-second cap) at notice level, each distinct address as `<addrN>`, so the log's timestamps show what the ICE agent sent when ("Wi-Fi"). |
| `ORCA_DIRECT_STUN` | Tests only: `0` leaves the ticket's STUN server out of the agent ("Wi-Fi"). |
| `ORCA_DIRECT_TURN` | Tests only: `0` never uses the ticket's TURN server, `1` from the first link (before 0.3.21 every link had it). Default: only after a link found no path ("Direct links", "TURN only when needed"). |
| `ORCA_DIRECT_LOOPBACK` | Tests only: `1` also offers loopback candidates, for two Orcas on one machine whose LAN address refuses a send to itself. |
| `ORCA_DIRECT_LOSS`, `ORCA_DIRECT_BLOCK` | Tests only: drop this percentage of the datagrams sent; `1` starts with every datagram dropped (as `test-direct off`). |
| `ORCA_NAME` | Name shown in the room. |
| `ORCA_TEST_DEV_GAME` | Tests only: an anonymous dev ticket from the site instead of the app (no ratings, no account). |
| `ORCA_SITE` | Tests only: the site for dev tickets (default `https://yougame.co`). |
| `ORCA_TEST_PADS`, `ORCA_TEST_SKIP_RENDER` | Harness overrides; they change the compatibility key. |
| `ORCA_TEST_GATE` | Tests only: masks in the input gate ("Input gate"). A test override too. |
| `ORCA_TEST_FULL_SNAPSHOTS` | `1`: rollback snapshots copy all of RAM instead of copy-on-write (`Source/Core/Core/Rollback/Cow.h`), to compare the two. A test override too. |
| `ORCA_THREAD_QOS` | `0`: no interactive scheduling for the CPU, room and controller-stream threads ("Input latency"); `1`: also for a scripted harness run's CPU thread. |
| `ORCA_TEST_PRESENT` | Tests only, headless (`-p headless`): `WxH` (1x1 up to 7680x4320) presents into an offscreen image of that size, the OSD and Orca's overlay on it, and screenshots (`YG_SHOT_EVERY`) and frame dumps take that image instead of the bare frame. Presenting then waits for its present time like a window does (headless normally presents nothing). Each screenshot's readback is a frame of its own ~13 ms longer, which the frame meter shows. With `ORCA_TEST_COMMANDS=1`, a `rect` line resizes the image to its W x H, as it would size the window, so `view`, `dim` and the overlay's placement can be tried headless. Not with the Software renderer (it draws no UI); the compatibility key is unchanged. |
| `ORCA_TEST_NO_AFP` | `1`: an ARM CPU with FEAT_AFP (Apple M4 and later) behaves as one without (M1 to M3): FPCR.AH is never set and the JIT takes its no-AFP paths ("Floating point on every host"). A test override too. |
| `ORCA_TEST_CPUCORE` | `0` (interpreter) or `5` (cached interpreter) instead of the host's JIT, as a reference. A test override too. |

**Orca.app** (the macOS bundle, `Tools/orca-package-macos.sh`) is what the YouGame desktop app
starts, with no arguments. A bundle build is a session by default (`ORCA_SESSION=0` turns that off),
its user directory is `~/Library/Application Support/Orca` unless `-u` says otherwise, and without
`-e` it asks for the player's own Brawl disc image with a macOS open panel. It checks the game ID
and revision before boot, asks again after a wrong disc (`orca error disc_revision` if the player
then gives up, `orca error cancelled` if they never chose one), and remembers the path in
`Config/Orca.ini` `[Disc] Path`, asking again when that file is gone. Until the canonical save
exists, Orca.app sessions start from an empty NAND (save hash 0) whatever the user directory's NAND
holds; Brawl makes its save in-session on both machines. A session with no `Config/GCPadNew.ini`
gets `Sys/Orca/Input/GCPadNew.ini`: port 1 on the keyboard (Dolphin's default keys) and the first
SDL gamepad of any model (`SDL/0/*`, a device name Orca matches as a wildcard).

A file given with `-e` (the app's disc, or a launcher profile's loader) is checked before anything
else starts: nothing at the path prints `orca error disc_missing <path>`, a file that won't open
`orca error disc_unreadable <path>`, and Orca exits 1 within a second (outside a session the same
goes to stderr). Dolphin's own check raised a panic alert there, before Orca's alert handler was
in place: on Windows a message box nobody saw behind the app's window, and the run waited on it
with nothing on stdout. Orca's handler is now registered before the boot file is read.

Two friends meet when both use the same code and the same compatibility key: the Orca build, the
game profile and data files, every forced setting (FMA included), the disc and the seeded save.
Not the host CPU: an Apple Silicon Mac and an x86-64 PC compute the same game (a 12,001-frame
scripted match hashed identically on both, 2026-10-01), so Mac and PC players meet, and so do Macs
with and without FEAT_AFP ("Floating point on every host" below). An x86-64 CPU without FMA3 has a
different key. The host (the Orca that started the game) is seat 0, controller port 1; a friend's
seat is its room slot.

Unit tests: `ninja tests && ./Binaries/Tests/tests --gtest_filter='Orca*'`. With `ORCA_LIVE_TEST=1`,
the `OrcaLive` tests also open real rooms on yougame.co. Two Orca processes over real rooms, steered
like the app steers them: `Tools/orca/dropin-commands.py <nogui> <disc>` (join, leave, rejoin, the
host leaving), `Tools/orca/dropin-rooms.py <nogui> <disc>` (a room with no host, a friend first
in the host's room, the host's connection dropping mid-match) and `Tools/orca/dropin-delay.py
<nogui> <disc>` (the input delay for five minutes after a join at character select or mid-match;
with `--friend-env ORCA_TEST_NET_SPIKES=... --host-env ORCA_TEST_NET_DELAY_MS=0 --expect-raise
S1-S2`, through a test link that goes dark again and again) and `Tools/orca/dropin-identity.py
<nogui> <disc>` (each player's own name-tag controls in the other's game, "Port values" below).

**Launcher profiles (prototype: Project+).** A mod that boots through its own loader program runs
from a launcher profile, picked with `ORCA_PROFILE` (`Data/Sys/Orca/PPLUS32.ini` is Project+ v3.2).
The session boots the loader executable (`-e <launcher.dol>`) with the disc as Dolphin's default disc
(`-C Dolphin.Core.DefaultISO=<disc>`), and refuses unless the loader, the disc's ID and revision, and
the SD card image next to the loader match the profile (XXH3-128). The SD image is opened read-only:
every session forces SD writes and folder sync off. The profile also names the save that seeds the
session NAND (`SaveTitle`), and its frame hook goes live once the game's instruction is at the
address. Its `Title` (Project+) names the game in the window title instead of the disc's. Its game patches (`Data/Sys/Orca/<profile>.patches`, `Core/Orca/UX/GamePatches.h`) apply
from then on, as guarded groups only (code groups, and the one data group that starts the rules'
items at None once, as `RSBE01.patches` does for Brawl): `PPLUS32.patches` skips Project+'s "Create
save file for Project+?" prompt the way `RSBE01.patches` skips Brawl's (Project+ loads Brawl's boot
module unchanged), so a session with no save writes none (answering Yes cost 13 s of save
writing), and its codeset's "Boot Directly to CSS" takes the title's path instead: the session
reaches the main menu at frame 197 ("Boot and friends from the menus"). None of a
mod's files are in this repository; the player's machine downloads the official release. Example
(Project+'s `Project+ Netplay Launcher.dol` and `sd.raw` side by side in `<dir>`):

```bash
ORCA_SESSION=1 ORCA_PROFILE=PPLUS32 dolphin-emu-nogui -p headless -u <user> -v Null \
  -C "Dolphin.Core.DefaultISO=<RSBE01 rev 2 disc>" -e "<dir>/Project+ Netplay Launcher.dol"
```

Orca.app started with no arguments reads `orca-launch.ini` next to the app, when the build ships
one: `[Launch]` with `Profile` (a launcher profile) and `Executable` (the loader, relative to that
folder; the profile then finds the SD image next to it). It boots that loader with the disc the
player picked as the default disc. The parser refuses unknown keys and sections, and any path that
is absolute, has a `.` or `..` component, or resolves (symlinks included) outside the folder
(`Source/Core/Core/Orca/Launch.h`). A YouGame mod listing's native build is Orca.app plus the mod's
files and that ini, side by side.

A session also refuses whatever would change one player's game alone: pausing, loading savestates,
the reset button, and the Wii power button (Stop ends the match instead).

Stock behaviour Orca changes outside a session: the build uses the bundled libcurl (with mbedTLS
and the system CA file) on macOS and Linux, because the netcode needs libcurl's WebSocket support.
That client doesn't use the macOS Keychain's extra roots, and it doesn't do TLS 1.3. On Windows the
bundled libcurl uses Schannel and the system store, with certificate revocation checked best effort
(a CRL or OCSP server that can't be reached no longer fails the connection).

The local controller: under the YouGame app, the app's controller helper reads GameCube adapters
(libusb, raw reports) and gamepads, and Orca takes the newest snapshot of its stream at each frame
boundary (`Core/Orca/UX/ControllerSource.*`). Without the app, it is port 1 as mapped in Dolphin's
controller settings. A session never runs Dolphin's own adapter code: its ports are standard pads.

### Controller input age

How stale the game's view of an adapter is when a frame reads it (`ControllerSource.h`). Only a
frame's own read counts (re-runs and a joiner's catch-up don't), and only reads whose pad differs
from the frame before's (an unchanged pad's age says nothing). The age is that of the newest adapter
report, made of two spans that are each timed on a single clock: the helper's (from the USB report
to the snapshot leaving the helper, on its steady clock) and the wait in Orca's cache (on Orca's).

- The hop through the app between them is not in the age. Its two ends are different processes'
  wall clocks: the helper reads the wall clock once at start and counts on its steady clock from
  there, and Orca reads its wall clock at each arrival. Those drift apart (this Mac's wall clock
  gains 4.5 ppm on its steady clock, 0.27 ms a minute) and step (NTP, w32time on Windows), so an
  hour-old helper would add about 16 ms to every sample. Instead the ROLLBACK lines carry the hop's
  jitter: each hop above the fastest of the second before it, where the clocks' offset cancels and
  their drift is microseconds. A clock step that lengthens the hops shows there for a second, never
  in the age. The whole transport, helper and hop together, is what the latency check measures:
  0.1-0.3 ms on loopback (`Tools/orca/controller-latency.mjs`; its scripted helper starts with the
  run, so its clock has had no time to drift).
- With every report published, the mean is about half the USB report interval plus the helper's
  part. So the 125 Hz the official adapter reports at shows as a little over 4 ms, and 1 kHz as a
  little over 0.5 ms. Not counted besides the hop: the adapter's own controller poll (its data is
  up to ~1.2 ms old when a report leaves) and the host's USB stack.
- Each `orca stats` line carries `pa`, the mean age over the interval in ms, and `rpf`, the adapter
  reports per frame (2.1 at 125 Hz, 16.7 at 1 kHz; fewer than the adapter sends means the helper
  merged reports while the app was slow to take them); both are -1 when no frame read an adapter.
  The app keeps only the stats keys it knows (`STATS_KEYS` in `desktop/src/orca-policy.ts`): netlog
  shows these two once it lists them.
- The ROLLBACK log has a line every 10 s with the percentiles, the split and the hop, and one for
  the run when the stream stops.
- Without an adapter: `Tools/orca/fake_bridge.py` with `FAKE_PAD_HZ` and `FAKE_PAD_NOISE`, and the
  `OrcaUXLatency.BoundaryInputAge` test (commands in the test). `FAKE_HELPER_SKEW_MS` and
  `FAKE_HELPER_DRIFT_PPM` give the fake helper a clock like the real one's. On this Mac, 10 s at
  59.94 reads a second: 125 Hz, 4.44 ms (helper 0.00, cache 4.34; hop p50 0.06); a helper an hour
  behind and losing 1000 ppm, 4.35 ms (the hop's p50 1.01: that drift over its one-second floor);
  1 kHz, 0.59 ms, and 0.62 ms with that clock. Before the split, the same clock read 60004 ms for a
  helper one minute behind and 9.68 ms for one losing 1000 ppm.

## Drop-in

Luis's rule: the host plays at once; an invited friend joins as if they had plugged a controller
into the next port of the host's game, and both machines show it. Luis, 2026-10-01: the friend
loads the host's latest state and syncs from there, so joining never takes longer the longer the
host has played.

### How it works (`Rollback/OnlineMatch.cpp`, `Orca/Session/Session.*`, `Orca/Session/Keyframe.*`)

1. **Solo.** A session machine always runs inside the determinism envelope. Online, SI is forced
   to four GameCube pads, and every port without a player reports no controller: the wire pad's
   bit `0x80` of byte 0, `UNPLUGGED_PAD`, outside the button bits, so `SI_DeviceGCController`
   answers nothing. The host's port 1 is its local pad with no input delay. It logs every
   frame's pads (32 bytes a frame) and joins its room in the background as the host. There is no
   rollback and no snapshot.
2. **Keyframe.** On an invite (`Online::PrepareJoin`, the embed host's `prepare-join` line) or
   when a friend arrives in the room, whichever comes first:
   - At its next frame boundary, the host captures the machine with `SnapshotRing::Capture`
     (device state and MEM1/MEM2, the same bytes a rollback snapshot holds).
   - It takes the session NAND as a copy-on-write `clonefile` (instant on APFS; elsewhere read at
     the boundary). The NAND travels because rollback snapshots keep only a journal mark.
   - The NAND's `/tmp` files that the game's boot writes whatever the player does (`BootNandFrame`
     in the profile) travel as XXH3 hashes only. Brawl writes 38.7 MB of packs by frame 1800 and
     Project+ 2.3 MB by frame 300, byte-identical with or without input (and unchanged through a
     match, checked with the harness's `YG_NAND_HASHES_AT`). Saves always travel: a save the boot
     writes can depend on what the player pressed (Project+ without its patches wrote Brawl's on
     the player's Yes). The host hashes its NAND at that frame. The joiner runs its own boot to
     that frame, unthrottled and unseen, while it downloads, then fills those files in from its
     own NAND, refusing the keyframe if one is missing or differs.
   - A thread packs everything, compresses it with zstd level 3 (long-distance matching, 128 MB
     window), encrypts it with AES-256-GCM under a fresh random key, and stores it through a
     `KeyframeStore`. The game goes on meanwhile.
   - The key travels only in the room's `kf` message, so the store holds ciphertext.
   - Never of the host's first 300 frames (5 s): Project+'s first frames after its launcher run
     differently on a machine whose JIT holds other blocks (frames 3-9 differ in a synctest that
     drops the JIT after every load). A friend who arrives earlier waits.
   - A keyframe less than 30 s old is reused; each one serves one joiner, who deletes it after
     loading.
3. **Join.**
   - The host starts a rollback session at its next frame J. It sends the friend the keyframe's
     id, frame K, size and hash.
   - The friend's Orca boots the same disc and profile, runs its boot to `BootNandFrame`, then
     waits (`orca state joining <percent>`; the host shows `orca state friend-joining`).
   - It downloads and checks the keyframe, replaces its own temporary session NAND with the
     host's (`ReloadFst`), and loads the state with `SnapshotRing::LoadImage`.
   - It then runs the host's logged pads for [K, J) (history in packets, runs of equal pads sent
     once) and the host's live inputs from J. It runs unthrottled, without rendering or sound,
     until it has caught up.
4. **Plug in.** Once the friend has caught up, the host picks S as its own frame plus a margin (the
   rollback window, twice the maximum delay, and the round trip). It announces S in its roster,
   which every packet carries. Both machines plug port 2 in at exactly S. From S the seat counts
   for stalls and checksums like any other, and the host never waits on a joiner before that. Once
   the last friend on the way has plugged in, the host prints `orca state friend-joined`.
5. **Leave.**
   - The friend asks to leave; the host picks an unplug frame L ahead in the same way. The
     friend stops after its inputs before L are acknowledged. The host's session runs 12 frames
     more once the last friend is out, so that acknowledgement gets through even when a packet is
     lost or a matchmade room's links close behind it (0.3.15). The friend's leave is also over
     when the host leaves the room (a matchmade room's host does, at once), or after 3 s with no
     word once past L. A leaving friend prints `orca state left`, never an error.
   - A friend whose connection drops is unplugged by the host after its last input, rolling back
     the frames that guessed otherwise.
   - A friend who stops answering is dropped after 10 s.
   - Why a friend went reaches both sides:
     - The host prints `orca state friend-left <left|desync|stalled|network>` and keeps playing.
     - A friend whose own session saw a desync leaves with `bye desync`.
     - A friend the host unplugs for a desync (or for going quiet) gets a `drop` message and
       ends with `orca error desync` (or `peer_left`).
     - A friend whose host left ends with `peer_left`, or with `desync` when the host's bye
       said so. One stalled on the host's inputs hears that at once, not after the 10 s.
   - With nobody else plugged in, the host hands its log back and plays solo, with no input delay.
   - A third or fourth friend goes the same way on seats 3-4. Only two players have been tested.
6. **Rooms.**
   - Orca asks for 4-seat private rooms with the game-owned lobby. No room match is ever started,
     so there are no results or ratings.
   - Seats are lobby slots. A hello carries the compatibility key and whether the sender is the
     host.
   - The rooms Worker needed no change: it admits late joiners, assigns the lowest free slot and
     relays `msg` between connections.
   - The host must hold slot 0 and be the room's host. A friend who reached the code first (an
     invite sent before the host's game got there) holds both, so a host that finds itself on
     another slot gives that room up and opens a fresh one, which the page then mirrors.
   - A joiner that hears no host hello within 8 s of its welcome (nobody's game is in the room: a
     stale invite, a host that left or restarted, or got there second) ends with `peer_left`
     ("Your friend isn't in that game any more") instead of waiting out the 90 s join limit.
   - The server answers a ping every second; 8 s without any message from it ends the room
     (`network`), as a closed socket does, and closes it as a dropped connection (no `leave`, close
     code 4001), so the server holds the seat. Not much less than 8 s: a keyframe upload fills a
     home uplink, and the pings wait behind it.
   - A host whose room ends without it leaving (a dropped or silent connection, a server restart,
     the code taken) prints `orca error network <why>` once (with `caps join`) and reopens:
     2 s later, then backing off to 30 s between tries. Within the server's 90 s hold the same code
     comes first: the app's player id comes back into its held seat, and invites already sent still
     lead there. Once a room works again, `orca state playing` follows. A host's session that ended
     with its room doesn't report `friend-left network`, and a re-run in progress finishes first.
   - A socket the server replaced (close code 4000: another run of the game on the same account
     took the room) is the other run's room: a host opens a fresh one rather than take it back, and
     never goes back to a room someone else holds, whatever ends the tries after.
   - A rollback the joiner can't make (`No snapshot to roll back`, `exceeds the limit`: an engine
     fault, not the games going apart) ends its session as `network` with the error in the
     sentence; every session end logs its error (`Online: session ended at frame N: …`).
   - A player the room re-seats on a new socket (`back`, or `join` for a listed id) is another run of
     Orca: it is unplugged like an `away` one, and greeted (and offered a keyframe) anew.
   - A room the page was shown ends with an emptied mirror (its code, no players or participants,
     `ready: false`), so the page knows Orca left it. Each room mirrors only while it is the
     process's newest: an older room's late update never overwrites a newer one's.
   - The page's lobby Leave is the app's `leave` (with `caps leave`); without it, it ends the room. A
     host that leaves always opens a new room, even with nobody in the old one: the page reads
     `orca state left` as Orca having left its room. The new room opens a second later, so the
     app's Stop (`leave`, then `quit` at once) never asks for its ticket; the old room's goodbye is
     still sent, since the process waits up to 2 s at exit for rooms it is leaving. A friend hears
     the host went at once, not after its silence limit.
   - A player on another port than 1 (it joined a friend's game and then went solo: it left, its
     host did, or the session failed) goes home (Luis, 2026-10-05: "a guest whose host left goes
     back to their own lobby"; `OnlineMatch.cpp` `ComeHome`): at its first alone boundary on the
     menus (the main menu, any page of it, or a character select; never a fight, a stage select, a
     results screen, the moment between two scenes or a single-player mode: `UX/OnlineMenu.h`
     `MenusScene`), with no session, nothing of drop-in under way, the room it was in gone and no
     queue image to put back, it moves to port 1 ("Drop-in: back to a game of its own on the menus")
     and a room of its own opens a second later (`ReopenRoom`, as after a queue room). Home on the
     character select a With Anyone pick opened (its host left the friends lobby for the queue at
     that pick: "Online menu", "Casual or Ranked with friends in the game"), it announces that pick
     too (`orca menu online casual|ranked`, whatever its page can search: alone, the page answers it
     as any pick), so its Start readies it on its own queue select. Before branch `q11-lobby` it
     stayed on its port with no room: nobody could drop in, and the queue's own character select
     never got its header there (it wants port 1), so With Anyone > Casual or Ranked and Start did
     nothing at all (YouGame's 2026-10-05 "Start did nothing": a friends-room guest whose host
     stopped). Solo with no session, so the boundary is a first run nothing re-runs, and only this
     game changes: its pads (port 1 plugged in, the old port unplugged) and, from the next boundary,
     the frame hook's ports; the next friend's keyframe carries it. On a character select the move
     comes a frame after the old host's port unplugged, before Brawl closes that panel, so port 1's
     panel keeps what the old host put there (its character, token down) and the guest's hand now
     drives it, while the guest's own panel (its old port, unplugged) closes: it picks again
     (screenshots 2026-10-05, both tokens down on With Friends' select, then the host's app
     stopped). Cosmetic, and only there: the select a With Anyone pick opens starts with nobody's
     token down, so nothing carries over on the way to the queue. A guest in a fight when its host
     goes still prints `orca state no-room` once, after the room it left has told the page it is
     empty (and never mirrors that room again), and goes home once back on the menus. The app can
     still send `join` meanwhile (it wins over going home).
   - A friend that leaves while it still catches up (never plugged in) just leaves the room at
     once; the host drops the seat (`friend-left left`).

**Port values: each player's name and own controls** (`Rollback/OnlineMatch.cpp` "Ports",
`Core/Orca/UX/NameTags.h`). A joiner plays on a copy of the host's machine, save included, so
its own name tags are not there. Each port carries its player's values instead, which the frame
hook writes into the port's name tag on the character select:
- **What:** the YouGame username, as the tag's name, and the player's own controls: the tag's
  rumble byte (+0x0C) and its whole layout (+0x14, 0x2D bytes: GameCube, Wii Remote, Nunchuk,
  Classic, tap jump and shake smash included). As a match starts (scMelee's first frame), Brawl
  and Project+ copy each port's tag layout for its controller into the per-port table at
  0x805B7480, so the port plays with its player's own buttons and tap jump.
- **Whose:** a player's own controls are read from its own game while it plays alone: the tag its
  port wears on the character select (or last wore there), else its YouGame tag (`Events::
  OwnControls`). A joiner sends them in its hello (`ct`); the host takes its own as a session
  starts.
- **When:** the host decides each port's values and the frame they apply from, and every machine
  holds them before it runs that frame. A joiner gets them with the keyframe offer (`kf`'s `nm`,
  with their version `nv`). Players already in the game hear them (`nm`, `v`) and acknowledge the
  newest version in their packets (`va`); the host plugs a newcomer in only once every player has
  (`Session::RequireValues`), so its plug frame, which each player hears of before running it,
  comes after its values. The host's own values change only while it plays solo.
- **Safety:** a profile from another machine reaches the tag only as the game's own menus could
  have set it (rumble 0 or 1, flag bytes only their bits, actions up to 0xE, else the default),
  and only at its exact size; the wire takes at most 64 bytes. A port with controls and no name
  the game can show wears "P<port>", like the game's own label. Friends rooms carry controls too:
  a joiner plays with its own layout in a friend's game.
- **Proof** (2026-10-03, Brawl rev 2 and Project+): `YG_SYNCTEST=7` from character select through
  the match's start with both ports carrying custom controls (`ORCA_UX_TEST_NAMES`,
  `ORCA_UX_TEST_CONTROLS`): 5,621 and 4,221 re-run frames, 0 RAM mismatches. `YG_KEYFRAME_TEST`
  where each port gets its tag and across the match's start: PASS in both games.
  `Tools/orca/dropin-identity.py` (two Orcas, each with its own layout set in its own game, the
  friend joining from its character select): both games' tags hold each player's layout and
  rumble, the match's per-port table shows port 1 with the host's and port 2 with the friend's on
  both machines, and the hash logs agree on every frame from the keyframe on (Brawl 3,073
  frames, 50 checksums; Project+ 1,628 frames, 27 checksums).

**The frame hook runs in the host's floating-point mode** (`Rollback.cpp`, `HostFloatScope`).
- The CPU thread runs compiled C++ in the guest's FP mode (`PowerPC::RoundingModeUpdated`). In
  Brawl that is FPCR `0x1000002` (FZ and AH) on Apple Silicon, and MXCSR with the guest rounding
  mode and FTZ on x86-64.
- Under FPCR.AH, FNEG and FABS leave a NaN's sign alone, and clang builds some 64-bit constants
  with MOVI + FNEG. A `{0, INT_MAX}` roster entry came out as `{0, -1}`.
- The hook now switches to the host's default on entry. On the way out it sets the guest's mode
  again from its FPSCR, which a snapshot load inside the hook may have changed. That is exactly
  what the JIT itself sets.
- The synctest (0 RAM mismatches over 6,384 re-run frames) and the loopback (PASS, 899
  checksums) are unchanged. As defence in depth, "never" is still `0x3fffffff`.

**Host arithmetic** (0.3.15, `Core/Orca/UX/Queue.*`, `Source/Core/Core/CMakeLists.txt`). What
Orca's own C++ works out from emulated memory on every machine (the input gate's masks, presses and
sticks, whatever a hook writes to RAM) is integer arithmetic. The frame hook runs in the host's
default FP mode (above), but the builds still differ: Apple clang contracts `a * b + c` into one
fused multiply-add on ARM64 by default, which rounds once where MSVC's x86-64 code rounds twice.
Through 0.3.14 the queue's steering (`Queue::SteerToward`) put the joiner's stick in from
`sqrt(dx * dx + dy * dy)` in floats, and the Mac build fused it (`fmadd s1, s2, s2, s1`): 12 of 20
million random (hand, pick) pairs gave a Mac and a PC sticks a unit apart, a desync during game 1's
steered pick that no synctest on one machine can see. Since 0.3.15:
- The steering is fixed point: `Queue::ToFixed` reads a float's value in 1/65536 units from its bits
  alone (NaN, infinity and 4,096 units or more are no place: centred), `IntSqrt` is exact, the
  divisions truncate and the axis rounds half up. Same behaviour: over 2 million pairs on the
  character select every stick is within a unit of the float formula worked out in double
  precision, 99.98% equal (the rest within rounding of a half). The "all but stopped" test before A
  (`Queue::HandStill`) is integer too.
- `OrcaQueue.SteeringIsTheSameOnEveryHost` pins golden sticks every host must give (the real flows'
  picks, the near cases, and the pairs where the old version split Mac from PC). The same code
  hashed 5 million sticks identically built for arm64, x86-64, and x86-64 with FMA and
  `-ffp-contract=fast`; the old float version, on the same inputs, did not (arm64 and x86-64).
- The Orca and Rollback sources (`Orca/`, `Rollback/` in `core`) build with `-ffp-contract=off` on
  clang and gcc, as a belt to those braces. Without it the Mac build also fused in
  `DirectLink::Deliver` (the flood limit), `Session` (stall accounting, the snapshot spacing) and
  the overlay, all one machine's own; with it none of the 43 objects has a fused op. MSVC needs no
  equivalent: `/fp:precise` (root `CMakeLists.txt`) never contracts in VS 2022 and later, and the
  x86-64 baseline (no `/arch:AVX2`) has no FMA instruction to emit.

What is still float in `Core/Orca/UX`, `Core/Orca/Session` and `Core/Rollback` (swept 2026-10-04):
on values every machine shares, only operations without rounding, whose answers no compiler or FP
mode changes: compares of a float from RAM against a constant (`CssHandMayPressA`'s grid edges,
the queue identity's "finite and within 100", the ranked set's damage `> 0 && < 10000`), the
truncation of that in-range damage to the percent, bit copies (`std::bit_cast`, the pick's place
into the queue block), and constant floats written to RAM (the stage select's cursor). The rest is
one machine's own and never has to agree: the joiner's pick place (worked out on its own machine
and sent as bits in the queue identity), the controller snapshot to pad conversion (the pad travels
as PadCodec's 8 bytes and both machines decode their own from those), timing, pacing, the
adaptive delay and stall accounting (the host's plug-in frame is sent, not recomputed), stats, the
overlay, and the harness's fault injection. No `sqrt`, `pow`, trig, `hypot` or rounding that every
machine works out for itself reaches emulated memory or inputs (the controller conversion's
`lround` runs on the player's own machine only).

**Floating point on every host** (2026-10-02). Every Mac and PC computes the same game, whatever
the CPU. Three hosts are in play, and each one's own floating point differs from PowerPC's in the
guest's non-IEEE mode (FPSCR.NI, which Brawl and Project+ run in):

| | x86-64 (Jit64) | ARM with FEAT_AFP (Apple M4+) | ARM without (Apple M1 to M3) |
|---|---|---|---|
| Mode Orca sets | MXCSR FTZ | FPCR FZ, AH | FPCR FZ |
| NaN made from no NaN (inf * 0) | negative | negative | positive (PowerPC: positive) |
| Denormal inputs | kept (as PowerPC) | kept | flushed to zero |
| Denormal outputs | flushed, tininess after rounding | flushed, after rounding | flushed, before rounding (as PowerPC) |

Project+ makes a NaN (fnmsubs, inf * 0) as a match starts, so for a few hours on 2026-10-02 a
Project+ player on an M1 to M3 was kept from the others (the room hello's float class,
`platform_mismatch`). That is gone. What a session does instead:

- **PowerPC's NaN rules** (`Profile.cpp` forces `MAIN_ACCURATE_NANS`): an input NaN is picked in
  PowerPC's order (a, b, c) and made quiet; a NaN made from none is `0x7FF8000000000000`; a NaN
  result is never negated. Both JITs' accurate-NaN paths now cover every arithmetic op: Jit64's
  fadd/fsub and ps_sum didn't fix x86's negative NaN, and JitArm64 kept the host's NaN when no input
  was one (negative under AH), missed a NaN in ps0 alone under AH (FMAXP returns its second operand
  there) and made ps_sum's NaN the host's. frsqrte of a negative number gives PowerPC's NaN on ARM
  too, fctiw(z) of a NaN gives `0x80000000` as on PowerPC and Jit64 (FCVTZS gave 0), and a NaN
  stored as an integer by psq_st saturates the way Jit64 stores it (the interpreter has no answer
  there). JitArm64's frsqrte also missed FPSCR.VXSQRT for a negative input of magnitude 2 or
  more (its NaN-or-infinity test was the exponent's top bit), which Jit64 and PowerPC set.
- **Denormal inputs kept**, as PowerPC and the other two hosts do: on a CPU that flushes them
  (`JitArm64::DenormalInputsFlushed`), a single that may be a denormal (loaded, not produced by an
  arithmetic op in the block) never goes into a single-precision operation, compare or conversion
  (`IsSingleForArithmetic`); the op runs in double precision, where every single is a normal number,
  as Jit64 computes everything. JitArm64's frsqrte takes a double denormal's exponent from the
  input instead of from FRSQRTE, which reads it as zero under FPCR.FZ on every Apple CPU, M4
  included (checked on this M4 Pro: FRSQRTE of 2^-1074 is +inf with FZ, with or without AH). In a
  session, rounding to single where Jit64 rounds (psq_st, ps_sum's copied lane) applies on every
  ARM CPU.
- **Paired loads and stores:** in a session JitArm64 makes an SNaN that psq_l loads quiet, as
  Jit64's CVTPS2PD does (an SNaN pattern loaded by psq_l and stored by stfs or stfd, or moved by
  fmr first, was `0x7FC00001` on a PC and `0x7F800001` on a Mac); denormals stay exact. And it
  leaves to the interpreter the psq_l and psq_st that Jit64 leaves there (address translation off,
  or rA = 0), whose answers differ from both JITs' (psq_st truncates to single). Project+'s codes
  use rA = 0 (psq_stx f14 to f31 at `0x803f2e70`, psq_lx at `0x801629bc`), so a PC ran those on
  the interpreter and a Mac on its JIT before. Falling back there first stopped JitArm64 on its
  own register cache (an FPR the fallback had flushed, flushed again as unused, unlocked
  `INVALID_REG`), which `Arm64FPRCache::FlushRegister` now skips as the GPR cache does; any
  fallback whose FPRs nothing read afterwards (an Rc = 1 float op, say) could hit it.
- **The interpreter under the host's mode.** The JITs fall back to the interpreter (Rc = 1 forms,
  among others), and that C++ runs in the guest's FP mode too, so under FPCR.AH (an M4) the host's
  FABS and FNEG leave a NaN's sign alone and Clang's `std::copysign` of a double, whose mask it
  builds with FNEG of all ones, loses the sign (the same trap as the frame hook's above). The
  interpreter's fabs is bitwise now, and fres (`Common::ApproximateReciprocal`) and fctiw's
  round-to-nearest (`RoundToIntegerMode`) put the sign on with integer operations: on an M4,
  fres(-0) was +inf, and fctiw of -1.5 or -3.5 rounded to -1 and -3.
- **Jit64's fres and ps_res of -0.0** set FPSCR.ZX and FX, as the interpreter and JitArm64 do (its
  zero test missed the sign bit). Those bits reach RAM through mffs and an OS context save.
- FEAT_AFP is detected on macOS (`hw.optional.arm.FEAT_AFP`; Dolphin never set it on Apple), and
  FPCR.AH is set only where the CPU has it. `ORCA_TEST_NO_AFP=1` makes an M4 behave as an M1.
- **Outside a session (stock Dolphin behaviour, for the release notes):** the accurate-NaN fixes
  apply wherever Dolphin's accurate-NaN setting is on; frsqrte (negative inputs, double denormals,
  FPSCR.VXSQRT for a negative of magnitude 2 or more, which JitArm64 skipped), Jit64's fres(-0.0),
  the interpreter's fabs, fres and fctiw under FPCR.AH, JitArm64's FPR flush after a fallback, and
  FEAT_AFP detection change for every run. They match PowerPC, Jit64 and the interpreter, and the three bAFP-gated JitArm64 paths
  (FloatCompare, GeneratePsMaddEft, SetSIMDMode) behave the same under AH with FIZ off.
- Forcing accurate NaNs changes a PC's and an M4's results wherever a NaN is made (x86's
  inf - inf was `0xFFF8...`). Matchmaking is unaffected (the build is in the compatibility key),
  but an input recording or replay from an older build no longer reproduces a run that makes one.

Residual differences, all detected rather than silent (the 60-frame checksum exchange reports
a desync and the friend is unplugged), and none measured per match yet:
- **M1 to M3 only, double denormal inputs.** A double below 2^-1022 used as an input is flushed
  there and kept elsewhere. It needs a 64-bit word with a zero exponent loaded with lfd and then
  computed with: compares (fcmpu, fsel, ps_sel through FCMP/FCMPE), fctiw (FRINTI), an fmadd
  addend, frsqrte through an interpreter fallback, and the interpreter's C++ in any Rc = 1 fallback.
  No scripted run (about 80,000 frames) has shown one.
- **M1 to M3 only, tininess before rounding.** A result whose exact value lies within half an ulp
  below the smallest normal and rounds up to it flushes to zero there (AH off detects tininess
  before rounding) but not elsewhere. For singles that window is 2^-25 relative to 2^-126. Only
  inexact results land in it: fmul, fdiv, fmadd, and conversions from double to single (frsp,
  psq_st, rounding to single after an op); a sum or difference of two singles that close to
  FLT_MIN is exact, so it can't. A value decaying geometrically by a factor k per step lands in
  the window with probability about 2^-25 / (1 - k): 3e-7 at k = 0.9, 3e-6 at 0.99, 3e-5 at
  0.999. Making an M1 to M3 detect after rounding has no cheap form (AH is what changes it, and it
  isn't there); a debug counter for single results in [FLT_MIN (1 - 2^-24), FLT_MIN) and for
  double-denormal FP inputs over the scripted runs would turn this estimate into a rate.
- **fctiw of a number that rounds to -0:** PowerPC (and the interpreter) put `0xFFF80001` in the
  upper word, both JITs `0xFFF80000`. The same on every host, so a Mac and a PC agree; only an
  Rc = 1 fctiw (interpreter on both) differs from the plain form.
- An x86-64 CPU without SSE4.1 stores a NaN as a u16 differently (0); every CPU with FMA3 has
  SSE4.1.

Considered and not done: setting FZ without AH on every ARM CPU in a session (what
`ORCA_TEST_NO_AFP` does) would make M1 to M4 share one FPCR, so the M1-to-M3 residuals above
would vanish between Macs, at 504 fps instead of 532 on an M4. x86 could then copy the
before-rounding flush in `FinalizeSingleResult` (a double intermediate below FLT_MIN becomes a
signed zero, exact for fmuls and fadds, and what the interpreter's ForceSingle comment says
PowerPC does). The cost: M4-vs-PC parity would then rest on that x86 change too.

Checked 2026-10-02 on an M4 Pro (FEAT_AFP), with `ORCA_TEST_NO_AFP=1` standing in for an M1 to M3:
- Offline scripted runs (`YG_HASHLOG`) hash the same on every frame three ways: this build, this
  build with the knob, and rel-0.3.5 (`d9ed99f137`, accurate NaNs off, the Mac build that 0.3.5's
  cross-machine smoke matched with a PC). Project+ `pplus-1v1.txt` 6,010 frames (its match makes
  the fnmsubs NaN at frame ~461) and `pplus-dropin.txt` 20,015 frames (four match stretches);
  Brawl `bf-mario-link-results.txt` 10,322 frames and `bf-dropin.txt` 25,012 frames (menus only:
  it stops at stage select offline). Earlier the same held with other Brawl fuzz seeds (20,016
  frames).
- A Project+ synctest (`YG_SYNCTEST=7`, frames 1000-1600) passes with and without the knob: 4,151
  re-run frames, 0 RAM mismatches (the device-state-only mismatches are the emulated icache, as
  before).
- Unit tests: `JitFloatSemantics.*` (the JIT against the interpreter in a session's float setup,
  with PowerPC's answers pinned, data accesses translated so Jit64 compiles psq_l/psq_st too) and
  `JitArm64.FresNonIEEE`/`FrsqrteNonIEEE` pass with and without the knob, as do `Orca*`.
- The x86-64 side was checked by reading Jit64 and under Rosetta 2 (inf - inf is `0xFFF8...`;
  MINPS and MAXSS with a NaN return the other operand; CVTPD2DQ of a NaN is `0x80000000`; FTZ
  detects tininess after rounding), and the pinned psq_st NaN-to-integer answers by reading
  Jit64's quantized store routines. Jit64's changes have not run on a PC yet: run
  `JitFloatSemantics.*` there and one `Tools/orca/xplat.py` match between a PC and a Mac before
  calling the PC side proven (for an M1-class Mac, `ORCA_TEST_NO_AFP=1` on both machines; xplat
  passes `ORCA_TEST_*` through, and the knob does nothing on x86-64 but marks both keys as test
  sessions, so they still meet).
- No real M1, M2 or M3 has run it; the knob is the stand-in.
- `ORCA_TEST_CPUCORE=0` and `=5` run Project+ through that NaN on the interpreters, but their RAM
  differs from the JIT's from frame 0 (each core's timing; scenes shift by two frames), so whole-RAM
  hashes can't compare cores. `JitFloatSemantics` compares them per instruction instead.
- Cost (`Tools/orca/perf.mjs`, Brawl's match, frames 2200-9800 unthrottled, interleaved runs on a
  shared M4 Pro whose other load moves single runs by +-5%): against rel-0.3.5 this build is about
  3-5% slower (604 vs 577 fps over four rounds at a steady load; 627 vs 608 over four more), and
  10-11% slower with `ORCA_TEST_NO_AFP=1` (560 fps), the price of keeping loaded denormals exact on
  an M1 to M3. A real M1 starts from a slower base, so its re-simulation headroom shrinks more;
  not measured on one. The psq_l quieting is about 1% of that (within the noise: 587 vs 577 fps in
  one series, none in the other). Every FP arithmetic op also carries a 15-25 instruction far-code
  stub for its NaN path on both JITs, so the far-code buffer fills (and the JIT cache is cleared)
  sooner in large games. Jit64 pays a compare and a never-taken branch per arithmetic op
  (UCOMISD/JP, or CMPPD/PTEST/JNZ for pairs), plus a register copy where d is an input: an
  estimated 2-5%, not measured on a PC.

**Results** (2026-10-01): two Orca processes on one Mac, over prod rooms with dev tickets and a
local keyframe folder.
- The host played solo for a minute; a friend joined at frame ~3,700 and plugged into port 2.
- Both reached character select, stage select and the fight (`scMelee` at frame 6,017) on the
  same frames.
- 185 checksums matched over 11,000 frames, then the friend left at an agreed frame and the host
  played on solo. A second friend joined (74 checksums matched) and left.
- Join time:
  - 0.5 s Dolphin boot to the first frame, then 0.2-0.4 s until the keyframe is offered: room
    connect and hello, capture 8-10 ms, compress 125-170 ms, store 5 ms;
  - transfer 8 ms (local), unpack 72-96 ms, load 17-23 ms, catch-up 0.5-0.7 s;
  - 0.8-1.1 s from the friend's first frame to plugged in, about 2.2 s from process launch.
- Over the internet, the upload of 38-53 MB adds its own time.

**Limits:**
- Only two players have been tested.
- With a third player, the packet that carries a catching-up joiner's inputs also limits the
  inputs every other player gets: the live players would stall during the join. Catching up
  should come from history only.
- A player dropped abruptly could already have its last inputs confirmed by another player but
  not by the host.
- Prod end to end (2026-10-01): two Orca processes on one Mac, each behind
  `Tools/orca/fake_bridge.py` minting real tickets (`FAKE_COOKIE`, `FAKE_TICKET_SLUG`) for a
  private test listing, through YouGame's keyframe store.
  - The host played solo for a minute; a friend joined, played 3 minutes (183 checksums matched)
    and left. A second friend joined (70 checksums) and left.
  - Keyframes were 25.8 MB and 30.2 MB, uploaded in 2.0 and 3.4 s and downloaded in 2.3 and
    1.8 s.
  - Joins took 5.7 and 7.2 s from the friend's first frame to plugged in. That is about 2.4-3.9 s
    waiting for the keyframe (capture, compress 0.2-0.3 s, upload, offer), the download, and about
    1 s of catch-up.
  - Every keyframe was gone afterwards (404 `gone`).
- Project+ joins work the same way (local store: 23.2 MB keyframe, 32 checksums).
- Desktop only: the NAND capture is an APFS clone on macOS and a synchronous read elsewhere
  (about 55 MB).

### Measurements

Brawl rev 2, `bf-mario-link-results.txt`, Null video, muted, Apple Silicon under a shared load of
8-17.

| Frame (scene) | MEM1+MEM2 raw | zstd -1 | zstd -3 | Non-RAM DoState | Session NAND |
|---|---|---|---|---|---|
| 300 (scStrap, boot) | 96 MB | 23.5 MB | 22.7 MB | 3.6 MB | 2.4 MB |
| 3600 (scBoot, 1 min) | 96 MB | 26.7 MB | 25.8 MB | 3.2 MB | 37.9 MB |
| 5350 (muMenuMain) | 96 MB | 31.3 MB | 30.2 MB | 3.6 MB | 54.2 MB |
| 5600 (scSelctCharacter) | 96 MB | 29.5 MB | 28.4 MB | 3.2 MB | 54.2 MB |
| 7000 (scMelee, mid-fight) | 96 MB | 30.3 MB | 29.0 MB | 3.2 MB | 54.2 MB |

- The NAND is mostly `/tmp`: 37 MB of `ARC` packs that Brawl writes on the strap screen (26.4 MB
  at zstd -3). Brawl's save adds 16 MB from frame ~5000 (`collect.vff` is 14.6 MB, 0.14 MB
  compressed).
- Whole machine at zstd -3 with long-distance matching:
  - 37.6 MB at frame 3600 and 51.5 MB mid-fight;
  - 0.16-0.25 s to compress on all cores, 0.25 s to decompress;
  - long-distance matching saves 4-14 MB against plain zstd.
- In Orca (keyframe of frame ~600-2300): capture 10 ms at the boundary; pack and compress
  125-160 ms on a thread (29-38 MB); unpack 64-99 ms; NAND plus state load 17-77 ms.
- Replay unthrottled with rendering skipped: about 2,100 frames/s in the boot, 1,200 in menus and
  540 in fights.
- With NAND references (2026-10-01):
  - keyframes are 25.8 MB at frame 3,644 (was 37.7) and 30.3 MB mid-fight (was 52.8), with
    unchanged compress times (120-130 ms);
  - joins took 1.4-1.6 s from the first frame, 0.9 s of it the joiner's own boot to frame 1,800,
    which overlaps the download over a network.
- zstd levels on a mid-fight keyframe without references (159.6 MB raw, long-distance matching,
  all cores):

  | Level | Size | Compress |
  |---|---|---|
  | 1 | 50.1 MB | 131 ms |
  | 3 | 48.7 MB | 135 ms |
  | 7 | 47.7 MB | 209 ms |
  | 12 | 46.5 MB | 758 ms |
  | 19 | 42.4 MB | 10.6 s |

  Decompression is about 90 ms at every level. Higher levels don't pay: what's left is already
  compressed game data. Per section at level 3: MEM2 24.0 MB, MEM1 6.4 MB, `/tmp` 26.4 MB, and the
  rest under 0.2 MB.
- Uploading while compressing would save at most the compress time, 0.13-0.17 s, against
  6-10 s of upload at 25 Mbit/s for 20-30 MB. It also needs a multipart upload, since R2 takes a
  body of known length. Not worth it; leaving `/tmp` out halved the upload instead.

### Keyframe store: YouGame endpoint (live since 2026-10-01, YouGame PR #398)

`HttpKeyframeStore` in `Orca/Session/Keyframe.cpp`. Tests use `ORCA_TEST_KEYFRAME_DIR` instead:
the store refuses dev tickets.

- **Address:** `https://yougame.co/api/orca/keyframes/<room>/<id>`. `id` is
  `kf-<frame>-<first 8 hex of XXH3-64 of the body>`. `ORCA_SITE` replaces the site, and a ticket
  reply's `keyframes` URL on the same site wins.
- **Auth:** `Authorization: Ticket <ticket>` with the room's mpTicket ticket, from a signed-in
  player of a live Orca listing.
  - The store takes tickets up to 6 h past expiry, so Orca reuses the room's ticket.
  - On 401 `{"code": "ticket"}` it mints a new one for the same room
    (`YouGameRoom::FreshTicket`) and tries once more.
- **PUT:**
  - `Content-Length`, at most 128 MB; the edge refuses over 100 MB, and keyframes are 26-53 MB.
  - `X-Orca-Frame`, and `X-Orca-Hash` (16 hex starting with the id's 8). The body is the
    encrypted keyframe.
  - 200 `{id, size}`. 409 `exists` means an earlier try landed, and counts as done.
  - 409 `room_full`: another player holds both of the room's slots.
  - 6 PUTs a minute per account.
- **GET:** streams the body with `Content-Length` (progress for `orca state joining`). Orca checks
  the size and the hash, then decrypts. 404 `gone` means it expired.
- **DELETE:** the joiner deletes after loading, off the CPU thread, as best effort. The host
  deletes a keyframe nobody took when it replaces it or leaves.
- **Retries:** network errors, 429 and 5xx retry 4 times with 0.5/1/2 s backoff. Every other
  refusal is final. Errors are `{error, code}`.

### Room messages (relayed `msg`, `Orca/Session/YouGameRoom.cpp`)

- `hello {c: key, r: heard you, h: is host, dl: 1}`: sent until answered. A mismatched key keeps a
  joiner out. `dl` offers a direct link (absent with `ORCA_DIRECT=0`).
- `dl {v: 1, g, key, desc}`, `dlc {g, c: [candidates] | done}`, `dlr {g}`: direct link signalling
  ("Direct links"), to one connection, only between two Orcas whose hellos both offered links.
- `kf {f, id, n, x, key}`: host to a joiner's connection, every second until its first packet
  arrives. `key` is the keyframe's AES-256-GCM key (64 hex).
- `drop {r}`: host to a joiner it unplugged: `desync`, `stalled` or `network`.
- `p`: session packets, broadcast once to the room (and to each linked player directly, without the
  history fields). `q` is the sender's packet count, from 1: a copy no newer than the newest from
  its seat still adds its inputs, acknowledgements and a checksum not taken yet, but never moves
  the roster, the sender's frame, lead or round trip (`Session::ReceivePackets`). Fields beyond the
  2-player ones:
  - `ro`: roster, `[plug, unplug, live]` per seat, with -1 for never;
  - `ha`: history acknowledgement;
  - `ht`, `hf`, `hp`: history seat, first frame, and `[count, 4 pads hex]` runs;
  - `lv`: leaving.
- `bye {r}`: why a player leaves, `desync` or `stalled` (anything else is ignored). It is broadcast
  before the leave, so the room's leave event carries it.

## Direct links

`Orca/Session/DirectLink.*` (branch `direct-link`, 2026-10-03; Luis: "direct p2p is A MUST"). Every
session packet also goes straight to each other Orca in the room over UDP, beside the relay. The
relay costs about 15 ms each way: Mac -> Cloudflare MIA -> the room's Durable Object -> MIA ->
other machine, over TCP. On one LAN that is a 40-50 ms round trip where a direct one takes about
1 ms.

**What Slippi does** (read 2026-10-03: project-slippi/dolphin `Source/Core/Core/Slippi`,
Ishiiruka, slippi-rust-extensions):

- **Data path.** ENet over plain UDP between the two players. Pads go `UNSEQUENCED` on their own
  channel, every unacknowledged pad repeated in each packet (up to 128) until acknowledged, and the
  latest checksum rides each one (`SlippiNetplay.cpp` 689-720, 1248-1316). The ping is the pad
  acknowledgement's round trip (400-441). The socket is marked DSCP EF (`IP_TOS 0xb8`, qWAVE on
  Windows; 979-1023).
- **Connecting.** Hole punching through its own UDP matchmaking server:
  - a fixed random port in 41000-51000, the one it talks to the server from
    (`SlippiMatchmaking.cpp` 316-334);
  - the server hands each side the other's public address, and the LAN address when the external
    addresses match (609-635);
  - both connect at once, accepting a connect by host alone for NATs that change the port
    (`SlippiNetplay.cpp` 98-148, 895-921).
- **What it lacks.** IPv4 only (`SlippiMatchmaking.cpp` 405-406), no encryption or peer
  authentication, and no relay or TURN: after 8 s it gives up and matchmaking looks for someone
  else (918-926). The escape hatch is a forwarded port (`SlippiPane.cpp` 92-95). The firewall
  prompt isn't avoided: it binds `ENET_HOST_ANY` on that port.

**What Orca does**, the same data path with standard ICE and the room as a live fallback:

- **ICE** is [libjuice](https://github.com/paullouisageneau/libjuice) 1.7.4 (MPL-2.0, used under
  its section 3.3 with Dolphin's GPLv2+; `Externals/libjuice`). It is what libdatachannel uses for
  WebRTC, without SCTP and DTLS. Candidates:
  - host candidates on every interface, IPv4 and IPv6 (one dual-stack socket);
  - the public address from STUN (`stun.cloudflare.com`, or the ticket's);
  - Cloudflare TURN over UDP from the ticket's `ice`, the credentials the browser SDK uses
    (YouGame `web/src/lib/turn.ts`; dev tickets carry them too).
  - Connectivity checks run both ways; peer-reflexive addresses cover port-changing NATs.
  - TURN over TCP or TLS isn't used: on a network that blocks UDP the room's relay is the path.
- **Signalling** is room peer messages (above), only between two Orcas that said hello with the
  same compatibility key and both offered links. The lower connection id offers (`dl` with its
  ICE description and host candidates); the other answers; candidates found later trickle in
  batches (`dlc`, at most 10 messages a second, under the room's 120).
- **Keys.** Each side sends 32 random bytes in its `dl`. HKDF-SHA256 over both (the lower id's
  first), salted with the room code, with info `orca-dl1`, the generation and both ids, gives one
  key per direction.
  - Every datagram is ChaCha20-Poly1305: the 18-byte header is the associated data, and the nonce
    is the sender's counter, which only grows under a key.
  - A 64-counter replay window drops repeats. A datagram that fails authentication is dropped
    silently and counted.
  - A packet must name the seat of the link's player (`DecodeDirectPacket`, as the relay checks)
    and carry no history.
  - Only the two Orcas of a pair can make a datagram the other takes. The room sees both halves,
    as it sees the keyframe key: the trust is the relay's, no more.
- **Datagram.** `'o'`, version and kind (packet or keepalive), generation, counter, the newest
  counter received from the peer and how long ago (in 0.1 ms), then the sealed payload and a
  16-byte tag. 1200 bytes at most: IPv6's minimum MTU less IP, UDP and a TURN channel header.
  - The payload is the packet's JSON without the drop-in history (the relay alone carries it, in
    order). A catch-up packet that doesn't fit keeps its oldest inputs.
  - The echo gives the round trip on the path inputs take, like Slippi's ping, with no shared
    clock. A keepalive goes out after 200 ms idle.
- **Sending.** `YouGameRoom::Send` queues the relay copy as before, and seals and sends the direct
  copy at once on the emulator thread, under DirectLink's own lock, never the room's, with no
  2 ms poll and no 10 ms send gap. Each link logs its sends' cost when it closes: 70-100 us on
  average on an M-series Mac (sealing, libjuice and `sendto`), rarely over 1 ms (1 in 3,500 in
  one run; a preempted thread), at most 4.5 ms seen. While a session stalls (it asks every
  millisecond), a direct copy goes out for new inputs, a checksum or a leave, else every 4 ms.
  Datagrams are marked DSCP EF as Slippi's are (not on Windows: libjuice can't, and qWAVE is a
  follow-up).
- **Both paths, always.** The relay still carries every packet. Whichever copy arrives first
  counts, so a link that fails or never opens costs only the relay's time, and switching either
  way needs nothing. Unlike the browser SDK (direct when open, relay otherwise), the inputs always
  take the faster of the two.
- **Links come and go** (one agent per pair, built and destroyed on DirectLink's own thread; the
  STUN and TURN names are resolved there first, so no teardown waits on DNS). libjuice nominates
  a pair once and calls a pair failed only after 30 s without consent, so Orca rebuilds:
  - when the agent fails;
  - when no answer comes within 5 s, or no path within 10 s of both descriptions;
  - when a link that was up gets nothing for 3 s while the relay still brings the peer's packets
    (a Wi-Fi/Ethernet switch, a DHCP renew, an expired NAT mapping);
  - once, 10 s after connecting, on a TURN or server-reflexive pair between two machines on one
    LAN (a macOS Local Network permission granted late): a host subnet in common AND a public
    address STUN saw on both sides (`OneLan`). A shared /24 alone isn't one LAN: two homes on
    192.168.1.x or Xfinity's 10.0.0.x would tear down a working cross-home link for a probe of
    the friend's private address on this side's own network.
  - Rebuilds for a failure back off 2, 4, 8 ... 30 s, and the backoff resets after 30 s up. The
    answering side asks for one with `dlr`; the offering side builds it (not twice in a second).
  - Each build is a new generation with fresh keys. TURN credentials older than 10 minutes are
    minted again first (`YouGameRoom::FreshTicket`): a friend may arrive long after the room
    opened. A mint that fails (it can hold the link thread 10 s) isn't tried again on every build:
    30 s, doubling to 5 minutes, while links keep the credentials they have.
  - One side may start over while the other keeps its count (a seat change, links switched off
    and on one side only): the answering side refuses an offer older than its generation and
    answers it with that generation (`dlr`, once a second at most); the offering side goes past it
    with its next build, half a second after its last. An answering side with no link at all asks
    with generation 0, which the offering side always takes.
- **Three or four players:** a link per pair, each falling back on its own.
- **Drop-in:** linking starts at the hello, in the lobby. A joiner's history comes through the
  relay, and its live inputs go direct as soon as the link is up.
- **A peer that floods.** Each link delivers at most 300 packets a second (a token bucket with a
  burst of 64), after authentication: an honest Orca sends one a frame, and while it stalls one
  every 4 ms. Past that the datagram is dropped and counted (its packet still comes through the
  relay), so a modified Orca holding the link's keys can't make `Receive` decode thousands of
  packets on the emulator thread. The first drop logs a warning; each link's close line counts
  them ("over the rate").
- **Switched off** (no link tried, every input through the relay, logged as
  `Orca room: direct links off (<why>): inputs go through the relay`):
  - by YouGame, for everyone: a ticket reply with `"direct": false` at its top level
    (`TicketAllowsDirect`; absent or not a boolean means on). Every ticket the room gets is read
    again (a fresh one for TURN credentials or the keyframe store too), so the switch also reaches
    a room that is already open, at its next ticket. A room whose first ticket says off never
    starts its link thread. The desktop app's page must pass `direct` through in its `mpTicket`
    reply (YouGame `web/src/components/Player.tsx`, which lists the fields it returns);
  - by the player: the app's `direct off` / `direct on` on stdin, with the `direct` cap ("Embedding").
    It holds for the run, and takes effect at the room's next tick (milliseconds): the links go,
    and `direct on` builds them anew.
  - `ORCA_DIRECT=0` (a test switch) builds no links and offers none in the hello.
  - While switched off an Orca still says in its hello that it can link, and ignores the other
    side's offers, so switching back on needs nothing from the other player.
  - All of these but `ORCA_DIRECT=0` are 0.3.9 on. Orca 0.3.8 (`da855207e2`, the first release
    with direct links) ignores the ticket's `direct` and has no `direct` cap (nor the flood limit,
    the mint backoff, the generation fix or `OneLan`). While 0.3.8 is the current release, links
    go off for everyone only by making another version current (0.3.7 is relay only).
- **Logs** never carry addresses, candidates or credentials: libjuice's own lines go to the
  NETPLAY log (never stdout, which the app parses) with addresses taken out.
  - Embedded in the app (`--embed`), the NETPLAY and ROLLBACK logs are on at INFO from 0.3.9
    (`MainNoGUI.cpp`; the packaged Logger.ini has both off at NOTICE), so the per-game log under
    the app's `orca/logs` has the room's, the links' and the match's lines. "seat N sends over 300
    packets a second" during a catch-up is expected.
  - NOTICE lines: `Orca direct link: seat N up, direct host/host, round trip 1 ms (link G)`,
    `... down (nothing for 500 ms; link G, 0 refused), inputs go through the relay`, each build,
    and each link's counts when it closes.
  - The session's stats line ends `link direct host/host 1 ms` (or `relay`).
  - libjuice names an unrelayed local side by its first local candidate (normally `host`, here and
    there `prflx`). The remote half tells a LAN path from a NAT's.
- **`orca stats`** gains `tx` (0 relay, 1 direct, 2 TURN: the worst of the friends' links) and
  `lrtt` (that link's median round trip, -1 on the relay). `ping` stays the relay's round trip.
  The desktop app passes only the keys it knows (YouGame `desktop/src/orca-policy.ts`
  `STATS_KEYS`), so the page sees these once it adds them.
- **The firewall.** The room opens a throwaway UDP socket on every interface the moment it is in
  the room, so a Windows Defender Firewall prompt (Windows asks on the first non-loopback bind of
  an exe it doesn't know) comes in the lobby, not when a friend arrives mid-game. Whatever the
  player answers, links should still form in most cases (not yet tried on the PC): ICE sends
  first from both sides, and a stateful firewall passes the replies. Windows ties the rule to the exe's path, and the app installs each
  Orca under its own version folder, so every release asks again. Slippi avoids that with one
  fixed install path, updated in place (slippi-launcher `mainline_installation.ts`).
  - On macOS, Orca.app is signed, so the Application Firewall lets it be. The Local Network
    privacy prompt (macOS 15+) goes to the launching app. Denied, LAN pairs fail and links take
    the router's hairpin or TURN instead.
- **Privacy.** ICE shows each player's LAN and public addresses to the others in the room, as
  Slippi does to opponents. A TURN-only option could hide them later.
- Not yet: playing on through a room disconnect while direct links are up (Slippi drops its
  matchmaking connection once matched); feeding the link's round trip into the delay rule (it
  still uses the relay's); a binary packet codec; thinning the relay copy.

Measured on the Mac (2026-10-03), two Orcas on one machine, Brawl, Mario vs Link fuzzed:

| Run | Path | Link round trip | Rollbacks | Stalls | Desyncs |
|---|---|---|---|---|---|
| Prod rooms (`xplat.py`, real tickets, 117 s together) | direct host/host all 117 s | 0 ms (relay ping 13-14 ms) | 0 and 0 | 0 | 0 |
| Same, `ORCA_DIRECT=0` | relay | - (ping 26-28 ms) | 7 host, 65 joiner | 4 (joiner) | 0 |
| Dev room (`direct-link.py`, 40 s of match) | direct | 0 ms | 0.00/s both | 0 | 0 |
| Same, friend `ORCA_DIRECT=0` | relay (negotiated off) | - (ping 27-35 ms) | 0.57/s host, 6.20/s friend | 0 | 0 |
| `ORCA_DIRECT_LOSS=20` both | direct | 0 ms | 0.00/s, 0.29/s | 0 | 0 |
| `ORCA_TEST_NET_DELAY_MS=30` both | direct | 121 ms (30 ms out and in on both) | 6.3/s both | 0-1 | 0 |
| `ORCA_DIRECT_FORCE=turn` both | TURN relay/relay (Cloudflare) | 23-24 ms (relay ping 12-14 ms) | 0.03/s both | 0 | 0 |

- The full `direct-link.py` run: the link was up 0.4 s after the friend plugged in.
  - `test-direct off`: both on the relay at once, no desync.
  - `on`: back to direct (after the silent-link rebuilds the relay's packets set off).
  - `in`: one way only.
  - Three rebuilds: links 4-6, each up in well under a second, 0 refused, 0 replayed.
- `dropin-commands.py` (join, leave, rejoin, the host leaving) and `dropin-rooms.py` pass with
  links on. The friend saw 0 rollbacks over 135 s with the host.

Tests:

- `OrcaDirectLink*` (in-process links over loopback: keys, datagrams, replays, rebuilds, three
  players, blocks, the test delay, a flooding peer, either side starting over, a failed
  credential mint).
- `OrcaRoom.TicketSwitchesDirectLinks` (the ticket's `direct`).
- `OrcaSessionDirect*` (duplicates and reordering in the session, a drop-in friend and a
  wall-clock match beside the relay).
- `ORCA_LIVE_TEST=1` `OrcaLive.DirectLinkBetweenTwoRooms` (two rooms on yougame.co, the player's
  switch included).
- `Tools/orca/direct-link.py <nogui> <disc>`: two Orcas over a dev room, a match, `test-direct`
  off/in/on, three rebuilds and the player's switch (`direct off`, then `on`, on the host only).
  `--both ORCA_DIRECT_FORCE=turn --expect turn` and similar run the variants; `--ticket-direct off`
  fetches both Orcas' tickets through a local proxy that adds `"direct": false`. `Tools/orca/xplat.py` passes the `ORCA_DIRECT*` switches through and reports the
  transport.

### TURN only when needed (Orca 0.3.21)

A link starts with host and STUN candidates only. One that never came up (`no path`: answered but
not connected in CONNECT_TIMEOUT; or ICE `failed` before it was ever up) marks the pair
`with_turn`, and the next link to that peer gathers the ticket's TURN server too. The offering side
decides: its offer (`dl`) carries `"t": 1` and the answering side's agent follows it; the answering
side's request for a new link (`dlr`) always says `"t": 0|1`, and the offering side takes that. A
link that worked and then failed (consent lost after 30 s, a socket error) or went quiet (`silent`)
keeps going without TURN, and an `upgrade` to the LAN clears the flag on both sides, so a pair that
needed TURN once (a firewall prompt still open) is not left with an allocation beside a host/host
path. Until a TURN link is up the room's relay carries the inputs, as for any link that is down.

Why: on 2026-10-05 Luis's PC (Realtek RTL8822BE) dropped off the air for 100-400 ms every 5.0 s for
as long as a direct link with a TURN allocation lived, and the Mac stalled on its inputs each time
(42-57 stalls in 160 s at delay 2). The same direct link without TURN (`ORCA_DIRECT_TURN=0`, R11):
4 stalls, PC router ping 0.5% bad with no beat; with TURN (R6) 7.9% bad on the beat. Most pairs
connect host/host or through STUN and never needed the allocation; it also costs Cloudflare TURN
traffic for nothing.

### Wi-Fi

A machine whose Wi-Fi radio leaves the air loses or delays everything sent to it, and everything it
sends, for that long. Rollback hides up to `input_delay + max_rollback` frames of it (167 ms at
3 + 7); past that the other side stalls, and a stall is a dropped frame on its screen (fps 57-58
over the second it happened in). Orca's build has nothing to do with it, and no build fixes it.

Found 2026-10-05 (Luis: "significantly more stutters and frame drops today than yesterday", Mac and
PC on one LAN, both on Wi-Fi on 5 GHz channel 36): five xplat rounds, Mac hosting, PC joining, two
on 0.3.13 and three on 0.3.18, all alike. Each lost 0.9-1.6% of the PC's datagrams on the way to
the Mac (the Mac's socket buffers dropped a few dozen at most) and stalled the Mac 36-54 times in
about 160 s at delay 2; the Mac's to the PC lost 0-0.15%. The Mac's per-second stats (xplat's
`stats.jsonl`) put every stall on a five-second beat, the same beat as the PC's router ping
(100-400 ms replies or timeouts every 5.0 s under traffic, 1% lost), while the Mac's own router ping
stayed clean (2 of 40 stall seconds near a blip, against 5 of 159 seconds overall). The PC's radio
(Realtek RTL8822BE, a Wi-Fi and Bluetooth combo card) has periodic dead time; the night before, the
same pair lost 0.05-0.1%.

- Turning the PC's background scans off and media streaming mode on through the WLAN API (what
  Steam's streaming and Moonlight do) changed nothing on that card: same spikes, same rate
  (2026-10-05; the code is on the unmerged `q8-wifi` branch).
- A Mac's own off-channel time is AWDL (AirDrop, Handoff, Sidecar, Universal Control, AirPlay); it
  was not the cause here, but it is the first suspect when a Mac's router ping spikes.
- **What fixes it is the player's:** a cable on the machine whose radio drops out (on either one,
  it takes that radio off the path); then Bluetooth off, or the adapter's power saving off
  (Windows: the power plan's Wireless Adapter Settings, the driver's own Advanced settings), or
  another channel or band. Both machines on Wi-Fi on one channel is the worst case.

Later the same night (rounds R6-R9 and T-tests, PC router ping at 10 Hz throughout) narrowed it
further: the PC's dead time comes only with a direct link. A match on 0.3.19 over a direct link
showed 7.9% bad pings on a strict 5.0 s beat starting within 0.2 s of the link coming up (the beat
keeps its phase for minutes, so it is no libjuice timer: consent checks are a random 4-6 s); the
same match relay-only (`ORCA_DIRECT=0`) 0.1% and no beat. Orca alone with no room: clean. Plain UDP
at Orca's rate and sizes from the PC, to the PC (up to 250/s, EF or not), and both ways on one
socket with EF echoes: all clean. What is left is libjuice's own work on the PC (its ICE checks,
consent, the STUN binding and the TURN allocation, its socket); `ORCA_DIRECT_ICE_LOG`,
`ORCA_DIRECT_STUN=0` and `ORCA_DIRECT_TURN=0` exist to take that apart. Separately the PC (or its
router) drops everything for about 3 s once every 3616 s (seen at 20:44:01, 21:44:17, 23:44:49 and
04:46:10), which fits an hourly group-key rekey a driver handles badly.

To find whose radio it is: run xplat on both sides with a 10 Hz router ping on each machine (macOS
`ping -i 0.1 --apple-time <gateway>`; Windows a `System.Net.NetworkInformation.Ping` loop with
timestamps) and line the stall seconds in the stalling side's `stats.jsonl` up against each ping's
spikes and timeouts. Stalls follow the radio that drops out.

## Input delay

`Orca/Session/Session.cpp` ("Input delay") has the rule; it changed in 0.3.5, and again for drop-ins
and spiky links on branch `delay-dropin` (2026-10-02 and 03).

- Two frames by default, like Slippi. Rollback hides the rest of the link, jitter and spikes
  included. A spike past the rollback window costs a stall there, not a frame of delay on every
  input all match long.
- The adaptive delay rises only for what rollback can't cover, and only where a little delay cures
  it:
  - **The typical link.** It is the median of the player's latest 15 round trips to the relay (one a
    second) and the slowest friend's median, which each reports. Through the relay, one way is half
    of each. The delay covers what the 7-frame rollback window leaves of that, keeping a frame for
    the boundary and one for jitter: two frames cover a round trip of about 240 ms between the
    players. A new session takes the room's recent round trips at once, and the delay rises as soon
    as every player's median is in. One side's alone sets nothing. It rises once the link is 4 ms
    (one way) past a frame's boundary and comes back once it is 4 ms short of it, so a median that
    wanders around a boundary doesn't flip it up and down.
  - **A friend's stalls that a little more delay would have spared.** A friend stalls when this
    player's inputs outran its rollback window, so only this player's delay can help. Each side
    times its own counted stalls (until the input it waited on came, not to the end of the
    rollback that input sets off, whose re-run would make a stall look a frame longer) and
    reports, with their count, how many were over within one, two, three and four frames
    (`Packet::spared`, `"sp"` in the room message): k frames more of the other's delay would have
    brought the input it waited on k frames sooner, and spared it. Each
    second gets a need: the least delay that would have kept the friend out of every stall that
    second (and, with none, how near the edge of its window it ran). A raise of k frames is worth
    it once it would have spared stalls in 8 seconds per frame of the last 45 (none from before the
    last step down): a frame on every input from then on, against a stall saved every five or six
    seconds. The delay then rises a frame, and the next second judges the next frame on the same
    seconds.
    - Lateness a frame or two cures raises within half a minute or so of its start: a slow, bursty
      uplink (inputs a frame late in bunches every few seconds, which the round-trip median doesn't
      show), or dropouts just past the window every two seconds.
    - Spikes at random that outlast the window by anything from under a frame to six (the Mac's
      bad-day Wi-Fi: 80-220 ms, every 2.7 s on average) raise nothing: a frame more would spare only
      the shortest, and a chance bunch of those seldom reaches 8 in 45 s. Nor do long dropouts every
      second (150-300 ms): a raise to 6 would spare fewer than half of them. Those cost their
      stalls, as they would in Slippi.
    - A friend's stall counts only if its lag on this player's inputs (below) reached the window
      around then (with three or four players, it may have waited on someone else). It doesn't
      count:
      - around a hitch on either machine;
      - when this player's own stall set it off: the friend waited on inputs this player wasn't
        sending, because it was stalled itself (a stall of this player's heard of during the
        friend's or in the half second before it);
      - while a controller isn't plugged in yet (catching up), or while waiting for the first
        packet;
      - in the first 10 s after the newest plug-in, on both machines: the start of a match, or a
        drop-in, where the joiner plugs in up to a dozen frames behind the host, the host stalls on
        its first inputs, and time sync evens the two out.
    - A friend on an older Orca reports no lengths: each of its stalls counts as one a frame would
      have spared.
- Rollbacks never move the delay, however many there are, and neither do time-sync waits.
- A raised delay comes back down a frame at a time. One the link raised follows the link down after
  3 s of wanting less (a first median inflated by a keyframe's transfer). One that stalls raised
  steps down after 5 quiet seconds, then a frame each further quiet second, while there is room: in
  the last 5 s, no second where a frame less would have stalled the friend, run it at the edge of
  its window or within a frame of it, and at most one where it stalled even at this delay (a spike
  the delay could not have spared either). Each packet shows how far the friend ran past this
  player's newest input it had (its frame minus its acknowledgement); that plus the delay this
  input carried (the reach) doesn't depend on the delay. Two frames of room, not one: what a delay
  sees runs a frame or two short of what the delay below sees once the friend stalls there again
  (dropouts every 2 s showed a frame of room at 2 from 3, and stalled again at 2). A step down
  spends what the seconds before it said, so the next raise needs fresh ones. Stalls that raise it
  again within a minute undo the step and double the next quiet wait (up to 40 s); a step that
  holds a minute halves it again. After a minute of a slow, bursty uplink, the delay is back at 2
  about 10 s after the link is clean.
- Each change is logged with its reason and the numbers behind it, for instance `Online match: input
  delay 2 -> 3 at frame 5400: 1 frame more would have spared the others' stalls on this player's
  inputs in 8 of the last 45 s (the delays those seconds needed: 3 3 4 3 7 3 3 3 3)`. The
  1800-frame stats line also counts the stalls a frame more would have spared, and it and
  `xplat.py` show the round-trip medians the link part uses (`orca stats` `pmed` and `fpmed`).
- The app's `delay N` fixes it instead (Embedding, stdin).
- Since the SI relatch ("Input latency"), a pad sampled for frame f is what the game reads in frame
  f, so delay N is N frames, as in Slippi. Before it the game read each frame's pads a frame late,
  and delay 2 acted like 3.
- **Round trips are per side.** `ping` in `orca stats` and "round trip" in the stats line are the
  latest single round trip from this machine to the relay (YouGameRoom's ping every second), not
  between the players: one way between them is about half the sum of the two. Each machine measures
  its own leg, so the two can differ a lot. In the 2026-10-02 drop-ins the PC read 14-23 ms in every
  run, while the Mac on Wi-Fi (AWDL active) read 15-17 ms with occasional 79-117 ms spikes on a good
  run and 30-155 ms on a bad one, at a load of about 25 in one of them. The same thread carries the
  game's packets, so whatever delays the pong delays the inputs too. `xplat.py`'s `ping_avg`
  averages those single samples, so spikes pull it up; the medians are `ping_median` and
  `friend_ping_median`.
- Why (2026-10-02, a Project+ match, 0.3.5): a Mac at 34-35 ms whose Wi-Fi spiked to 113-123 ms
  every ~11 s, against a friend at 22 ms whose game hitched about as often. In menus both sat at 2
  with no rollbacks. In every match the old rule climbed to 4-5:
  - A second with 30 re-run frames, or two stalls in three seconds, on either side raised both
    players. The stalls came from the spikes and from the friend's hitches.
  - A lower needed ten seconds of at most three re-run frames per second. A match never has those,
    so the delay came down only between matches.
  - At 4-5 the rollbacks barely moved: 20-30 per 15 s, 3-4 frames deep, against 59 at 2.
- Why (2026-10-02 evening, `delay-dropin`): three Mac and PC drop-ins over the prod relay on clean
  PC links. In two, the PC's delay went from 2 to 5 within 30 s of plugging in and both sides
  hovered at 4-6 for the whole match; in the third both held 2. The drop-in wasn't the cause (the
  0.3.5 and 0.3.6 rules are the same). The Mac's own link was: in the bad runs its spikes past the
  rollback window came every few seconds (ping average 49-70 against 25-33), and each stalled both
  sides once. 0.3.5 read three stalls in 10 s as sustained, raised the other side's delay, cleared
  the history and could raise again at once, while a step down needed 10 quiet seconds per frame.
  Two more flaws broke the bar without driving those runs: the first-seconds allowance was timed
  from the keyframe, so a joiner's ran out during its catch-up, before it plugged in; and a stall
  this player's own stall set off counted toward this player's delay.
- Why the stall lengths (2026-10-03, review of `delay-dropin`): its first fix raised only on stalls
  in 3 of any 4 seconds. That held 2 on spikes at least 2 s apart, but missed both ways. Lateness
  that stalls in at most 2 of every 4 seconds never raised it however long it lasted: a slow,
  bursty uplink (90 ms + 0-120 ms, in order) stalled the PC 89 times in 5 minutes at 2, where 3
  would have left 6-7; dropouts of 130-150 ms every 2.1 s kept both at 2 with 80-120 stalls a side.
  And spikes at random rather than spaced bunched into 3 of 4 seconds now and then, so the delay
  went up and down 4-14 times in 5 minutes. A count of stalled seconds can't tell those links apart
  (each stalls in 5-11 seconds of every 30); how long each stall lasts can: the uplink's and the
  dropouts' are over within a frame or two, the random spikes' anything up to six. `SessionTest`
  covers each:
  `SpikesAtRandomKeepTwo` (16 draws: no change), `SpikesAtRandomEveryTwoSecondsRarelyMove`,
  `ClusteredLatenessRaisesTheLateSide`, `SustainedLatenessRaisesThenComesBackDown`,
  `DropoutsAFrameWouldSpareRaiseAndStay`, `LongDropoutsEverySecondRaiseNothing`,
  `StallsWeighByWhatARaiseWouldSpare` and `StallsARaiseWouldSpareRaiseThenDecay`; the spaced
  spikes (`SpikesEveryFewSecondsKeepTwo`), drop-ins on a clean link at 10, 30 and 75 s
  (`DropInOnACleanLinkKeepsTwo`) and `StallsSetOffByTheOtherSidesStallCountForNothing` still hold.
  Over 16 simulated draws each: random spikes every 3.5 or 2.7 s on average, the Mac hosting or
  joining, never moved either delay (every 2 s on average, one up and one down on a side on 1 or
  2 draws in 16); the bursty uplink raised the Mac's to 3 within 31-122 s and the PC stalled 12-32
  times in 5 minutes; the dropouts raised the Mac's to 3 and the PC's to 4 within 26-53 s, for
  good. With re-run frames of 2 ms each (12 draws): the uplink raised within 37-85 s, the dropouts
  within 28-59 s, and random spikes every 3.5 or 2.7 s moved a delay up and back down on 1 draw
  in 12 of some of the four cases.
  `Tools/orca/dropin-delay.py` checks it with two Orcas over a real room; `ORCA_TEST_NET_SPIKES`
  (now also `~M` for gaps at random) and `ORCA_TEST_NET_UPLINK` (Profile.h) give one of them the
  test link. On one Mac over the prod relay (2026-10-03, dev rooms):
  - Joins at character select and mid-match held 2 on both sides for 5 minutes in Brawl and
    Project+: 0 desyncs, 0-5 stalls a side, none counted.
  - The friend's room on spikes at random (`~2700/80-220`) held 2 on both sides for 5 minutes in
    both games: 63-69 stalls a side, of the counted ones 10-14 short enough for a frame to spare,
    0 desyncs.
  - A slow, bursty uplink on the friend (`ORCA_TEST_NET_UPLINK=90+120/60-210`, Brawl, two runs)
    took the friend's delay to 3 22 and 67 s into it, the host having stalled about 10-15 times
    by then (in the second run 17 of its 20 counted stalls were over within a frame), and back to
    2 7 s after it ended. The host's stayed at 2. A first build timed stalls through the rollback
    their input set off: only 8 of 14 counted stalls, nearly all 3-16 ms long, came out as over
    within a frame, and nothing rose.
  - Dropouts of 130-150 ms every 2.1 s for 110 s (Project+) took both to 3, 68 and 83 s in, and
    back to 2 around their end.
  - The loopbacks (`YG_LOOPBACK`, fresh recordings, 8,900 frames) PASS in both games.

  Two Orcas on one Mac's Wi-Fi are a noisy link on some runs (round trips of 100-370 ms every few
  seconds on one of the two connections): compare runs only with their `pmed` and `ping`.

## Input latency

What happens between a press and the frame that shows it, and what Orca does about each part
(latency-r1, 2026-10-02). Measured with the latency probe (`Tools/orca/latency-probe.py` on the
`lat-probe` branch): a scripted press, the first emulated frame whose image differs from a run
without it (frame dumps), and timestamps on one clock from the frame boundary that sampled the pad
to the present call and to CoreAnimation's `presentedTime`. Solo, real-time pacing, Metal, a visible
871x434 window on the M4 Pro's 120 Hz screen. Online adds the input delay on top (2 frames by
default, 16.68 ms each).

| Change                          | Where                                   | Emulation   | What it does                                                                                       |
| ------------------------------- | --------------------------------------- | ----------- | -------------------------------------------------------------------------------------------------- |
| Immediate XFB                   | `Orca/Profile.cpp`                      | no          | Each frame is presented at its XFB copy instead of 18 ms later at the VI field that scans it out.  |
| VSync off                       | `Orca/Profile.cpp`                      | no          | Forced, so a `GFX.ini` can't add 2-19 ms or let the screen pace the game.                          |
| Smooth Early Presentation       | `Orca/Profile.cpp`                      | no          | Presents evened out for about 2 ms: a 60 Hz screen repeats and drops a third as many frames.       |
| No re-present on rollback loads | `VideoCommon/Present.cpp`               | no          | A rollback no longer flashes the snapshot's (stale) frame or spends a present inside the rollback. |
| Slow screens drop frames        | `Metal/MTLGfx.mm`                       | no          | A screen under 59 Hz no longer slows the game to its own rate.                                     |
| Thread priority                 | `Orca/ThreadPriority.cpp`               | no          | The CPU, room and controller-stream threads run at interactive priority.                           |
| Brawl's input lag fix           | `Data/Sys/Orca/RSBE01.patches`          | yes (Brawl) | Vanilla Brawl copies this frame's pads, not last frame's: its image comes a frame sooner.          |
| SI relatch                      | `HW/SI/SI.cpp`, `Rollback/Rollback.cpp` | yes         | The game reads the pads the frame hook just set, not the previous hook's: a frame sooner.          |

The two that change emulation change the lobby compatibility key (the patch file's hash, and the git
revision every build carries), so builds with and without them never meet.

### Presentation (host only)

- **Immediate XFB** (`GFX_HACK_IMMEDIATE_XFB`, forced on). Dolphin otherwise shows each frame at the
  VI field that would scan it out on a Wii: Brawl and P+ copy their XFB about 15.8 ms into a frame
  and the field starts 18 ms later. Project+'s and Slippi's Dolphin ship it on. Why it can't desync:
  `ImmediateSwap` is already skipped on re-run frames; the VI-end throttle it bypasses only sleeps
  (VI skip is off under `WantsDeterminism`); `SyncGPU` does nothing in single core. Measured:
  per-frame RAM hashes identical to it off over whole scripted matches, Null and Metal. Brawl and P+
  make one XFB copy per frame, so `CapImmediateXFB` stays off. It also shows the corrected frame
  after a rollback at once, where the VI path showed up to two mispredicted copies.
- **VSync** forced off. A composited window never tears, so VSync only adds latency (2-19 ms at the
  glass) and, on a screen slower than 59.94 Hz, would let `nextDrawable` pace the game.
- **Smooth Early Presentation** (`MAIN_SMOOTH_EARLY_PRESENTATION`, forced on). Immediate XFB
  presents each frame the moment its copy is done, and Brawl's and P+'s copies land unevenly in the
  frame, so on a 60 Hz screen 4-6% of the refreshes repeated a frame and 5-7% of the frames never
  showed. Smooth presents at the VI time plus an offset that follows the earliest arrivals slowly
  and the latest quickly: about 2 ms later on average, a third as many repeats and drops
  (Measurements). Its offset jumps late when a frame comes late, so the worry was stalls and their
  catch-up; but a session never makes a stall's time up (`Session.cpp` resets the pacing when a
  stall or a wait ends), so after a stall there is only the rollback's re-run, and the frames right
  after a stall cost the same 2 ms as any other. Rush Frame Presentation stays off: it gains
  nothing over Immediate XFB and puts each rollback's re-run time straight onto the frame's
  present.
- **No re-present on rollback loads.** `Presenter::DoState` re-showed the loaded state's last frame
  on every load; for a rollback that is a stale frame (often the one before: a one-frame backward
  flicker) plus a present and a `nextDrawable` inside the rollback. Rollback snapshots now skip it
  (`VideoCommon_LoadRedisplays`); savestates and drop-in keyframes (`SnapshotRing::LoadImage`) still
  show their frame. Host only.
- **Rollback and the EFB.** Rollback snapshots leave the EFB alone (no readback per save, no upload
  per load), and re-run frames draw nothing and skip their EFB clears. The GPU's frame straddles the
  frame boundary: at the SI poll Brawl and P+ are 170-350 draws past their last XFB copy (80% of
  Brawl's synctest loads, 98% of P+'s), so the frame shown after a rollback is the abandoned
  timeline's draws before the boundary and the corrected timeline's after it. Clearing the EFB on
  re-run frames would blank that first part, so it stays. What went wrong was the batch still
  buffered in the vertex manager at the boundary (23% of Brawl's loads, 55% of P+'s; anything from a
  4-vertex HUD quad to a 1,200-index character mesh): `VertexManagerBase::DoState` flushed it after
  `VideoCommon_DoState` had restored the snapshot's XF matrices, projection and TEV state, so it was
  drawn through the wrong position matrices (solid triangles fanning out from a character for
  exactly one frame, as hits land: a P+ player on Windows, 2026-10-05) and was missing from the
  frame it belonged to. Loads now flush it first, under the registers it was buffered with. Proof:
  `YG_SYNCTEST=7` on Metal with `ORCA_TEST_PRESENT=1280x720` and a shot every 10 frames, against the
  same inputs without synctest: P+ 46 of 70 shots differed outside the stats line before (missing
  damage digits, stray glyphs), 0 after; Brawl 29 of 90 before (white and orange spikes shooting out
  of Mario and Samus), 0 of 91 after. Host only: no RAM change.
- **Slow screens** (Metal). With VSync off `nextDrawable` only waits for a free drawable, but a 50
  Hz screen frees one every 20 ms: the game waited ~17 ms every frame and ran at 50 fps (83% speed),
  dragging an online friend down too. Drawables now come from a background acquire, and a frame
  without one isn't shown, while the window's screen shows under 59 frames a second
  (`NSScreen.maximumFramesPerSecond`, read by the frontend every main-loop pass), or after 8 of 16
  `nextDrawable` calls waited 4 ms or more. Each has its way back: the window on a faster screen;
  120 background acquires in a row on time. A covered window behaves as before.
- **Thread priority.** The CPU thread (emulation, the frame hook, snapshots and re-runs), the room's
  network thread and the controller stream get `QOS_CLASS_USER_INTERACTIVE` on macOS, and on Windows
  a raised priority (above normal for the CPU thread, highest for the two network threads) with
  power throttling off. Only in a session; a scripted harness run keeps its CPU thread as it was
  unless something paces it (an online match, `YG_THROTTLE=1`). `ORCA_THREAD_QOS=0` turns it off,
  `=1` forces it on.

### Audio (host only)

- **The buffer** stays at Dolphin's 80 ms (`AudioBufferSize`, not forced). At 80 the mixer's
  queue holds 38-52 ms of sound (the setting is its ceiling, not its level); Cubeb adds 10.7 ms and
  the device its own, so sound trails the picture by about 50-65 ms. A smaller buffer starves: the
  game hands the mixer a frame's audio in one burst, a rollback's re-run holds the next burst back
  by up to ~15 ms, and a queue that overflows its ceiling is cut to half of it. In real-time
  loopbacks with rollbacks, 80 ms gave 0-1 underflows (a gap the mixer fills by looping) in a
  minute of match, 56-64 ms 1-3 for 0-10 ms less delay, 40-48 ms 5-32, 32 ms 174 (Measurements).
  A stall starves any size: no frames, no sound.

### Emulation

- **Brawl's Controller Input Lag Fix [Magus]** (`RSBE01.patches`, "Input"). Brawl's pad code at
  0x8002AD88 copies each pad's status for the game from the array at +0x444, a frame old; the
  community code (in Dolphin's own `GameSettings/RSBE01.ini` netplay set, and in Project+'s codeset)
  makes it copy from +0x40. Orca writes it in place, as 0x404 off the copy's 22 load displacements,
  one guarded group of 45 words that lands once at boot. Project+ already runs it, so
  `PPLUS32.patches` doesn't carry it.
- **SI relatch.** The game reads its pads a few ms into each frame from the SI input buffers, which
  the SI's last poll filled just before Orca's frame hook. So every frame played the previous hook's
  pads, a whole frame that stock Dolphin and Slippi don't have ("delay 2" acted like 3). At the end
  of the frame hook, on every pass, `SerialInterfaceManager::RelatchInputs` writes each channel's
  input buffer from the pads just set, as a poll would, but leaves the status register alone: no
  RDST bit, no interrupt, no host input update. The game's read takes the buffer only when the poll
  before it set RDST, as it had, so it now takes this frame's pads, and nothing else changes. Why it
  is deterministic: a pure function of the session's pads, applied after the boundary's snapshot was
  saved or loaded, so a snapshot holds the poll's latch and every load relatches. Scripted inputs
  land a frame earlier relative to the game; they count frames from scene entry, so the scripts
  still reach the same scenes.

### Measurements

Medians over 82 presses (two runs of 41 jumps each, Mario on Battlefield; P+ the same), M4 Pro
built-in 120 Hz screen, 1-minute load 3.6-6.7 (other agents' emulators sometimes alongside).
"Sample" is the frame boundary that sampled the press; "boundary" the one that starts the frame
whose image first changed.

| Build | Brawl: image at | sample -> present | boundary -> present | sample -> glass |
|---|---|---|---|---|
| 0.3.5 (`d9ed99f137`) | press+4 | 100.95 ms (6.05 fr) | 34.28 ms (2.06 fr) | 104.4 ms |
| + presentation and threads | press+4 | 79.69 ms (4.78 fr) | 13.03 ms (0.78 fr) | 85.2 ms |
| + Brawl's input lag fix | press+3 | 62.88 ms (3.77 fr) | 13.31 ms (0.80 fr) | 68.9 ms |
| + SI relatch | press+2 | 46.62 ms (2.79 fr) | 13.52 ms (0.81 fr) | 52.8 ms |
| + Smooth Early Presentation | press+2 | 48.89 ms (2.93 fr) | 15.54 ms (0.93 fr) | 52.9 ms |

| Build | P+: image at | sample -> present | boundary -> present | sample -> glass |
|---|---|---|---|---|
| 0.3.5 | press+3 | 83.73 ms (5.02 fr) | 33.63 ms (2.02 fr) | 86.9 ms |
| + presentation and threads | press+3 | 65.59 ms (3.93 fr) | 15.33 ms (0.92 fr) | 69.5 ms |
| + SI relatch | press+2 | 46.37 ms (2.78 fr) | 13.67 ms (0.82 fr) | 51.7 ms |
| + Smooth Early Presentation | press+2 | 48.90 ms (2.93 fr) | 15.52 ms (0.93 fr) | 52.9 ms |

- On glass depends on the compositor's state (about 4 ms or about 19 ms after the present, which
  Orca doesn't choose). With Immediate XFB alone the present cadence was less even: 30-38% of
  frames off the two-refresh rhythm against 20-23% before; with Smooth Early Presentation 10.7-12.5%
  (the last rows: a probe build of `dae32cfe4f`). Smooth costs 2.3-2.5 ms to the present
  call, but on glass about nothing at the median (52.9 against 52.8 and 51.7 ms) and less at p90
  (58.5 and 55.8 against 61.1 and 59.2): presents that come evenly catch the compositor's earlier
  slot more often (GPU done -> glass p90 6.4 and 4.7 ms, against 12.0 and 9.5).
- Smooth Early Presentation in rollback play, with stalls: real-time loopbacks (`YG_LOOPBACK`,
  `YG_THROTTLE=1`, input delay 2, the remote 6-9 frames late, so 4-7 frame rollbacks: Brawl ~890
  and P+ ~550 per run) with a 150 ms link spike every 4 s (`YG_LOOPBACK_SPIKE=4000,150`: 30 and 19
  stalls of ~105 ms), Metal in an 871x434 window, two runs each way per game. "60 Hz" is a
  simulated 60 Hz screen showing the latest finished frame at each refresh (GPU-done times):

  | | Brawl off | Brawl on | P+ off | P+ on |
  |---|---|---|---|---|
  | 60 Hz repeats / drops, 1.5 s+ after a stall | 4.0-4.1% / 4.7-4.8% | 0.7-0.8% / 1.5-1.6% | 6.0-6.1% / 6.8% | 2.1% / 2.9-3.0% |
  | the same, first 0.5 s after a stall | 3.6-5.5% / 7.3-8.3% | 1.5-1.9% / 5.1-5.2% | 5.9-6.8% / 9.1-9.6% | 3.3% / 6.1-6.2% |
  | off the 2-refresh rhythm (120 Hz glass), first 0.5 s | 29-32% | 13-24% | 30-43% | 14-36% |
  | sample -> present, 1.5 s+ after a stall (p50) | 79.0-79.2 ms | 81.4 ms | 79.2 ms | 81.1 ms |
  | the same, sampled 1-15 frames after a stall (p50) | 77.8-79.9 ms | 81.2-81.6 ms | 78.4-80.6 ms | 79.5-81.7 ms |
  | stall end -> first present call (p50) | 22.4-30.1 ms | 25.1-32.3 ms | 26.2-26.4 ms | 26.6 ms |

  (Sample -> present here includes the 2 frames of input delay.) There is no catch-up after a
  stall: the session resets the pacing when a stall ends, the late inputs cost one rollback (4-5
  re-run frames, none of them rendered), and the next frame runs on time. So Smooth's slow-moving
  offset has nothing to fall behind on, and the frames after a stall pay its ~2 ms like any other.
- Audio (`AudioBufferSize`, the probe's `AQ` lines: the DMA mixer's queue at each dequeue and
  its underflows), Cubeb at volume 0, Null video, real time. Loopbacks as above with a 150 ms
  spike every 10 s, the match seconds without a stall (Brawl 54, P+ 45):

  | Buffer | Brawl: queue mean, underflows | P+: queue mean, underflows |
  |---|---|---|
  | 80 ms | 51.7 ms, 0 | 51.6 ms, 1 |
  | 64 ms | 42.3 ms, 3 | 51.1 ms, 2 |
  | 56 ms | 41.3 ms, 3 | 43.2 ms, 1 |
  | 48 ms | 37.0 ms, 11 | |
  | 40 ms | 32.3 ms, 5 | 31.0 ms, 32 |
  | 32 ms | 22.5 ms, 174 | |
  | 24 ms | 15.4 ms, 648 | |

  The seconds with a stall had 19-34 underflows at 56-80 ms and 38-233 below. Solo (the scripted
  match in real time, no rollbacks): 80 ms 41.4 and 38.4 ms, no underflow in 72 and 57 s of match;
  64 ms 30.0 and 30.9 ms, 1 and 0. Input rate 32 kHz, a granule step 4 ms.
- On Luis's 49.95 Hz LG UltraWide (Brawl, before vs after the presentation changes): the game ran at
  50 fps (a boundary every 19.96 ms, `nextDrawable` waiting 15.7 ms each frame); now at full speed
  (16.70 ms), 2.5% of the screen's refreshes repeat a frame. Sample -> present 136.8 -> 79.8 ms,
  sample -> glass 195.7 -> 127.8 ms (that screen adds 39 ms after the GPU, at any setting).
- SI relatch: in every scene of both games (boot, menus, character and stage select, match) 100% of
  the game's pad reads took the pads of the hook just before them (before: 2-49% in Brawl's menus
  and 4% in a match), every read found RDST set, and there was still one read per frame.
- Thread priority, measured with the frame meter's own number (boundary-to-boundary host time,
  `YG_FRAMETIME`), one minute of a real-time Brawl match in a window, Orca and 10 or 12 busy loops
  all at nice 10, four pairs: p99 19.7 / 20.6 / 25.2 / 25.2 ms with it against 29.0 / 20.8 / 26.6 /
  24.7 without, and the worst frame 98-106 ms against 121-158 ms. No consistent p99 gain on an M4
  Pro at this load, a smaller worst case; the case it is for (the app frontmost, Orca's threads on
  efficiency cores, a Windows PC with EcoQoS) is not measured.

Checks:
- Presentation and threads: per-frame RAM hashes identical to 0.3.5 over whole scripted matches of
  both games (Brawl 10,807 frames, to the results screen and back to character select; P+ 9,000),
  Null and Metal (in a window).
- No re-present on rollback loads: a loopback session with 66 rollbacks presented 66 frames more
  than its first-pass frames on 0.3.5, and none more with the change.
- Input lag fix alone: the same sync test on Brawl, 0 RAM mismatches; P+'s per-frame hashes
  unchanged by it (its patch file isn't P+'s).
- Input lag fix and relatch: whole scripted matches of both games still reach the results screen and
  character select again (scene changes move by 1-9 frames; the scripts count from scene entry and
  needed no change); sync tests (rewind 7, boot through menus, character and stage select into a
  match; Brawl frames 300-4,500, P+ 150-3,000) with 0 RAM mismatches; real-time loopback sessions
  (`YG_LOOPBACK` with `YG_THROTTLE=1`: Brawl 289 rollbacks and P+ 387, deepest 6, 479 and 379
  checksums matched), where every rollback relatches the remote's corrected pads over mispredicted
  ones; drop-in keyframe tests at Brawl frames 700, 1,500, 1,700, 2,500 and 4,000 and P+ 250, 400
  and 1,500; a two-Orca drop-in over dev rooms (`dropin-commands.py`, both games: joins, leaves,
  rejoins, every checksum matched). The relatch's evidence is those 0 RAM mismatches and the
  loopbacks. The sync test's per-section device-state diff (`YG_SYNCTEST_SECTIONS`) can't show what
  the relatch wrote: snapshots are taken before `RelatchInputs`, and the next poll overwrites
  `in_hi`/`in_lo` before the next one. (No section differed on more re-runs than without the
  relatch, the SerialInterface's included, which says only that much.)
- P+'s keyframe test at frame 130 (the boot's save prompt) fails its re-run on 0.3.5 too (9 of 30
  frames): not from these changes.
- Smooth Early Presentation (host only), on the merge with 0.3.6 (`dae32cfe4f`): per-frame RAM
  hashes of both whole scripted matches identical to the build without it (`a7a2d51a99`: Brawl
  10,811 frames, P+ 9,009 in common), and with Cubeb audio (volume 0) identical to without.
- Slow screens: a whole Brawl scripted match in real time in a Metal window, the frontend reporting
  a 50 Hz screen (`YG_LATPROBE_SCREEN_HZ=50`, probe build), so drawables came from the background
  acquire throughout: per-frame RAM hashes identical to a Null run over all 10,811 frames.

### Re-measuring

```
# A probe build of any commit: cherry-pick the probe hooks (lat-probe) onto it, build, then
Tools/orca/latency-probe.py run --out /tmp/lat --bin <probe build> --press jump \
  --configs shipped --repeat 2 --hold-lock
Tools/orca/latency-probe.py run --out /tmp/lat-pp --bin <probe build> --game pplus --press jump \
  --configs shipped --repeat 2 --hold-lock
# the second screen: --screen 1

# Rollback play with stalls: record a plain run, then replay it through a session in real time,
# in a window, with a 150 ms link spike every 4 s from frame 2400 (probe build, YG_LATPROBE=<file>,
# YG_LATPROBE_WINDOW=1); pacing and latency around the stalls come from the probe log.
ORCA_SESSION=1 YG_INPUT=Tools/orca/inputs/bf-mario-link-results.txt YG_EXIT_AFTER=9800 \
  YG_PADREC=/tmp/bf.pads <nogui> -p headless -v Null -e <disc>
ORCA_SESSION=1 YG_LOOPBACK=/tmp/bf.pads YG_THROTTLE=1 YG_LOOPBACK_SPIKE=4000,150,2400 \
  YG_EXIT_AFTER=9600 YG_LATPROBE=/tmp/bf.probe YG_LATPROBE_WINDOW=1 <probe build> -p macos -v Metal \
  -C Dolphin.Core.SmoothEarlyPresentation=False -e <disc>   # a forced setting: only a probe build
                                                            # without it in Profile.cpp takes this
# Audio: add -C Dolphin.DSP.Backend=Cubeb -C Dolphin.DSP.Volume=0 (and AudioBufferSize=<ms>);
# the probe log's AQ lines are the mixer queue's depth and underflows, once a second.
```

Settings go through Dolphin's config system names: `Graphics.*`, never `GFX.*` (ignored without a
word). Headless Metal presents nothing, so presentation is measured in a `-p macos` window; warm the
shader cache first (a cold Metal cache costs ~3.3 s at match start). A forced session setting
(`Orca/Profile.cpp`) beats `-C`: comparing one needs a build that doesn't force it.

### V-sync move and steady presentation (vsync-move, 2026-10-07)

Luis approved taking back the ~13 ms Orca kept from Brawl's v-sync wait ("Code the JIT doesn't
see", 2026-10-04), for both games, without worse pacing during rollbacks. Branch `vsync-move` off
release-0.3.30 (`b9572814ba`); kCompatVersion 22. Probe builds: `vsync-probe-base` (b9572814ba +
the probe hooks) and `vsync-probe` (the candidate + the hooks, plus a `YG_PACE` knob for A/B rules),
both measurement-only.

**What changed**
- Both games run Project+'s "Move v-sync call for Brawl/PM": two guarded 3-word groups in
  `RSBE01.patches` and the same in `PPLUS32.patches` (written at the first live boundary, before
  Project+'s codes first write the same words, through the memory-patch path, so no machine's JIT
  keeps Brawl's words); `PPLUS32.ini` drops their two `KeepGameCode` lines. Originals checked in a
  RAM dump (ORCA_UX_PROBE): `0x80023B1C` is `bl 0x801E892C`, `0x80024020/24` `mtlr r0`, `addi r1,r1,0x20`.
- `Presenter::ImmediateSwap`: a copy is due at `GetTargetHostTime(next field ticks)`, computed at the
  copy on the CPU thread. The old due time was the host time taken when the previous field ran, so
  after a rollback it was the last re-run field's (unthrottled: `now`), and after a stall the old
  schedule's. That alone was most of prototype A's trouble: on the first frame after a re-run the
  arrival read as on time, Smooth's offset jumped half way to it and the frames after it waited.
  A state load's redisplay keeps `now` (its ticks are another timeline's: a keyframe join would
  otherwise sleep for minutes, caught in review), and no present waits more than 34 ms.
- `PresentPacer` (VideoCommon/PresentPacing.h) replaces Smooth Early Presentation's rule for
  Immediate XFB: present at due + offset, offset = all but the latest 4 of the last 180 arrivals,
  at most +0.5 / -0.2 ms a frame, never after due. A present after due would delay the next frame's
  start instead (tried: `late=1000` moved the arrivals 1 ms later and nothing else).
- `PrecisionTimer::SleepUntil` on macOS sleeps long waits in 1 ms steps. A stand-alone test slept
  ~11 ms with a 1.02 ms spin margin: woke 2.8-3.2 ms late at p50 (QoS user-interactive or not, nice
  or not); 1 ms steps: p50 0, p90 0.06 ms. In Orca the 12 ms present sleeps overshot 0-1 ms on half
  the frames (present intervals p10/p90 15.7/17.7 ms); with steps 16.68/16.69. Precision frame
  timing is now forced on (it was only Dolphin's default).

**Solo latency** (latency probe, 82 jumps over two runs each, M4 Pro 120 Hz built-in screen,
871x434 window, warm shader cache; base = b9572814ba + probe, cand = vsync-move + probe;
p10 / median / p90 ms):

| | Brawl base | Brawl cand | P+ base | P+ cand |
|---|---|---|---|---|
| sample -> XFB copy (emulated), median | 49.08 | 34.93 | 49.05 | 34.57 |
| response frame's boundary -> present call | 14.70 / 15.40 / 16.11 | 1.98 / 2.23 / 2.46 | 14.27 / 15.42 / 15.76 | 1.62 / 1.85 / 2.12 |
| sample -> present call | 48.22 / 48.78 / 49.43 | 35.33 / 35.57 / 35.87 | 48.01 / 48.76 / 49.45 | 34.97 / 35.19 / 35.46 |
| GPU done -> on glass, median | 16.70 | 16.88 | 17.16 | 17.19 |
| sample -> on glass | 65.61 / 68.57 / 71.93 | 52.64 / 56.24 / 59.53 | 65.92 / 69.14 / 72.21 | 52.16 / 55.70 / 59.03 |
| frames off the 2-refresh cadence | 4.3% | 1.9% | 4.5% | 1.9% |

The compositor sat in its slow state all night (~17 ms from GPU done to glass, against ~4-6 ms on
10-02 and 10-04), so the glass totals are ~13 ms above the earlier tables for both builds; the
difference is the move's: -12.3 ms Brawl, -13.4 ms P+ at the glass, -13.2 / -13.6 ms at the present
call. An earlier pair (pacer window 300, fall 50 us) gave the same medians (Brawl 48.78 -> 35.63,
P+ 48.81 -> 35.24 at the present call). The image still changes in the copy of press+2.

**Rollback play** (real-time loopbacks as in "Measurements": `YG_LOOPBACK`, `YG_THROTTLE=1`, input
delay 2, the remote 6-9 frames late, a 150 ms spike every 4 s from frame 2400, an AGENT TEST window,
Metal; analysed from 2400). Each build plays its own recording. Brawl: `bf-mario-link-results.txt`
(~890 rollbacks, 30 stalls). Project+: fuzzed play KOs differently in the two builds (base's match
ended at 3030, cand's at 4331, so the windows weren't comparable), so a KO-free script (port 1
jumps 3 in 13 frames, port 2 shields 3 in 8 and crouches 3 in 11; ~1440 rollbacks, 20 stalls, a
heavier load than fuzz). Analysis: `vsa2.py` (scratch, kept with the runs in
`~/Documents/brawl-dolphin/vsync-runs`); its 60 Hz simulation reads higher than the 10-02 table's
for the same build, so compare columns, not tables.

| Brawl | base (3 runs) | cand (2 runs) |
|---|---|---|
| 60 Hz repeats / drops, 1.5 s+ after a stall | 4.6-4.8% / 1.6-1.8% | 4.7-4.8% / 1.8% |
| the same, first 0.5 s after a stall | 7.8-8.0% / 2.0-2.1% | 7.7-8.2% / 1.8-2.2% |
| off the 2-refresh rhythm (120 Hz glass), first 0.5 s / 1.5 s+ | 9-12% / 6-8% | 8-9% / 6% |
| sample -> present, 1.5 s+ after a stall (p50) | 81.3 ms | 81.0 ms |
| boundary -> present, frames without / after a rollback (mean) | 14.04-14.06 / 15.70-15.73 ms | 13.69-13.70 / 15.34-15.35 ms |
| present-call gaps over 25 ms | 48-52 | 41-44 |

| Project+ | base (3 runs) | cand (2 runs) |
|---|---|---|
| 60 Hz repeats / drops, 1.5 s+ after a stall | 8.5-8.8% / 5.8-6.1% | 8.2-9.0% / 5.5-6.2% |
| the same, first 0.5 s after a stall | 10.5-11.1% / 4.1-4.6% | 10.9-11.5% / 4.4-4.9% |
| off the 2-refresh rhythm, first 0.5 s / 1.5 s+ | 17-23% / 20-22% | 21-22% / 17-24% |
| sample -> present, 1.5 s+ after a stall (p50) | 81.1 ms | 80.7 ms |
| boundary -> present, without / after a rollback (mean) | 13.06-13.21 / 15.92-16.14 ms | 12.70-12.76 / 15.62-15.71 ms |
| present-call gaps over 25 ms | 142-163 | 148-152 |

With 4-7 frame re-runs (11% of Brawl's frames, ~30% of this P+ script's) the offset sits at its
limit (the due time): latency and pacing match base's within the run-to-run spread (P+'s first
0.5 s after a stall reads ~0.4% higher; the first frame after a stall, which carries a 4-5 frame
re-run, is presented ~0.6-1 ms later than in base). The gain shows where re-runs are short:

| Remote 3-4 frames late (`_LAG=3 _JITTER=1`, no spikes: 1-frame rollbacks) | Brawl base | Brawl cand | P+ base | P+ cand |
|---|---|---|---|---|
| rollbacks (share of frames) | 442 (6%) | 441 | 932 (17%) | 919 |
| 60 Hz repeats / drops | 1.2% / 1.1% | 1.1% / 1.0% | 1.0% / 0.9% | 1.0% / 0.9% |
| sample -> present (p50) | 81.2 ms | 74.4 ms | 81.1 ms | 74.7 ms |
| boundary -> present (mean) | 14.04 ms | 7.74 ms | 13.92 ms | 8.97 ms |

Rules tried offline on the candidate's arrivals (`vsa2.py sim`, same arrival trace): Dolphin's
Smooth rule with the corrected due time is fine with 1-frame rollbacks (1.2% / 1.1% at 6.9 ms
boundary -> present in Brawl) but not with 4-7 frame ones (5.6-5.9% / 2.6-3.0% against base's
4.6-4.8% / 1.6-1.8%); presenting at the copy, 16% / 13%. Quantiles 950-990 and windows 120-600 all
pin at the limit with long re-runs; 980 of 180 keeps pacing at base's with short ones for 0.3 ms
more than 970. The window and fall rate set how fast the offset comes down after a match's load,
whose frames arrive late for ~170 frames (Brawl's scMelee intro, arrivals p50 -2 ms against -14.5
in play): with 300 frames and 50 us it settled ~13 s into scMelee, with 180 and 200 us ~11 s.

**Determinism** (candidate, `vsync-move` at the final build)
- Hash logs, two runs each: Brawl `bf-mario-link-results.txt` identical over 10,808 frames,
  Project+ `pplus-1v1.txt` over 9,006. Against base every frame differs from frame 0 (the groups
  land at the first boundary).
- Sync tests (`YG_SYNCTEST=7`): Brawl 300-4,500 (29,351 re-run frames), P+ 150-3,000 (19,901),
  0 RAM mismatches; with `YG_SYNCTEST_CLEARJIT=1` Brawl and P+ 300-2,500 (15,351 each), 0; from
  frame 2 to 700 in both games (4,837 each, rollbacks into the frames around P+'s JIT clear), 0.
- `jit-history.py` with clears at 388, 2,000, 4,500 and 7,000: Brawl SAME over 9,005 frames,
  Project+ SAME over 9,006.
- Loopbacks: every run PASS with all checksums matched (Brawl 959, P+ 719 a run).
- `dropin-commands.py`, both games, on Metal with a test present target (a wrapper turns the tool's
  `-v Null` into Metal + `ORCA_TEST_PRESENT`, so keyframe redisplays go through the timed present):
  PASS, keyframes loaded in 42-48 ms, checksums matched.
- Unit tests: 560 `Orca*`, 538 pass, 22 skipped (disc/live), 0 fail. New: `OrcaPresentPacing.*`,
  `OrcaUXGamePatches.BrawlRunsProjectPlusVSyncMove`; `OrcaLaunch.ProjectPlusKeepsBrawlsCode` now
  lists five kept words.
- Not run here: Jit64 (x86-64) hash logs and Mac <-> PC (`xplat.py`); the release's PC side.

**Perf gate** (gate-vsync, the lead's gate0330b scenario: Project+ ranked set over prod rooms, both
Orcas rendering with Metal test presents, 40 +- 8 ms, candidate = unsigned Release package of the
branch at `3fe0cba133`, the same code as the commit; base = live 0.3.29; B0/C0 warm-ups, then four
runs each, interleaved, none tainted by the SSD, every run PASS with every checksum matched).
Means per run, host / joiner at frame 14400:

| | 0.3.29 live | vsync-move |
|---|---|---|
| slow frames (hitches here + in game loads) | 8.5 / 8.2 (16.8) | 13.8 / 14.5 (28.2) |
| of which here | 1.5 / 1.25 | 0.5 / 0.5 |
| of which in game loads | 7 / 7 | 13.25 / 14 |
| stalls | 1.2 / 10.8 | 0.5 / 10.8 |
| rollbacks / re-run frames | 23.5 / 20.5, 69.5 / 61.5 | 25.5 / 22.5, 68.8 / 65.8 |
| snapshot ms | 0.92 / 0.93 | 0.93 / 0.97 |

The whole difference in slow frames is frames the game itself held in loads. With the move a frame
that runs over a field waits for the next retrace, so load frames hold whole fields: 2.0, 5.0, 6.0
where 0.3.29's held 1.5, 4.4, 5.4 (frame boundaries' emulated ticks in the loopback logs: base 12
such frames from boot to the match, the candidate 16, all in scMemoryChange, the end of scSelStage
and scMelee's first frames under the loading image). A 2-field hold is 33.367 ms and `HITCH_TIME` is
33.334 ms, so each one now counts as a slow frame in a game load; 0.3.29's 1.5-field holds never
did. They are emulation, the same on every machine (the session's own split puts all of them under
"game loads"). Hitches here, stalls, rollbacks and snapshot cost are level or better.

**Release (0.3.31, 2026-10-07):** the v-sync move ships alone as 0.3.31 (kCompatVersion 22), on
0.3.30 (`a65d5995d3`). The warm-up seed lists were re-recorded on the release's code (the old ones'
blocks over `0x80023B84-0x8002402C` held Brawl's words and were skipped): Brawl `bf-two-new.txt` to
frame 11500 (28,759 + 2,716 blocks, 27 s headless), Project+ `pplus-1v1.txt` to 1200 (26,946, 7 s).
Windows: its build, D3D present timing and a Mac <-> PC match are the PC's.

Correction (2026-10-07, evening): 0.3.31 never shipped on its own. The v-sync move and these seed
lists ship in 0.3.32 (kCompatVersion 23), with the name-tag and Sound slider commits and the
input-replay drop-in; see "Orca 0.3.32".

## Music switch

Approved 2026-10-06 (X report `no-music-toggle`: "no way to disable music", a P+ player's Discord
list). Orca 0.3.29 candidate, branch `music-off`. ORCA.md "Music switch" has the design.

**Where music and effects come apart.** An AX voice probe (every running parameter block once a
second, harness, default inputs; a throwaway `ORCA_AX_PROBE` build, not committed) over menus,
character and stage select and a match:

- Brawl: the music is always the pair `pb=8049A5C0` (main L only, `mixer_control` 1) and
  `pb=8049A480` (main R only, 2), `is_stream` 1, ADPCM, looping over ring buffers at nibble
  addresses `0x204BC6C2..0x204F06BF` and `0x204F5EC2..0x20529EBF` (MEM2), ratio 1.0 (32 kHz), AUX
  A/B/C volumes 0. Every other voice was `is_stream` 0: one-shot effects with AUX sends (reverb,
  `mixer_control` 0x0003-0x0C63), and one looping effect, still type 0.
- Project+: the same shape, with two stream pairs (ring buffers at `0x204AEB02`/`0x204E8302` and
  `0x20521B02`/`0x2055B302`), again no AUX sends.
- So the stream flag is the classifier, and no music reaches the AUX buses, whose returns the
  game's CPU computes and Orca could not split.
- The DSP issues `CMD_COMPRESSOR` every frame (the music's share gets the same gain).
- AX writes its output frame to `0x804E84C0` / `0x804E8640` (384 bytes each, alternating); the AI
  DMA reads them as `0x004E84C0` / `0x004E8640`. The shadow matches on the physical address.

**Rejected.** Game RAM (Brawl's sound system volume, the Options menu, P+'s code menu): every peer
hashes MEM1/MEM2 every 60 frames, a session starts from an empty NAND and a joiner plays on the
host's save, so any in-game setting is the whole room's, never one player's. A synced "music off
for the match" (the triage's first plan) would mute it for everyone.

**A first build was wrong, and the checks caught it.** `AXUCode`'s main buffers hold 160 samples
(AX GC's 5 ms); the first build copied all of them into 96-sample stack copies, writing past them on
the stack of the voice loop (not traced further). With music off, hashlogs left 0.3.27's at
the first menu music (Brawl frame 1513, Project+ frame 216) and two two-Orca runs desynced
(Brawl: `bo` at 78 s; Project+: frame 3360 after 22 matched checksums). Sync tests of that build
still passed (0 RAM mismatches): they compare a build with itself, so only a hashlog against the
other setting, or two Orcas set differently, shows a switch that leaks into the game. The same
build also never matched a frame (cached vs physical address), so it changed no sound at all.

**Checked with the fixed build** (`f46f76064a`; the cap and command gate after it change no
emulation): hashlogs on = off = 0.3.27 (Brawl frames 0-10800, Project+ 0-9006); sync tests off 0 RAM
mismatches (Brawl 73451 re-run frames / 10494 loads, Project+ 60851 / 8694); audio dumps
sample-aligned on/off (`wavcmp.py`): music-only seconds 2000-4000 RMS to 0.7; match off/on RMS
median 0.77 (Brawl) and 0.84 (P+); AI blocks quiet 524275 of 524288 (the misses are the first ones,
before AX's first frame), 97% in a sync test (re-runs rewrite the buffers; their sound is dropped
anyway). Two-Orca casual q2, ada off and bo on: P+ PASS 103/103 checksums; Brawl PASS 102/102. One
earlier Brawl attempt failed before matching (bo never left its main menu in 60 s; bo had the music
on, nothing music-related) and one died when the worktree SSD image dropped mid-run; the rerun ran
from the internal disk.

**Review, 2026-10-06 (rebased on the 0.3.29 candidate, `8e2035cf3e`).** A stream voice is split
whether or not it is running when its parameter block is read: an old AXWii applies per-ms updates
inside the frame and could start it after the check (Brawl's ucode isn't one of those, so nothing
leaked); a stopped voice adds nothing. A `static_assert` keeps the 96-sample copies inside the main
buses (the first build's overrun). Brawl Rev 2 and Project+ now offer 16 caps, the most a desktop app
before this release reads; the app and page released with the switch read 24.

Runs on the rebased build (internal disk; the worktree's SSD image was stalling, `disk11` APFS
flushes up to 19 s, and processes started with their cwd on it hung in `_dyld_start`):

- Hashlogs: Brawl Rev 2 on = off, frames 0-10807; Rev 1 off = Rev 2 on, 0-10819 (Rev 1 reads as
  Rev 2); Project+ on = off, 0-9003. Sync test off (Brawl to 9000): 60851 re-run frames, 0 RAM
  mismatches.
- Two Orcas, Project+ casual q2, ada off: PASS, 104 checksums each side, 0 rollbacks, no desync.
- The stdin gate (headless, `ORCA_TEST_COMMANDS`): the offer is the 16 caps with `music`;
  `music off` before any caps answer, and with caps that leave it out, answer `unsupported music`;
  with it, `Music: off on this machine`; a repeat logs nothing; `music loud` and `music on extra`
  are refused.
- Audio, on vs off: Brawl as before (match effects median off/on 0.77, music-only seconds to the
  0.7 floor, 655347 of 655360 AI blocks quiet); Project+ 88 music-only seconds silenced, effects
  median 0.83.
- Voice probe (throwaway build), Brawl default inputs, off: scVsResult is frames 9861-10277; a new
  stream pair (main L only / main R only, the same MEM2 ring buffers) starts there and plays to the
  scene's end, so the results music is muted like any other. Beside it: a mono 22 kHz voice panned
  center for about 2 s (the announcer), voices with AUX B/C sends (the crowd). Over the whole run all
  294 sampled stream voices had AUX A/B/C volumes 0 and `mixer_control` 1 or 2; no normal voice was
  panned hard left or right. Other stages weren't probed.
- Cost: `time -l` instructions retired by the whole process, Brawl to frame 9000 with a snapshot
  every frame from 5900, three each: on 236.79/236.61/236.55 G, off 236.53/236.51/236.76 G (the
  switch's work is a few microseconds a frame, under the run-to-run noise). Frame times
  (`perf.mjs`, frames 6000-9000): plain with snapshots, mean on 2.65/2.70 ms, off 2.66/2.74; under a
  k=7 sync test, p50 40.5/40.1 on and 40.6/40.9 off. Both sync-test runs with it off had a tail (96 and
  38 frames over 50 ms, against 2 and 0 on), far more than the switch's work could cost; the second
  overlapped ten flagged SSD stalls (`run-lock` marked both perf runs tainted). The release's
  two-Orca perf gate runs with one side off.

### Music back at a song's loop (2026-10-09)

**Reports.** Two Project+ players on Windows, desktop 0.12.39 (bug_reports `91db1039`, 10-08 17:51
UTC: "music still randomly turns on from time to time"; `0366bc41`, 10-09 01:49 UTC: "comes back on
full blast when it loops").

**Cause, proven.** The switch classified music by the voice's type alone (`is_stream` 1). A RAM
watch of the stream pair's parameter blocks (`ORCA_UX_PROBE` watch at pb+0x10, running and type;
Brawl's Wii PB in RAM has no updates field, so loop/end/current are at +0x72/+0x76/+0x7A) over 6-7
minutes idle on the main menu, 0.3.35, music off: at every loop of the menu song (every ~5485
frames, 91 s) NW4R's stream player sets the pair to type 0 with loop start and end inside the ring
(L: loop `0x204D06C2`, end `0x204CD81D`; ring `0x204BC6C2-0x204F06BF`), the DSP jumps ~34 frames
later and the bounds go back to the ring's, and the type goes back to 1 after 34-303 frames in all.
The audio dump's only loud spans (RMS 1000-4400, the song at full volume) are exactly those windows,
offset by the audio latency: Brawl 3.8/5.2/0.7 s, Project+ 3.8/5.2/0.7/2.1 s. Same PBs in Project+
(`8049A480`/`8049A5C0`). The 2026-10-06 checks didn't see it: no run sat through a loop in a
music-only scene, and "AI blocks played quiet" counts shadow hits, not what the split left in.

**Fix** (branch `music-loop-0335`, off `release-0.3.35`): `Orca::Music::StreamVoices` remembers,
per parameter block, the ring a stream voice loops over; while that voice is type 0 and plays
inside that ring it is still music. Leaving the ring (a reused block's effect, a song that ends
into the zero buffer) forgets it. Followed with the switch on too, so turning it off during a loop
covers that loop. Host only and never saved; RAM unchanged.

Checks (Mac, release build + the branch, `bugs-oct9/music` in the lead's scratchpad): unit tests
`OrcaMusic*` 13/13; idle menu off, Brawl 21600 frames and Project+ 25000: the fix's dumps have no
loud span (the type-0 windows still happen: 3 and 4); default inputs: hashlogs fix off = 0.3.35 on
on every frame, dumps on sample-identical, off identical but for a loop window each (Project+
136.2-137.0 s: 0.3.35 RMS 3869, fix 1; Brawl 131.4-132.8 s, in the match: 8893 to 7609, the
effects stay); sync tests off: 0 RAM mismatches.

## Match start

Luis, 2026-10-03 (on his Windows PC): the first seconds of every match (the countdown and just
after) dropped frames and froze, and the rest of the match was smooth; loading may take longer, but
those drops must go. Two things compiled there, on the one thread that runs the game in a session
(single core): the GPU pipelines the match draws first, and the match's PowerPC code. Both now
compile before the match is on screen.

Status, 2026-10-03 (branch `pplus-start`): on the Mac every match start is smooth from the first
launch after an update, Project+ and Brawl alike, now that Orca ships a JIT list for a player who
has none and paces the warm-up by when each block first runs (below). On Luis's PC 0.3.9 left
Project+ with a 374 ms frame at the end of its loading image and its first launch after the update
with shaders still compiling in the match; both fixes are in and not yet confirmed there.
Before: on the Mac, from the second launch after an update on, both matches of a
two-match run start smooth (tables below). On Windows it is likely fixed, not confirmed: nobody has
seen the freeze on his PC yet. What would confirm it is a run of the installed Orca on a copy of
his own Orca folder (D3D11, his UID and shader caches, one and two matches per process) that shows
the freeze, and the same with this build that doesn't. Until then, say "likely fixed" for Windows.

### Shaders

A match's countdown is where most of a game's new GPU pipelines are first drawn (both
characters, the stage, the countdown's effects). Orca sessions force (`Orca/Profile.cpp`):

- **Asynchronous ubershaders** (`ShaderCompilationMode=2`): a pipeline never drawn before compiles
  in the background and is drawn with an ubershader until it is ready. Dolphin's default
  (synchronous) compiles it on the emulation thread, which in a session is also the GPU thread.
- **Compile before starting** (`WaitForShadersBeforeStarting`): the ubershaders and every pipeline
  in `Cache/<game>.uidcache` compile on all cores before the first frame.
- **EFB copy pipelines before the first frame** (`ShaderCache::LoadEFBCopyUIDCache`): EFB and XFB
  copy pipelines are in no Dolphin disk cache and no ubershader covers them, so each kind compiled
  at its first copy, in every run: Brawl's first two (R8 and RGBA8) come at the countdown. Their
  UIDs are kept in `Cache/<game>.efbcopy.uidcache`.
- **The UID cache before the ubershaders** (`ShaderCache::CompileMissingPipelines`, sessions
  only): its pipelines queue at priority 150, ahead of the ubershaders (200), instead of after them
  (300). They are what the match draws and take a fraction of the time (FXC on Luis's PC: the
  ubershaders ~35 s, Brawl's 451 pipelines ~1 s after them), so a boot that stops waiting has them.
- **A limit on that wait** (`ShaderCache::WaitForBootCompile`, sessions only). The wait ends when
  everything is compiled, ubershaders and UID cache alike, or at its limit: 60 s, or the app's
  `ORCA_SHADER_WAIT_S` (60-900 s). Desktop apps installed before that variable kill a boot that
  hasn't drawn in 120 s (`desktop/src/orca.ts`), so Orca keeps to 60 s unless the app says it
  keeps a boot alive while `orca shaders` lines arrive (it then sets the variable and restarts its
  timer on each line). Before 2026-10-03 the wait also ended at 40 s as soon as the ubershaders
  were done: P+ on Windows stopped there with its 1,331 UID-cache pipelines left (seconds of work:
  Brawl's 451 took about one), and they compiled beside its first match (VI over 33 ms for 9 s). What is left at the
  limit compiles in the background on the runtime thread count. The log says which: "Orca:
  compiled N pipelines (K ubershader) before the first frame in X s on T threads" or "Orca:
  stopped waiting for shaders after X s (limit L s), ...". While the wait lasts more than a
  second, Orca prints `orca shaders <done> <total>` once a second, before `ready`, so an app can
  tell a slow boot from a hung one; the units are pipelines plus the shader stages they wait on (a
  pipeline completes only after every stage of its priority, so pipelines alone read 0 until the
  end: 0 of 1,493 for 30 s on Windows). `ORCA_TEST_SHADER_WAIT_MS=N` sets the limit to N ms.
  Dolphin's shader workers slept through work already queued when their count changed; they no
  longer do (`AsyncShaderCompiler::WorkerThreadRun`).
- **No ubershader compiled on the emulation thread** (`ShaderCache::GetUberPipelineForUidIfReady`,
  `VertexManagerBase::UpdatePipelineObject`, sessions only). A draw whose specialized pipeline
  isn't ready is drawn with an ubershader; when that ubershader is itself still compiling in the
  background (a boot that hit its limit), Dolphin compiled it again right there, a freeze of as
  long as FXC takes (seconds). The draw is skipped instead, as Dolphin's "skip drawing" mode does,
  until the ubershader or the specialized pipeline (ONDEMAND, first in the queue) is ready; the log
  says so once ("Orca: skipping draws whose ubershader is still compiling ..."). Mac test, P+ with
  an empty UID cache, the limit at 0 and each pixel ubershader slowed by 3 s: the first 600 frames'
  worst was 51.6 ms with the skip and 3,012 ms without it.
- **FXC's output kept across builds** (`D3DCommon/FxcCache.*`, D3D11 and D3D12, whenever Dolphin's
  shader cache setting is on). `Cache/Shaders/D3D-fxc.cache` maps a SHA-1 of exactly what FXC is
  given (a SHA-1 of `d3dcompiler_47.dll`'s bytes, the entry point, the target, the flags, the
  macros and the HLSL text; `Shader.cpp` passes no source name, include handler or second flags)
  to the DXBC it returned. FXC is a pure function of that input, so a build whose shader
  generators and GLSL-to-HLSL path write the same text gets its shaders without compiling; any
  change to the text, the flags or the compiler is another key, so a binary is only ever used for
  the compile that made it. Each record also carries a SHA-1 of its key and DXBC: a torn or
  interleaved write ends the read and the file is written again from the records before it. One file
  for every game, read whole into memory and started over past 64 MB. It stays open while Orca runs,
  opened so others may read and write it (`File::SharedAccess::ReadWrite`, `_SH_DENYNO`): Windows'
  plain open shares nothing once it writes, so a second Orca on the same user folder could neither
  read it nor add to it (and `FxcCacheTest` failed on the PC). No release from 0.3.4 to 0.3.9
  changed a shader generator, glslang, SPIRV-Cross or `D3DCommon/Shader.cpp`, while each touched
  other video files, so a key on the generator sources or the VideoCommon tree would have lost the
  cache every time and been only as safe as its file list. The log names every FXC compile of 250 ms
  or more ("Orca: FXC compiled a ps_5_0 in N ms (K KB of HLSL, from GLSL in M ms; ...)") and, at
  shutdown, "Orca: FXC cache: R shaders read from ..., F compiles found there, A compiled and
  added". `ORCA_TEST_FXC_FLAGS=<hex>` replaces FXC's `D3DCOMPILE_*` flags (rendering only, not in
  the lobby key), to time other optimization levels. `FxcCacheTest` (every platform: elsewhere
  the test builds `FxcCache.cpp` itself) checks that each of those inputs changes the key, that a
  torn last record keeps the records before it and the file is written again from them, and that
  a changed byte in a record's key or DXBC is never returned under any key, nor anything after it,
  and that another Orca with the file open to append keeps its records through this one's read,
  rewrite and appends (after a rewrite the file is reopened with `"ab"`: a `"wb"` handle writes
  where it is, over whatever the other appended).

Rendering only: nothing drawn reaches RAM (EFB/XFB copies stay on the GPU, EFB access and bbox are
off), so none of this can make two players' games differ.

Why it came back after every update: Dolphin's compiled-shader files (`Cache/Shaders/*.cache`, D3D's
DXBC on Windows) carry the build's git commit in their header and are emptied by any other build;
the pipeline UID cache survives (and, from 2026-10-03, D3D's `D3D-fxc.cache`). Without the boot
compile, that cache was recompiled on one background thread (`ShaderCompilerThreads` defaults to 1),
and a pipeline still queued at the countdown compiled again on the emulation thread. macOS adds
Apple's own Metal cache, kept per binary (Metal keeps no `Cache/Shaders` file). Until this change
only the desktop app's macOS command line asked for the first two settings. The app's `-u` is
`<userData>/orca/user` (`%APPDATA%\yougame-desktop\orca\user` on Windows, `~/Library/Application
Support/yougame-desktop/orca/user` on macOS), one folder for every Orca version: the UID caches, the
EFB copy cache and the JIT list survive updates, the compiled shaders don't. The app passes no `-v`,
so Windows draws with Dolphin's default, D3D11 (`D3D`), unless the folder's `Dolphin.ini` says
otherwise; the forced-settings line names it (`GFXBackend=`).

Measured 2026-10-03 (M4, Metal, 871x434 window, scripted Brawl Mario vs Link on Battlefield and
P+ 1v1, `YG_SHADERLOG=1` probe build): VI stalls from 0.6 s before scMelee to 12 s after.

| run | compiles on the emulation thread | VI > 50 ms | worst | lost |
|---|---|---|---|---|
| Brawl, synchronous, cold | 60 pipelines (3.9 s), 2 EFB copies (0.15 s) | 11 | 1,603 ms | 4,652 ms |
| Brawl, synchronous, a real player's UID cache (407), new binary | 18 (1.2 s), 2 EFB copies | 10 | 275 ms | 1,840 ms |
| P+, synchronous, a real player's UID cache (656), new binary | 80 (1.8 s), 2 EFB copies | 9 | 841 ms | 2,279 ms |
| P+, synchronous + compile before starting, same | 35 never-seen (1.3 s), 2 EFB copies | 7 | 779 ms | 1,860 ms |
| P+, ubershaders + compile before starting, same | 2 EFB copies (0.14 s) | 4 | 100 ms | 635 ms |
| Brawl, ubershaders + before starting, new binary | 2 EFB copies (0.15 s) | 4 | 103 ms | 672 ms |
| the same with the EFB copy cache | none | 3 | 95 ms | 538 ms |
| Brawl, everything warm (any mode) | none | 2 | 96 ms | 523-543 ms |

The boot compile costs, on the M4 after an update: 2.9 s with an empty UID cache, 10.7 s (Brawl,
407 pipelines) and 14.6 s (P+, 656); 0.2 s otherwise. With Luis's own P+ UID cache of 2026-10-03
(709 pipelines, 790 with the ubershaders) it took 15.2 s, printing an `orca shaders` line a second.
With the wait cut to 2 s (`ORCA_TEST_SHADER_WAIT_MS=2000`) it stopped with 772 left (63
ubershaders): the game ran, the menus had one 216 ms stall (ubershaders compiled at their first
draw), and the match started as it did without the limit. Windows compiles each shader through
glslang, SPIRV-Cross and FXC; on Luis's PC (RTX 2080, D3D11, 0.3.9 on copies of his folder) the
first boot after the update compiled Brawl's 613 pipelines (162 ubershader) in 38-39 s and stopped
P+'s 1,493 at 40 s. Of that, the 63 ubershader stages (54 pixel, 9 vertex: 9 texgen counts x
early/late/per-pixel depth x uint output on D3D) took about 30-37 s with every thread in FXC, and
Brawl's 451 UID-cache pipelines then about one second. The progress read 0 for the first 30 s
because pipelines complete only after every stage of their priority is compiled (fixed above).
The boot compiles on Dolphin's precompile thread count (logical cores - 2 unless `GFX.ini` sets
`ShaderPrecompilerThreads`), not the runtime 1-4. New textures cost 3-5 ms in the same window
(the hires lookup for Orca's own pack 0.1 ms). What is left with everything warm (about 100 + 60 ms
at the countdown and 45 ms at 4 s, no compile, no texture work) is not shaders.

Re-measure with a build of branch `hitch-shaders` (the `YG_SHADERLOG=1` probe: one line per frame
that compiled anything, and the shader cache's size and boot time).

### The match's code (JIT warm-up, `Orca/JitWarm.*`)

The first time a process runs a match, Dolphin's JIT compiles the match's code at its first use:
in Brawl ~16,500 blocks in the two frames where the stage and the first fighter appear (64-105 ms
each on an M4, the same with Null video) and ~4,100 more when the second fighter enters during the
countdown (45 ms); Project+ the same 5,200 + 11,200 blocks 22-25 frames after scMelee. A second
match in the same process compiles little (its blocks are still there).

- **Recording.** In a match's first 600 frames (from the scMelee scene on), every block the game
  compiles is noted: its start, the physical ranges of its instructions and a hash of those words
  in RAM. One whose words are not the same a second later (code rewritten after its compile) is
  never kept. At frame 600 or the match's end, a thread merges them into
  `Cache/<profile>.jitwarm` (Brawl ~30,000 blocks, 1.2 MB): a block found compiled or compiled again
  is young again, the others age a match, and one unused for 24 matches (or past 200,000) goes.
- **Warm-up.** From the next match's first frame, at each boundary before a first pass, up to
  8 ms (`ORCA_JITWARM_BUDGET_US`): blocks the JIT doesn't have, whose words are in RAM again, are
  compiled, earliest-run first. Those not in RAM (another fighter's code at that address) are
  looked at again every 30 frames, 1 ms per boundary, until frame 600. Both games have the match's
  code loaded at scMelee's first frame, and both show a still loading image ("Ready to fight") for
  a while after it: Brawl through frame ~169, Project+ through frame 27 (the stage from 28). On the
  M4 the warm-up takes 37 frames (Brawl, 30,000 blocks) and 35 (Project+, 28,000), the blocks the
  stage's first frames run first.
- **Orca's own list for a player without one** (`Data/Sys/Orca/RSBE01.jitwarm`,
  `PPLUS32.jitwarm`; `SeedPath`). A first install, or the first match after an update from 0.3.8
  or before, has no `Cache/<profile>.jitwarm`, and that match compiled its whole start itself: on
  Luis's PC (0.3.9 on a copy of his 0.3.8 folder) Brawl's first match froze 160, 117 and 326 ms
  where the stage appears, 2.9-3.2 s after scMelee (R-A-bf-1). Orca ships a list for each game,
  recorded from the reference matches with `ORCA_JITWARM_SEED=0` on a fresh user folder: Brawl
  `Tools/orca/inputs/bf-two-new.txt` (Mario vs Link on Battlefield, then Mario vs Donkey Kong on
  Final Destination: 32,625 blocks), Project+ `pplus-1v1.txt` (28,054). The x86-64 build under
  Rosetta and the arm64 build record the same bytes, and either JIT compiles the other's list with
  nothing discarded. The warm-up reads it only when the player's own file is missing or doesn't
  read, and treats it like the player's own: a block whose words aren't this game's in RAM is never
  compiled (another fighter's or stage's code waits and is compiled at its first run, as before),
  and after the first match the player's own list replaces it. The log says so: "JIT warm-up: no
  list of this player's yet; Orca's <path> (N blocks)". `ORCA_JITWARM_SEED=0` turns it off.
  Re-record both after a change to the games' patches, the scene detection or how the JIT splits
  blocks (a stale list only finds fewer blocks in RAM), on a fresh folder each:
  `ORCA_SESSION=1 YG_SCENES=1 ORCA_JITWARM_SEED=0 YG_INPUT=Tools/orca/inputs/bf-two-new.txt
  YG_EXIT_AFTER=11500 <Orca> -p headless -v Null -u <fresh> -e <Brawl rev 2>` and, for Project+,
  `ORCA_PROFILE=PPLUS32 YG_INPUT=.../pplus-1v1.txt YG_EXIT_AFTER=1150` with `-C
  Dolphin.Core.DefaultISO=<Brawl> -e <Project+ Netplay Launcher.dol>`; copy `<fresh>/Cache/*.jitwarm`
  into `Data/Sys/Orca`. Mac, 2026-10-03, windowed on copies of Luis's own Mac folder (his UID
  caches, no list), `match-start.mjs`: Brawl without the seed (`ORCA_JITWARM_SEED=0`) 93 and 62 ms
  where the stage appears and 47 ms at 3.9 s; with it 31,544 of 32,625 compiled ahead (1,025 are
  Donkey Kong's and Final Destination's, not in RAM), 16 compiled by the game, worst VI 22.5 ms
  in the whole window, as the second launch (23.4 ms). Project+ with it: 28,031 of 28,054 ahead, 34
  by the game, from frame 28 worst 23 ms (the second launch 21.4 ms; frames 0-27 had 87 and 84 ms,
  behind the loading image, with 13 pipelines his UID cache didn't have). Two matches from the seed
  (`bf-two-new.txt`): from frame 170 worst 21.3 and 21.4 ms, but match 1 had 91 and 76 ms at
  2.3-2.4 s, where Brawl's stage appears, with 44 pipelines his UID cache didn't have and 16 blocks
  compiled by the game: not the JIT, likely a new pipeline's first ubershader draw on Metal (the
  next launch had none over 25 ms in either match). Headless, per-frame RAM hashes from
  the seed equal the 0.3.9 references (Brawl 10,739 frames, P+ 9,006, x86-64 with
  `ORCA_TEST_JITWARM_SLOW_US=28` and arm64), and so do synctests across the match start from the
  seed (4,851 re-run frames each) and the loopbacks (P+ 579 rollbacks, Brawl 789, 879 checksums).
- **Paced by the recorded frames** (`PacedTarget`). Each block is due by the boundary 3 frames
  before the frame the recorded match first ran it (`when`, the earliest over the matches seen),
  and the first sweep goes past the 8 ms as far as the most pressing deadline needs, its blocks
  spread evenly over the boundaries left. Project+ runs its match code in frames 21-24 (400, then
  5,200, then 11,200 blocks) and 1,200 more in frame 34. Jit64 on Luis's PC compiles ~200 blocks
  in 8 ms (~40 us each; Rosetta 2 on the M4 ~600, JitArm64 ~800), so unpaced it had ~5,000 of
  them by frame 24, and the game compiled the other ~12,600 itself in frame 24: the 374 ms VI of
  the PC's run, and the "12,602 already compiled" (blocks the game had compiled before the sweep
  got to them; nothing invalidated them). Paced, the PC's frames 0-21 each take ~30-35 ms of
  compiling, all behind the loading image. Brawl needs ~110 blocks a boundary (18,000 by frame
  ~166), under the budget everywhere. One boundary never takes more than 100 ms
  (`MAX_SWEEP_US`): a sweep that fell behind (boundaries where the JIT had no room) catches up
  over several frames, not in one long frame during play.
- **The JIT's tables grow behind the loading image** (`JitBase::ReserveForBlocks`, at a match's
  first frame). Jit64 keeps an `m_back_patch_info` entry for every fastmem load and store it ever
  compiled (~11 a block, until a cache clear), and the unordered map rehashes inside the compile
  that doubles it: 4, 12 and 26 ms at 100,000, 200,000 and 400,000 entries under Rosetta (a
  match's ~30,000 blocks cross 400,000 during the warm-up; the PC's logs show one 22-27 ms
  boundary a run, likely this). At frame 0 Jit64 now makes room for 16 entries a block: each
  block listed in a process's first match (at least 32,000), 8,000 blocks in a later one (which
  compiles ~2,700 with a new stage and fighter). 13-16 ms there under Rosetta, no slow compile
  after; nothing to do at a second match. JitArm64 keeps a `std::map` there.
- **Why it can't change the game.** A block compiled early is the block the game's first run would
  compile: the same words at the same addresses (the hash), the same feature flags, compiled in the
  guest's floating-point mode as the dispatcher compiles, and kept only if its instructions span
  the recorded ranges (else it is erased unused). Like the blocks a rollback keeps
  (`Rollback.cpp` keeps the JIT across loads), it relies on the game invalidating code it rewrites,
  hence the second's check above. Only code an instruction BAT maps is touched (no page-table walk,
  no TLB change), and the JIT is never asked to compile where it could clear its cache or reuse
  code memory under the running block (`JitBase::CanCompileFromHook`, `CompileFromHook`: the frame
  hook runs inside a JIT block). The emulated instruction cache sees the same fetches earlier; the
  synctest already counts it device state only.
- **Jit64** (Windows, Intel Macs) compiles from the hook the same way (`Jit64::CanCompileFromHook`:
  no stack-fault cleanup pending, trampolines not almost full, 1 MiB free in both the near and the
  far code). Dolphin frees code only in a compile from the dispatcher, which resets the host stack
  first (`Jit64AsmRoutineManager::ResetStack`), so a free range never holds the running block or a
  return address the BLR optimization pushed; a hook's compile takes only free ranges and leaves
  what was freed since to the dispatcher's next compile. A stack fault inside a hook's compile
  (the hook runs deeper in the host stack) is Dolphin's own: it only invalidates blocks and asks
  the next dispatcher compile to clear the cache, and the hook checks again before each block.
- `ORCA_JITWARM=0` turns it off. `ORCA_JITWARM_LOG=1` logs a line for each of a match's first
  600 frames: what the warm-up compiled and how many were due, what the game compiled itself and
  what the warm-up had done with those (warmed and gone since, not reached yet, not listed), and
  any single compile over 4 ms; `=2` also counts, outside matches, the listed blocks already in
  RAM (Project+: 26,000 of 28,000 from the title screen on, its code is resident). The frame lines
  place each frame in time: `node Tools/orca/match-start.mjs --from-frame 28 <run>` (Project+;
  Brawl 170) counts the VIs a player sees after the loading image. `ORCA_TEST_JITWARM_SLOW_US=N`
  makes each warm-up compile N us longer, a slower host for testing the pace.

Measured 2026-10-03 on the M4 (871x434 Metal window, the same scripted matches; host VI intervals
from 0.6 s before scMelee to 12 s after; "before" is the 0.3.8 release, with the macOS app's
`ShaderCompilationMode=2` + `WaitForShadersBeforeStarting` flags unless it says synchronous):

| run | VI > 25 ms | > 50 ms | worst |
|---|---|---|---|
| Brawl before, synchronous (as on Windows), cold | 12 | 10 | 1,502 ms |
| Brawl before, cold | 5 | 4 | 102 ms |
| Brawl before, warm | 4 | 2 | 95 ms (the JIT: 68 + 95 at the stage, 46 at P2's entrance) |
| Brawl after, first match ever (no list yet) | 5 | 4 | 102 ms |
| Brawl after, warm (4 runs) | 0 | 0 | 22.6-24.9 ms |
| Brawl after, a new binary (Metal cold) | 0 | 0 | 22.8 ms (boot 8.7 s instead of 2.1) |
| P+ before, synchronous, cold | 10 | 10 | 538 ms |
| P+ before, warm | 2 | 2 | 88 ms |
| P+ after, warm (4 runs) | 2 | 0 | 30.9-31.6 ms |

Two matches in one process (`Tools/orca/inputs/bf-two-new.txt`: Mario vs Link on Battlefield, then
Mario vs Donkey Kong on Final Destination, a stage and a fighter the first match never loaded), on
copies of Luis's own Mac Orca folder (his UID caches; `node Tools/orca/match-start.mjs <run>`):

| run | match 1 | match 2 |
|---|---|---|
| before (0.3.8, the macOS app's flags) | 4 VI > 50 ms, worst 99 ms | none > 33 ms, worst 25 ms |
| after, first launch after the update (no list yet) | 2 > 50 ms, worst 97 ms | none > 33 ms, worst 27 ms |
| after, the next launch | none > 25 ms, worst 23 ms | none > 33 ms, worst 27 ms |

On the Mac a second match was already smooth before: its shaders were asynchronous, and its code
was mostly compiled by the first match. On Windows, before this change, a never-drawn pipeline in
any match compiled on the emulation thread.

The game's own long frame at the end of loading (100 ms in Brawl, 84 ms in P+) remains, behind the
loading image. Emulation is unchanged: per-frame RAM hashes with the warm-up equal 0.3.7's (Brawl
10,800 frames, P+ 9,000) and a run without it over two Brawl matches (19,020 frames, the second
with other fighters); loopbacks pass (Brawl 936 rollbacks, P+ 579), and so do synctests across the
match start (both games, 4,851 re-run frames each).

The same on Jit64, 2026-10-03: an x86-64 build under Rosetta 2 with AVX and FMA advertised
(`ROSETTA_ADVERTISE_AVX=1`, the paths a PC runs), each game first recording its list and then
compiling from it: 30,004 (Brawl) and 26,532 (P+) blocks compiled from the hook, none discarded, no
boundary without room, no cache flush, no BLR fault. Per-frame RAM hashes equal the arm64 ones
above (Brawl 10,807 frames with and without the list, P+ 9,002, the two-match run 19,010);
synctests across the match start pass (4,851 re-run frames each) and so do loopbacks (Brawl 936
rollbacks, P+ 579). Rosetta translates Jit64's output, so this checks Jit64's logic, not Windows
itself (its thread-stack guard, MSVC): the same checks on the PC are what close that.

## Code the JIT doesn't see

Branches `pplus-jit-clear` (the prototype, on `rel-0.3.14`) and `rel-0.3.15`, 2026-10-04. The
canonical-start spike found Project+'s RAM changing the frame after a JIT clear (Brawl's never
does). Traced, measured both ways, and fixed by keeping what 0.3.14 ran.

**The cause.** Project+'s Gecko code handler applies `NETBOOST.GCT`'s RAM writes with plain stores
(no `dcbst`/`icbi`), and the first time it writes them (frame 1, the first frame after the frame
hook goes live) seven words of Brawl's code change, in functions the boot has already run and the
JIT has already compiled:

| Words | Code (`Project+/NETBOOST.TXT`, `Source/Extras/Netplay.asm` on the SD card) | Effect of a clear |
|---|---|---|
| `80023b88` beq -> nop, `80024028` blr -> b `801e892c` | Move v-sync call for Brawl/PM | RAM differs the next frame |
| `8001cd24`, `8001cd2c`, `800266b8` | Fixed OSReport Syntax on File Reading | RAM differs at the next file read |
| `8018cfc4`, `8018cfc8` cmpwi/beq -> cmplwi 0x30/blt | [Legacy TE] Extra null pointer catch | none seen |

Nothing invalidates those blocks afterwards: no icbi, dcbst or dcbf reaches the lines (none in
40,000 frames and five matches; DMA never drops JIT blocks in this Dolphin), a rollback load restores only the 4 KB blocks whose bytes changed
and a keyframe only the pages that differ (the handler writes the same words every frame, so they
never differ), and Dolphin's own workaround for code handlers that don't invalidate
(`HLE_Misc::GeckoCodeHandlerICacheFlush`: an icache reset and a JIT clear on the handler's first
five runs) is spent during the boot, before Project+'s codes start writing (its counter at
`0x80001800` is already at five at frame 0). So a warm JIT runs Brawl's words there for the whole
process, and the first whole-cache clear switches it to Project+'s: the v-sync move alone
(dropping just the blocks at `80023b88` and `80024028` at frame 388) reproduces the spike's change
at frame 389. The emulated instruction cache plays no part: no compile ever read a cached line that
differed from RAM. Brawl has no such words (its RAM is the same after any number of clears).

**Who clears, today.** Orca itself never does in a session: rollback loads and keyframe joins keep
the JIT (only changed pages are dropped, `Rollback.cpp`), BAT reloads are skipped when the BATs
are unchanged, and a session refuses savestates. Dolphin clears after the boot only when its code
space or Jit64's trampoline space fills, after a stack-guard fault, or when a JIT setting changes;
none happened in the runs below. Code space after five Project+ matches (five stages, other
fighters each time; 24,000 frames): JitArm64 26 of 128 MB near and 46 of 128 MB far (the first
far half 72% full, 1-5 MB more a match with new content), Jit64 24 of 128 MB near, 3.6 of 64 MB
far, 0.1 of 8 MB trampolines. A flush takes dozens of matches of new content, and a Mac and a PC
fill at different rates: rare, but a desync from the next frame when it happens, and the same for
any future Orca feature that clears the JIT.

**What 0.3.14 ran.** Brawl's frame function (`0x80023AE4`, called from the main loop at
`0x80017420`; the frame hook at `0x80017504` ends the loop) waits for a retrace before it draws,
in a loop (`0x80023B04`, `VIWaitForRetrace` at `0x80023B1C`) left by the `beq` at `0x80023B88`
while no XFB slot is free. Project+'s v-sync move never loops there (`nop`) and waits at the end
instead (`b VIWaitForRetrace` for the `blr` at `0x80024028`), so a frame is drawn and copied right
after the game's logic. Under a warm JIT 0.3.14 kept Brawl's wait: the one block compiled after
the codes landed (`0x80023B00`, frame 1) holds the `nop` but reaches it only with a free slot, and
the boot's blocks run the other six words as Brawl's. Its hash logs equal those of a build that
fetches Brawl's word at all seven addresses (below), so that is what every Orca up to 0.3.14
played.

**Measured, 2026-10-04** (M4 Pro, 120 Hz built-in screen, 871x434 Metal window, real time, the
latency probe on `rel-0.3.14` + the prototype: "A" with `JitClearFrame = 2`, the codes running,
against the same binary without it, which hashes as 0.3.14 over all 9,007 frames of
`pplus-1v1.txt`). Latency: 82 jumps each (two runs, interleaved), the image changes in the copy of
frame press+2 in both and the RAM at press+0:

| Project+ | 0.3.14 | A (codes on) |
|---|---|---|
| sample -> XFB copy (emulated) | 49.10 ms (2.94 fr) | 34.44 ms (2.06 fr) |
| sample -> present call: mean, p10 / p50 / p90 / max | 48.56; 47.92 / 48.52 / 49.30 / 50.72 | 35.52; 34.81 / 35.54 / 35.99 / 39.54 |
| sample -> on glass: mean, p10 / p50 / p90 / max | 53.15; 51.55 / 52.77 / 54.81 / 56.90 | 40.13; 38.83 / 39.63 / 43.20 / 46.15 |
| frame's boundary -> its present call (mean) | 15.10 ms | 2.28 ms |

Pacing, solo (the probe runs, about 3,800 frames each, and whole fuzzed matches, 2,340 frames
each, two of each per side): the same. Boundary-to-boundary p99 17.2-17.8 ms both, no frame over
25 ms in A (0.3.14 up to three); on a simulated 60 Hz screen A repeats 0.9-1.2% of refreshes and drops
0.9-1.2% of frames, 0.3.14 1.2-1.5% and 1.1-1.4%; on the 120 Hz glass A shows 8.2-9.6% of frames
off the two-refresh rhythm, 0.3.14 6.5-8.5%.

Rollback (`YG_LOOPBACK` of a recorded fuzzed match, `YG_THROTTLE=1`, input delay 2, in the window;
the same rollbacks on both sides), per run:

| | 0.3.14 | A |
|---|---|---|
| remote 6-9 frames late: 270 rollbacks, 1,143 re-run frames | | |
| boundary -> present, frames without a rollback (mean) | 13.73 / 13.78 ms | 12.30 / 12.36 ms |
| present-call gaps over 25 ms | 17 / 15 | 42 / 27 |
| 60 Hz screen: repeats / drops | 2.2-2.3% / 2.1-2.2% | 3.4-3.9% / 3.3-3.8% |
| remote 3-4 frames late: 133 one-frame rollbacks | | |
| boundary -> present (mean) | 13.79 / 13.74 ms | 9.61 / 9.68 ms |
| present-call gaps over 20 / 25 ms | 22 / 0 (each run) | 46, 40 / 7, 6 |
| 60 Hz screen: repeats / drops | 1.0% / 0.9% | 2.0-2.1% / 1.9-2.0% |
| 6-9 late with a 150 ms spike every 4 s (8 stalls): 60 Hz repeats / drops, gaps over 25 ms | 4.7% / 2.3%, 28 | 6.1% / 3.7%, 51 |

Why: Smooth Early Presentation (`Presenter::GetUpdatedPresentationTime`) moves its offset half the
way toward a late frame at once and only 1% a frame back toward early ones. A's frames come about
13 ms before their VI time, so a rollback's re-run time lands on that frame's present, the offset
jumps, and the frames after it wait (present sleep 9.8 ms on average against 2.2): with rollbacks
A keeps 1-4 ms of its 13 ms and its presents follow the re-runs. 0.3.14's copy waits for the
retrace anyway, which hides a re-run.

**The decision** (rule: A only with equal or lower latency and no pacing regression): A has lower
latency everywhere, but twice the 60 Hz repeats and drops and more long present gaps whenever
there are rollbacks, so 0.3.15 keeps 0.3.14's behaviour. The 13 ms is real for solo play and for
online play without rollbacks; taking it online needs a presentation that doesn't let a re-run
frame drag the others (and vanilla Brawl has the same wait, `RSBE01`).

**Update, 2026-10-07 (`vsync-move`, kCompatVersion 22):** the 13 ms is taken back. Both games now
run the v-sync move (Project+'s two words land from `PPLUS32.patches` at the first boundary, before
its codes write them, so no JIT keeps Brawl's), and presentation no longer follows re-runs: the due
time is recomputed from the throttle at the copy and a quantile of arrivals sets the offset
("Input latency", "V-sync move and steady presentation"). The two v-sync `KeepGameCode` lines are
gone; the other five stay.

**The fix (0.3.15).** `KeepGameCode = <address> <Brawl's word> <Project+'s word>` lines in
`PPLUS32.ini` (`Profile.h` `kept_code`, `Orca::KeptInstruction`): every instruction fetch at those
seven addresses (`MMU::TryReadInstruction`, so both JITs' compiles and the interpreters) gets
Brawl's word while RAM holds Project+'s, so any JIT clear compiles what ran before. RAM is
untouched: its hashes, the codes' own reads and the compatibility of everything else are as in
0.3.14 (a guarded game patch writing Brawl's words back would have changed RAM, and with it every
hash). Any other word at those addresses is fetched as it is. `JitClearFrame = 2` stays from the
prototype as a safety net: one clear at the end of frame 2 (frames count at the end of their first
run, a launch joiner's boot ahead and catch-up included, a rollback's re-run not), so every
machine compiles what a fetch reads from then on, whatever its boot compiled, and the kept words
go through a compile in every boot. Checks on `rel-0.3.15`:

- `pplus-1v1.txt`, 9,000 frames: hashes equal to live 0.3.14's (`notes/r314/mac-pplus.hash`),
  frame by frame (9,006 frames compared).
- `jit-history.py` with clears at 388 (where the v-sync move used to show), 2,000, 4,500 and
  7,000: the same 9,013 frames as the run without. Census (`YG_JITCODE_CENSUS`, against what a
  fetch reads): 0 words in the DOL and the codes compiled other than that, at frames 1 and 3 and
  through the match (0.3.14: the seven, at every census); the rest are heap blocks of modules the
  game unloaded.
- Synctests (`YG_SYNCTEST=7` from frame 300): to 4,000, 25,851 re-run frames, 0 RAM mismatches;
  with `YG_SYNCTEST_CLEARJIT=1` (the JIT dropped after each load) to 2,500, 15,351 re-run frames,
  0 (0.3.14 fails that one: 28 RAM mismatches in its first 500 frames).
- Jit64 (x86-64 under Rosetta 2): hashes equal to live 0.3.14's on a PC (`pc-pplus.hash`, which
  equals the Mac's), and the same with clears at 388 and 4,500.
- Unit tests: `OrcaLaunch.KeptGameCode`, `OrcaLaunch.ProjectPlusKeepsBrawlsCode`.

The prototype (A) passed the same kinds of checks with the codes running (clears at 388,
2,000+4,500 and 7,000+15,000+20,000 over five matches hash as without, on JitArm64 and Jit64;
synctests with and without `YG_SYNCTEST_CLEARJIT`; a launch join and `dropin-identity.py --pplus`
over a real room); it boots to the main menu at frame 194 instead of 197. Measuring again: the
latency probe (`Tools/orca/latency-probe.py`, `lat-probe`; its P+ scripts need the
`@muMenuMain:1 mash 30 99999 1 40 A` line `pplus-1v1.txt` has since the boot goes to the menu),
and for A a build without the `KeepGameCode` lines.

**Checking a codeset** (a Project+ update, another launcher profile): `Tools/orca/jit-history.py
<nogui> <disc> --pplus "<dir>/Project+ Netplay Launcher.dol"` runs the boot twice (as it comes, and
with JIT clears at `--clear` frames), compares the RAM hash logs frame by frame and lists every
word a live block runs that a fetch no longer reads (RAM, or the profile's kept game code; a census
at the profile's own clear frame runs after that clear). A codeset that rewrites game code in its
first frames shows here as words at frame 1 or 3; list them in the profile as `KeepGameCode`. Harness knobs (`Rollback/Harness.cpp`,
`Rollback/Diag.h`): `YG_CLEARJIT_AT=<frames>` / `YG_CLEARJIT_EVERY=N` (`YG_CLEARJIT_FROM`),
`YG_JIT_INVALIDATE=<frame>:<address>`, `YG_WATCH=<addresses>`, and `YG_JITCODE_LOG=<file>` (every
compile that read a word other than the last compile at that address, or than RAM, with what
reached the address in between; every clear and its source) with `YG_JITCODE_CENSUS=<frames>`.

## Game patches

`Data/Sys/Orca/<profile>.patches` (`Core/Orca/UX/GamePatches.h`): Orca's own changes to the game's
code and data, forced on in every session (never the player's own codes, which a session drops)
and hashed into the lobby key. They are applied at every frame boundary from the frame hook, first
runs and re-runs, before anything there is saved, as a pure function of emulated memory. One 32-bit
word per line, in hex; three kinds:

| Kind | Lines | Written |
|---|---|---|
| Guarded group | `<address> <original> <value>`, each 4 bytes after the last | once, while every word is its original (a module just loaded, a default the game just set); a line whose original is its value only guards |
| Data | `<address> * <value>` | every frame |
| Toggled group | guarded lines between `when` lines and `end` | its values while the conditions hold, its originals again while they don't |

**Toggled groups** (2026-10-03, for the online modes' locks: rules, and no way out of the
character select while a queue match's header says casual or ranked; friends games untouched):

```
when 806F2380 == 59474D31               # the match block's first word is 'YGM1'
when 806F2384 & FF000000 != 00000000    # and its masked word isn't 0
8119A120 2C030000 2C030000              # (an example) cmpwi r3, 0 (guard)
8119A124 4082000C 4800000C              # bne -> b while both hold
end
```

- A block's `when` lines must all hold: `when <address> [& <mask>] ==|!= <value>`, one aligned
  word of MEM1 or MEM2 each, read at the boundary. An address that isn't mapped never holds.
- At each boundary, a group whose words are all its originals gets its values while the conditions
  hold; one whose words are all its values gets its originals back while they don't. Anything else
  (its module not loaded, other code in that memory) is left alone, as a guarded group is; a guard
  word (original = value) has to match in both states.
- Both ways, every word goes through Dolphin's memory-patch path, which drops the JIT's copy, so
  the code the game runs next is the code in memory. A rollback's restore drops the JIT's blocks
  for every page it copies back, and a joiner's keyframe load is such a restore.
- They are applied last in the frame hook (`ApplyToggledGamePatches`), after every other writer
  there: a condition one of them sets at a boundary is followed at that same boundary, and the hook
  run again at that boundary (a re-run from the snapshot saved there) writes nothing new.
- Deterministic: the decision reads only the condition words and the group's own words. Both
  machines, every re-run, and a friend who loaded a keyframe (memory carries whichever state the
  code was in) decide the same at the same boundary.
- Refused at load (the file is then "damaged", status `profile`): a `*` line in a block; a block
  with no lines; a `when` after a block's line (no nesting); an `end` without a `when`; a file that
  ends inside a block; a mask of 0, or a value with bits outside its mask; a condition that reads a
  word the file writes (a toggle never switches its own condition, or another's); a toggled group
  of one word, or one that changes nothing. A toggled group writes on its values as well as on its
  originals, so a lone word would match other code by chance twice as often as a guarded one: the
  words around a flip go in as guards. A group never spans a `when` or an `end`.
- Meant for branch flips in the game's own code; real code goes into a cave. Conditions read fixed
  addresses only (no pointer chains): the match block's header is at one ("Free space":
  `FreeSpace::kMatchBlock`, 0x806F2380 in both games).
- Never on a word that game code also writes. Project+'s Gecko code handler writes its hooks again
  every frame (a C2 code's branch, such as its lag fix's at `0x8002AD8C`): a group toggled there
  lands at every boundary and is undone within the frame, and which of the two words the JIT runs
  then depends on the machine's JIT history (a synctest that clears the JIT after each load showed
  1,487 RAM mismatches in 850 frames). Any of Project+'s own hook sites the locks need (the plan
  lists its Code Menu opener and the Giga Bowser and Wario-Man selections) needs another way if it
  is such a hook: the codeset's data, or the input gate. A group that logs `on` at boundary after
  boundary and never `off` is in such memory.
- Adding the kind changed nothing the shipped files write, so it took no UX version of its own (the
  key's `ux=7` is the name tags' controls). A file that uses it is refused by older builds, and the
  file is in the lobby key, so they never meet.

Tests: `OrcaGamePatches*` (the format and its refusals, toggling both ways, the guards, the
conditions, idempotence and purity over a fake memory, and that the shipped files' groups land as
before).

Checked 2026-10-03 (Mac, arm64), with a test-only copy of each file that toggles the pad copy's lag
fix every 8 frames by the frame counter at `0x805B50D8` (a word that counts frames in both games),
so the code the game runs changes under every rollback window and keyframe: Brawl's 45-word lag-fix
group inside a `when` block, and in Project+ the pad copy's 22 loads moved by 0x404 (its own hook
at `0x8002AD8C` left alone, see above).
- The shipped files hash as on `origin/matchmaking` on every frame (`YG_HASHLOG`: Brawl
  `bf-mario-link-results.txt` 10,809 frames, Project+ `pplus-1v1.txt` 9,018).
- Toggling, the same run twice (the second compiling its match from the JIT warm-up list) hashes
  the same on every frame: Brawl 10,807 frames with 1,350 toggles, Project+ 9,021 with 1,127.
- `YG_SYNCTEST=7`, 0 RAM mismatches: Brawl from main menu through character select, stage select
  and the match start (1500-2700, 8,351 re-run frames, 1,384 toggles), the same with
  `YG_SYNCTEST_CLEARJIT=1`, and the match's end through results back to character select
  (9700-10800, 7,651); Project+ from boot through character select, stage select and the match
  start (150-1000, 5,901) and the match's end through results (4200-5100, 6,251). With the JIT
  cleared after each load, Project+ shows 28 RAM mismatches in the first seven frames of the
  window wherever it starts (151-157 from 150, 171-177 from 170), the same frames on
  `origin/matchmaking` without toggles, and none after them.
- `YG_KEYFRAME_TEST`, every one PASS (the RAM loaded equals the capture; 30 re-run frames hash as
  their first pass): Brawl at 1700 (character select), 1880 (stage select), 2050 (the match
  loading), 5000 (the match), 10000 (results), 10500 (character select again); Project+ at 250,
  400, 2000, 4500, 4900.
- `Tools/orca/dropin-commands.py`, both games, PASS: Brawl joins at character select (frame 1,828)
  and mid-match (9,118), 116 and 21 checksums matched, 14 rollbacks; Project+ at 1,975 and 7,385,
  84 and 21, 22 rollbacks.

Project+ on Windows, 2026-10-03 (the 0.3.9 candidate on Luis's PC, RTX 2080, D3D11, copies of his
folder). From the second launch, P+'s match 1 had 4 VI over 33 ms, one of 374 ms, and the warm-up
logged "15,452 of 28,054 blocks compiled ahead ... 12,602 already compiled" then "compiled 12,613
blocks itself" (headless Jit64 the same, 11,003). Those 12,600 were compiled once, by the game, in
frame 24, before the warm-up's 8 ms a frame got to them (it was ~5,000 blocks in); every one of
the 4 VIs was frames 0-24, while P+ still shows its loading image (through frame 27). From frame 28
(`match-start.mjs --from-frame 28`) that run had no VI over 25 ms, worst 22 ms, as Brawl's second
launch. The same on the M4 under Rosetta with `ORCA_TEST_JITWARM_SLOW_US=28` (the PC's ~40 us a
block): unpaced 15,036 ahead, 13,018 already compiled, 11,035 compiled by the game in frame 24
(none "warmed and gone": nothing invalidated a warmed block); paced 28,031 of 28,054 ahead, 34
compiled by the game in the first 600 frames, frames 0-21 at 30-38 ms of compiling each, and no
compile over 4 ms after frame 0 (the backpatch map's 26 ms rehash had landed at frame 31). Per-frame
RAM hashes with the pace and the slow knob equal the 0.3.9 references (P+ 9,006 frames, Brawl
10,739), and so do synctests across the match start (4,851 re-run frames) and the P+ loopback (579
rollbacks, 879 checksums). What a rollback costs the JIT: Rollback.cpp invalidates every block in
a 4 KiB page it restores, and Project+ has code in pages whose other words change from frame to
frame (blocks at 0x8056xxxx, 0x8078xxxx, 0x8093xxxx, 0x8106xxxx, a few in the DOL), so each
rollback compiles 40-120 blocks again (~1.5-5 ms of Jit64 on the PC), warmed ones included; the
loopback's 579 rollbacks did ~2,000 such compiles in the first 600 frames, none in the loading
frames. Invalidating only the 32-byte lines that changed would keep the blocks whose own words
didn't (not done, not measured).

## Online menu

Luis's rules: Play Online works in both games, through the game's own menus and never a new
screen; no Nintendo server is ever asked for anything; nothing about connecting looks like
Nintendo's. What this build does (the menus and the events here; the search, `host <code>` and
the matched room in "Matchmaking" below):

- **The top page.** Brawl's Wi-Fi button reads PLAY ONLINE (a texture, below) and can be selected
  again; Project+'s already reads PLAY ONLINE. The footer under it says "Play different modes
  online." in both games.
- **A opens the game's own ONLINE page** (With Friends / With Anyone), with no connection window:
  no save check ("There is no save data" in Project+), no NAND access, no network interface task.
  B goes straight back to the top page, with no "Disconnect from Nintendo WFC?" window. Options
  (WiiConnect24) and Spectator never show. Brawl's page title reads ONLINE.
- **With Anyone** shows CASUAL and RANKED (textures, both games, with their descriptions
  rewritten). Either goes to the local Versus character select and Orca prints
  `orca menu online casual` or `orca menu online ranked` (with the app's `host`: the page's queue
  searches while the player waits there, "Matchmaking"), else `orca menu online casual local` or
  `orca menu online ranked local`.
- **With Friends** goes straight to the local Versus character select, where friends drop in
  ("Drop-in"), and Orca prints `orca menu online friends` (since `ux=18`; Luis, 2026-10-05: "every
  single button in with friends menu does the exact same thing ... just have with friends and get
  rid of that extra submenu"). The game's own With Friends page (FIGHT!, Friend Roster, Team
  Multi-Man, Home-Run Contest) never opens: its four buttons all went to that same character
  select (exit codes 24-27 through Orca's one block), so nothing was lost. The button's description
  says "Play with friends you invite! (2-4 players)" (the game's "Play with people you've registered
  as Friends!" named a roster that isn't there).
- **B out of that character select** goes back to the page the pick was made on, with the cursor
  on the button picked, the way B after Group > Brawl comes back to Group with Brawl selected (and
  the way the game comes back from its own online modes): the ONLINE page on With Friends, With
  Anyone on Casual or Ranked (one more B is the ONLINE page); from the ONLINE page one more B is the
  top page. It holds after matches too: the results come back to the same character select, and B
  from there goes back to the same page. From Group > Brawl (Project+'s VERSUS) B still opens
  Group.

### How (`Data/Sys/Orca/RSBE01.patches`, `PPLUS32.patches`; `Core/Orca/UX/MenuText.h`, `OnlineMenu.h`)

- **Game patches**, guarded code groups (`GamePatches.h`); Project+'s copy of the menu module sits
  0x1480 above Brawl's, sora_scene is the same in both:
  - Brawl only: the main menu's cursor table is the game's own again (Group moves down onto the
    button). Project+'s was never changed.
  - The top page's press on the button takes the path every other button takes into its page,
    never creating the connection window (`muWifiCnctWndTask`), which is what would create the
    network interface task, check the save's Wi-Fi data and connect.
  - The ONLINE page's B does what the game does once its disconnect window says OK, without the
    window.
  - The ONLINE page's A on With Friends (muProcWifi's update, `0x81179CB0`; two copies of its
    input handling) leaves the menu as the With Friends page's FIGHT! did (`0x81191904`): the
    proc's exit with code 25 (`0x806A5604`: the button's decided animation, then the menu leaves
    two frames later, its procs no longer running), the same sound, return 0. It used to set the
    next proc to `0x1A`, the With Friends page (found under the harness: the proc's next proc,
    `+0x634`, watched; With Anyone is `0x1B`, Options `0x1C`). The code sits in the disconnect
    window's creation, which B's path above no longer reaches (nothing else branches there, and its
    last word already branches to the function's `li r3, 0`); the redirect, B's path and that code
    are one group, so none lands without the others.
  - The ONLINE page's Options (`muProcOptWifi`, WiiConnect24 jobs) and With Anyone's Spectator are
    never enabled (`isEnableButton` answers no; they also stay hidden without a connection).
  - sqMenuMain's exit table: codes 24-27 (With Friends), 29 (Spectator), 30 (Casual, was Basic
    Brawl / Basic Versus) and 31 (Ranked, was Team Battle) all go to Orca's block in place of the
    online sequences' entries, which starts sqVsMelee, the local Versus character select, as Group >
    Brawl does. No online scene, network thread or DWC is ever entered. The menu's exit code itself
    is left as the game wrote it, and Orca reads it.
  - The back out (sora_scene, both games): sqVsMelee always went back to sqMenuMain with argument 1,
    which the menu opens as Group with Brawl selected. The block keeps the exit code in sqVsMelee's
    object (+0x18, slack in its heap block) and the back out's helper (in the online entries' old
    blocks) turns it into sqMenuMain's argument for that button, once (With Friends' 24-27 -> 30,
    29-31 -> 27-29; anything else is 1, as before). sqMenuMain's own switch (`0x80701A4C`) maps
    them to menu modes: 30 to 34, the ONLINE page (proc 3) with the cursor on With Friends
    (muProcWifi's init puts it there for 34, on With Anyone for 35; the mode's page from the table
    at `0x806F9228`), and 27-29 to 29-31, With Anyone on that button. Until `ux=18` With Friends'
    codes went back as 23-26, the With Friends page on that button. sqPrizeCheck restarts
    sqVsMelee after a match without touching the object, so the code outlives matches. Game code and
    memory: rollback re-runs it and a drop-in keyframe carries it.
  - The IOS refusals of every session stay as the backstop: NET_KD_REQ, IP/Top and SSL refuse
    in a session and each refusal is logged at INFO (`IOS_WC24`, `IOS_NET`, `IOS_SSL`).
- **Text** (`MenuText.h`): the main menu's message files, found through the menu scene's archive
  (`mu_menumain_en`, MiscData 5 and 10), are rewritten in emulated memory: Brawl's footer ("...
  while connected to Nintendo WFC."), Project+'s "(Discontinued)" and "(Project M not compatible!)",
  the ONLINE page's With Friends (message 18, since `ux=18`), With Anyone's descriptions (Casual,
  Ranked), the old With Friends page's (Friend Roster, Team Multi-Man, Home-Run Contest, and the
  Boss Battles text no button shows; that page no longer opens) and the records' "Nintendo WFC"
  mentions. A message is rewritten only while its bytes are exactly the game's own, into the same
  slot (never longer, zeros after), at every frame boundary on both machines: a pure function of
  emulated memory, covered by the UX's compatibility version (`ux=5`). The description box at the
  bottom prints from its own copy of a message, formatted when a page shows it; a menu that opens
  straight on With Friends or With Anyone (B out of an Online character select) formats it in the
  frame the archive loads, before any boundary, so the box's copy is rewritten the same way, while it
  holds the original and the panel says that message is the one shown (`MenuText.h`).
- **Textures** (`Data/Sys/Orca/Textures/RSBE01`, `Tools/orca/textures/README.md`): our own art,
  drawn by `Tools/orca/textures/make-labels.py` with SIL OFL fonts: Brawl's PLAY ONLINE label and
  ONLINE title (Anton, like the game's own light condensed titles), CASUAL and RANKED in both games,
  each 4x with mip levels down to the game's size (a custom texture's sampler takes them even where
  the game's texture has none, so 1x internal resolution reads a clean 1x level). A session forces
  texture replacement on (`Profile.cpp`) and `HiresTexture::Update` loads Orca's pack with the
  player's own: Orca's keeps the names it has, so a player's pack in `<user>/Load/Textures` (a Brawl
  HD pack, say) can never bring back "NINTENDO Wi-Fi CONNECTION" or "NINTENDO WFC", and the player's
  has every other name (`CollectHiresTextureFiles`). Orca's is always preloaded so the game's own
  label never shows while ours loads, and a session shows no "Found N custom textures" message.
  Replacing textures only changes what is drawn, so the pack is not in the compatibility key:
  `YG_HASHLOG` is identical with and without it, in both games.
- **Events** (`OnlineMenu.h`): a reader in the frame hook follows the main menu at every boundary
  that runs a frame for the first time (never a re-run) and reads the exit code the menu leaves with
  (scene manager `0x805A0060` +0x284, visible from the first scMemoryChange boundary). It prints an
  event only for a pick made while the player's game is alone (OnlineMatch's `AloneAt`): no friend
  plugged in or on the way, not joining or catching up, no keyframe wanted or being made
  (`SoloIdle`), nothing of drop-in waiting for the boundary to take it (a friend's arrival, a
  prepare-join, the app's join or leave: `Online::DropInPending`), and no keyframe stored for an
  invite nobody took yet while it is fresh (30 s: a friend who arrives then replays every frame
  since). A pick made while friends play, or while one is about to land, is theirs too: the reader
  never announces it, not even once the game is alone again (Casual and Ranked go to the session at
  that boundary instead: "Casual or Ranked with friends in the game" below). Going solo after a session, or loading a
  friend's keyframe, starts the reader over (`Events::NoteResync`), so nothing stale is ever
  printed. It only reads memory.
- **No network, proven per run**: `Tools/orca/online-menu.py` drives every page in both games and
  fails on any `/dev/net` open, any NET_KD_REQ, NCD, IP/Top or SSL request, or any NAND command on
  the save's, WiiConnect24's or the network settings' folders after the boot.

| stdout | When |
|---|---|
| `orca menu online casual` / `orca menu online ranked` | With Anyone > Casual or Ranked, with the app's `host`: the game went to the local Versus character select and the page's queue searches ("Matchmaking"). |
| `orca menu online casual kept` / `orca menu online ranked kept` | The same picked with friends in a host's game, which kept them because its page can't search that queue now (no `pick-casual` / `pick-ranked`): nothing armed, the page says why, never a search. Armed (the line again, unmarked) once the friends are gone (below). |
| `orca menu online casual local` / `orca menu online ranked local` | The same without `host` (an older app or page): nothing searches. |
| `orca menu online friends` | With Friends: the local Versus character select, where friends drop in. |
| `orca menu cancel` | Searching (or matched as the host, with nobody plugged in yet), the player backed out of the character select to the main menu, alone: the search is over. Once. |

The reader prints each line once per pick, only while the player is alone, and never carries
state. A Casual or Ranked pick made with friends in the game is the session's to announce (below).

**Casual or Ranked with friends in the game** (Luis, 2026-10-05: "Ranked/Casual under With Anyone =
drop the friends lobby and join a 2-player room the matchmaker fills"; `OnlineMatch.cpp`
`TakeLobbyPickNow`, `DecideLobbyPick`, `ComeHome`, `AnnounceHomePick`; branch `q11-lobby`). Before it
a pick made while friends played was never announced, so With Anyone > Casual or Ranked from a
shared lobby did nothing and said nothing. Now the reader hands such a pick (never With Friends) to
the session at the boundary that saw it, after the app's commands (`TakeLobbyPick`). Either
player's press picks in a shared menu, and every game sees it at the same frame and goes to the
same Versus character select (titled CASUAL or RANKED once alone: "The character select's title").
- **The host** (port 1, the app's `host`, and the picked queue's cap: `pick-casual` or
  `pick-ranked`, its page can search that queue now), with friends in its game, waiting for their
  keyframe, or arriving in the room and not taken in yet, leaves its room as the app's Leave does,
  prints `orca state left lobby` and shows "Left the lobby for Ranked" (or Casual: said as what happened, since
  the friend may have pressed it), then announces the pick (`orca menu online casual|ranked`): from
  the next boundary it plays alone on port 1, so the queue's own character select gets its header
  and Start readies (`orca queue ready`). An arrival not taken in counts as a friend: left in, it
  would be taken into a game under the queue's header and refuse it (`orca error mismatch`); now it
  hears the host left.
- **Each friend** lets the pick go at that boundary (the game is its host's), hears `orca state
  host-left`, and comes home on that character select ("Drop-in"): there it reads the pick the
  select was opened for (the sqVsMelee word `CssTitle::ReadPick` reads, 30 or 31:
  `UX/OnlineMenu.h` `CssPick`) and announces it too, so its Start readies it on its own queue
  select without backing out ("Drop-in: home on the ranked character select"). Before this it came
  home on a RANKED select where nothing was armed, and Start did nothing: the same dead end as
  YouGame's "Start did nothing", for whoever had not been host. Whatever its own page can search
  (its pick caps): it is alone, and its page answers the pick as any alone pick (its reason, or the
  search).
- **A queue the page can't search now** (YouGame, 2026-10-05: Ranked needs a signed-in player, and
  both queues sit behind the site's `orca_queue` and `orca_ranked` switches; the page answers
  `pick-casual` / `pick-ranked` in its `caps` only for a queue it can search, `lib/orca.ts`
  `orcaCapsAnswer`): leaving would drop the friends for nothing, since the page would only say
  "needs an account" or "coming soon" and cancel. The host keeps them and prints the pick in place,
  unarmed and marked (`orca menu online ranked kept`, logged "staying with them"): no queue select,
  no search lines, no queue header (a friend on the way would refuse it: `orca error mismatch`),
  and a kept keyframe stays. Its page answers with its reason and `queue-cancel`, which changes
  nothing; every game stays on the Versus character select the pick opened, the friends with it.
  A `kept` pick never searches, even if the page could by now: it says "Leave your friends' lobby
  first" (`orcaMenuPlan` `friends-first`). Once the friends are gone and the host's game is alone
  on that select (`ArmKeptPick`, `DecideKeptPick`), the pick is armed as an alone pick is (`orca
  menu online ranked`, unmarked: the queue's own select, so Start readies it); it is dropped once
  the game has gone on from that select (`PickStands`) or a queue or search is under way anyway.
  The page sends its `caps` again when what it can search changes mid-run (signed in, a switch
  flipped): Orca takes a later `caps` line whole (`Status::SetAppCaps`), and every feature is read
  where it acts, so the next pick follows the new answer. The switches and the sign-in are the
  page's server props: a change reaches a running page on its next router refresh (signing in in
  place does one), and a reload starts a new run that answers afresh.
- **Nobody to leave by now** (the app's own leave went first, the last friend has gone and the
  session only idles out its last frames, or only a keyframe is kept for an invite nobody took yet
  or a prepare-join): announced in place, no `left lobby`; a kept keyframe is dropped (a friend who
  comes later gets one under the queue's header), never while a session still runs.
- Without the app's `host` nothing would search, so the friends' game stays theirs, as before
  (nothing printed); a queue room's menus are locked.
- A pick seen at a boundary the commands can't take (a session's frame not the next one) waits for
  the next, while it still stands (the frame hook last read the way out of the main menu, or the
  character select that pick opened: `PickStands`); once the game has gone on from it (back on the
  main menu, a stage select, a fight) it is dropped, so nobody leaves their friends for a pick that
  is over.

Local: the session ends at the pick's boundary, so nothing of it reaches the friends' games but the
room's leave (no compatibility change).

Checked 2026-10-05 (branch `q11-lobby` on `rel-0.3.22`, M4, headless): the `Orca*` unit tests (453
pass, 20 skipped: the live and network ones; new
`OrcaOnlineMenuReader.APickMadeWithFriendsGoesToTheSessionOnce`, `OrcaOnlineMenuLobby.*`).
`Tools/orca/dropin-rooms.py --only home,lobby` over dev rooms PASS: step 6, the guest whose host's
app stopped heard `host-left`, came home on With Friends' character select (no `no-room`, its page
shown its own room), picked Ranked and was `orca queue ready ranked` 0.30 s after Start; the same
step against the 0.3.23 build (no fix) FAILs as Luis saw it: `no-room`, `orca menu online ranked`,
then nothing for 10 s after Start. Step 7: the host's Ranked pick with the friend in printed `orca
state left lobby` and `orca menu online ranked`, its queue select got its header and it was ready
0.30 s after Start; the friend came home on the character select the pick took it to and readied
Ranked itself. Steps 1-5 PASS (their friend went home or said `no-room`, by where it was).
`queue-e2e.mjs --q2 --queue ranked` (dev tickets) PASS: a 2-0 set, set over, both back not ready,
222 checksums matched on each side. `YG_SYNCTEST=7 YG_SYNCTEST_FROM=1400` over
`inputs/bf-reseat.txt` with `ORCA_UX_TEST_RESEAT_AT=1957` (port 2 to port 1 on With Friends'
character select, then port 1 out, With Anyone > Ranked, its character select, out again) to frame
3200: 12,551 re-run frames, 0 RAM mismatches.

Checked again after review, 2026-10-05 (branch `q11-lobby` merged with `rel-0.3.23`, `ux=18`, M4,
headless): the `Orca*` unit tests (459 pass, none failed; new
`OrcaOnlineMenuLobby.APickWithNobodyToLeaveIsAnnounced` and
`AFriendHomeOnThePicksCharacterSelectTakesThePickToo`). `Tools/orca/dropin-rooms.py` over dev rooms,
every step PASS: step 7, the host's Ranked pick with the friend in, and the new step 8, the
FRIEND's press of Ranked in the shared menu, both end the same way: the host prints `left lobby`
and `orca menu online ranked`, the friend `host-left` then `orca menu online ranked` from its way
home ("Drop-in: home on the ranked character select"), and both are `orca queue ready ranked` 0.31 s
after Start on the select they landed on, neither backing out. New step 9: a new Orca joined a
came-home guest's own room from the app's invite and played 15 s on its With Friends select: 17
checksums matched on each side, no error. Steps 1-6 as before. `queue-e2e.mjs --q2 --queue ranked`
PASS: three verdicts the same on both sides, set over, both back not ready, 225 checksums matched on
each side. The synctest over `inputs/bf-reseat.txt`, now the `ux=18` walk, with
`ORCA_UX_TEST_RESEAT_AT=1907` (FRIENDS select at 1657, then RANKED at 2644) to frame 3097: 11,830
re-run frames, 0 RAM mismatches. Screenshots of a host stopping with both tokens down on With
Friends' select: the guest came home driving port 1's panel, still showing the old host's character
("Drop-in"). Not run: Project+ (`dropin-rooms.py` has no Project+ mode), a live arrival racing the
pick (`ArrivalPending`, unit-tested only), a pick that waits for a takeable boundary (`PickStands`).

The pick caps, checked 2026-10-05 (branch `q11-lobby`, M4, headless): the `Orca*` unit tests (462
pass; new `OrcaOnlineMenuLobby.WithFriendsAQueueThePageCantSearchKeepsThem`,
`APickKeptWithFriendsIsArmedOnceTheyAreGone` and `EveryCapFitsTheAppsCapsLine`, and the pick cap in
`CasualOrRankedWithFriendsLeavesTheFriendsLobby`). Before it, the branch's own run of step 7 (caps
`join leave stats host results queue2`, no word of the page's searchability) printed `left lobby`:
the leave depended on `host` alone. `Tools/orca/dropin-rooms.py --only lobby,guest,stay` PASS: steps 7
and 8 with both pick caps as before (`left lobby`, `orca menu online ranked` on both, both `orca
queue ready ranked` 0.31 s after Start); new step 10 without `pick-ranked`: the host printed `orca
menu online ranked kept` and no `left lobby`, no queue header or queue select, the friend heard
nothing, and the page's `queue-cancel` changed nothing for 8 s; then the friend's app left, the
session ended at frame 2739 with 40 checksums matched, the host, alone on the RANKED select, armed
the kept pick (`orca menu online ranked`) and was `orca queue ready ranked` 0.31 s after Start, and
the friend came home there, took the pick without `pick-ranked` and readied it too. Step 6 PASS on
the first build of this change (before review put the guest's rule back and added `kept`). Not run: a page re-sending `caps` mid-run against a live Orca (the handler replaces the
set; unit-tested through `SetAppCaps` elsewhere).

Tests: `OrcaOnlineMenu*` unit tests (the gates, the textures and their precedence, the text rewrite,
the reader's edges and `AloneAt`); `Tools/orca/online-menu.py <nogui> <disc> [--pplus <dol>]
[--video Metal --shots <dir> [--scale 4]]` (every way through the menu, solo, offline);
`Tools/orca/online-menu-dropin.py` (a friend drops into a host on the ONLINE page and both go to
With Friends' character select at the same frame, nothing printed; with `--hold`, the join is held
first, "Boot and friends from the menus" below; with `--invite`, a pick within 30 s of an invite's
stored keyframe prints nothing and one after it prints). online-menu.py's `friends` scenario is A on
With Friends straight to the character select (exit code 25); its `back-*` scenarios hold B
out of the character select and press A on the page that comes back: the same pick (exit code) again
proves the page and the cursor (`back-friends`: the ONLINE page on With Friends; `back-casual`,
`back-ranked`), `back-chain` walks B from the ONLINE page to the top page and picks Casual,
`back-versus` checks Group > Brawl still comes back to Group. They use the harness's per-visit
rules (`@muMenuMain:2 ...`: only on the scene's second entry).

**Checked 2026-10-05, `ux=18`** (branch `q10-menus` on `9611b74874`, live 0.3.21; M4, headless,
`-v Null` unless said; runs and inputs in `q10-runs` beside the checkouts):
- **Found under the harness** (`ORCA_UX_PROBE` watches and two RAM dumps on the ONLINE page, both
  games; the module disassembled from them): A on With Friends set muProcWifi's next proc
  (`+0x634`) to `0x1A` the frame after the press, With Anyone `0x1B`, Options `0x1C`; the With
  Friends page's A (`0x811918A4`) leaves with 24-27 by its cursor through the proc's exit
  (`0x806A5604`, which stores the code and a two-frame delay in the menu, `+0xAB0`/`+0xAB4`; the
  menu's process does the exit once the delay runs out, `0x806CE67C`); sqMenuMain's argument 30 is
  mode 34, whose page byte is 3 (the ONLINE page), and muProcWifi's init puts the cursor on With
  Friends for 34 (`0x81179C2C`). Project+'s module is Brawl's at `+0x1480` word for word but for
  its calls into sora_scene.
- With the build: A on With Friends at the ONLINE page's frame 220 (Brawl 1,651) left the menu two
  frames later with 25 (the next proc never moved), the character select titled FRIENDS; B held out
  of it opened the menu in mode 34 on the ONLINE page with the cursor on With Friends (0) and its
  description "Play with friends you invite! (2-4 players)" (Metal screenshots, both games).
- `online-menu.py`, Brawl and `--pplus`: all nine scenarios PASS (tour, casual, ranked, friends,
  back-friends, back-casual, back-ranked, back-chain, back-versus; the groups at `0x81179F04` and
  `0x8117A104`, Project+ `0x8117B384` and `0x8117B584`, 39 words each, landed on the main menu; no
  network or save access).
- A on BACK, the queue's own character select (`ORCA_TEST_QUEUE=casual:solo`, the app's `caps host
  queue2 locks` on stdin): not ready, a new A on BACK left for the menus (Brawl at the select's frame
  203; Project+ 183) and printed `orca menu cancel`; ready (`orca queue ready casual 0 0`, Project+
  `8 0`), one A on BACK printed `orca queue unready` at the press's boundary (the gate kept it from
  the game, `FLAG_BACK_A` set for one boundary) and the game left on the next frame (Brawl 2,088 then
  2,091; Project+ 825 then 828), then `orca menu cancel`. An A held since before the hand reached
  BACK did nothing on it, its next press backed out; a casual room's header (`ORCA_TEST_QUEUE=casual`)
  kept A masked on BACK (the select stayed).
- `YG_SYNCTEST=7`, **0 RAM mismatches** (no `ram DIFF` line in any run; the device-state-only ones
  are the usual icache): Brawl from 1,440 to 2,400 (6,671 re-run frames each) over ONLINE > With
  Friends > the character select > B > the ONLINE page > With Friends again; over A on BACK idle;
  over ready then A on BACK; and over a friend's plug-in on the ONLINE page (`ORCA_UX_TEST_NAMES=ada,bo
  ORCA_UX_TEST_PLUG_AT=1650`: the move at 1,680, B back to the ONLINE page on With Friends, no second
  move). Project+ the same four from 200 to 1,200 (6,951 each; the move at 450).
- Unit tests: 463 `Orca*`, 443 pass, 20 skipped, 0 fail (new `OrcaOnlineRules
  .AOnBackBacksOutOfTheQueuesOwnCharacterSelectOnly`, `OrcaQueue.AOnBackWhileSearchingUnreadiesThenBacksOut`,
  `OrcaInputGate.TheStickCentresWhileAIsDown`, `OrcaOnlineMenuText.TheOnlinePagesWithFriendsPromisesNoRoster`;
  `OrcaOnlineMenuGates` checks the 39-word groups and the helper's new words).
- `online-menu-dropin.py` over a real dev room (two Orcas): Brawl PASS (the friend loaded the host's
  keyframe of frame 2,163 on the ONLINE page; both moved at 2,261; 12 checksums matched each, 0
  rollbacks, nothing printed), Project+ PASS (keyframe 1,211, both moved at 1,300, 12 each).
- After review, an A held onto BACK while ready (searching): the queue centres the stick only for a
  new A (raw A up on the frame that just ran), as the rules do, so the hand moves on. Ready, A
  pressed off BACK and held, the hand slid onto BACK and off it again, A let go: the next A there did
  nothing (the hand had left), and a later move onto BACK and A backed out (Brawl `orca queue
  unready` at 2,248, Project+ 985). The same walk without the slide off backed out at that next A
  (Brawl 2,188, Project+ 925), so the hand had been on BACK with A held. `YG_SYNCTEST=7` over the
  first walk: Brawl 1,440-2,500 (7,371 re-run frames), Project+ 200-1,100 (6,251), 0 RAM mismatches.
- Not run here: the two-Orca perf gate (`perf-gate.py` against 0.3.21, the release's: this touches
  the frame hook's masks and the `.patches`), `--hold`/`--invite`, the queue's own select under a
  real queue (`queue-e2e.mjs`), and Mac <-> PC.

### Boot and friends from the menus

Luis, 2026-10-04: booting Brawl or Project+ lands on the game's main menu (the home menu), never on
Versus' character select; a friend who joins from the YouGame page meets the host there and both go
to With Friends' character select together.

**What sent each game to the character select at boot** (proved before changing anything):

- **Project+: its own codeset.** "Boot Directly to CSS v5.4" (`Source/Project+/BootToCSS.asm` on
  the SD card, assembled into `NETBOOST.GCT`, which the codeset's expansion loader reads to
  `0x80550010`) hooks `sqBoot::setNext` at `0x806DD5F8` and starts `sqVsMelee` in place of the
  game's `sqPrizeCheck` unless a pad holds L/R (Training), Start (the title) or Z (Replays) right
  then. With Orca's save-prompt group the session went "straight to character select at frame 150".
- **Brawl: nothing.** `RSBE01.patches` already skipped the strap, the save prompt and the title, so
  Brawl reached the main menu by itself (frame 1,430 with no input: the strap scene waits for
  its 38.7 MB `/tmp` copy until 1,244, the title is gone by 1,413). Luis's recent sessions were
  Project+.
- **Not the background room.** The room a host's Orca opens at boot (`lobby`, for the page's invite
  panel) changes nothing in the game: solo, every other port reports no controller, nothing is
  written to memory for it, and no command from the page moves the game.

**Boot** (`PPLUS32.patches`): one guarded group over `NETBOOST.GCT`'s words in memory
(`0x80559C20`, four words, the middle two changed) makes the code's no-special-input case its Start
case: `sqPrizeCheck` with `0x14`, which goes on to `sqTitle` and the main menu as vanilla Brawl
does.
The hook's branch and the `nop` after it are the code handler's, written again every frame; the C2
code's own words are not (only its last word, the branch back), so the group lands once, like any
guarded group ("Game patches"). The title is skipped as in Brawl (the same two `scTitle` words of
sora_scene, which Project+ keeps); the prizes were already skipped by the codeset's own
`Netplay.asm`. No save prompt, no save written, everything unlocked as before. Project+ reaches its
main menu at frame 197 (scBoot 122-159, the title 163-180), where its character select used to
come at ~150-163. The harness's Project+ inputs press A through VERSUS
(`@muMenuMain:1 mash ...`), as Brawl's always did, and `online-menu.py`, `free-space.py` and
`queue-e2e.mjs` start Project+ from the top page.

**A friend from the menus** (`UX/OnlineMenu.h` `FriendsMove`): the frame hook, first runs and
re-runs, as a pure function of memory and the session's synced ports, both games:

- A friend plugs in (a port 2-4 newly in the synced plan) while the host's game is anywhere but
  Versus (`sqVsMelee`, `sqSpMelee`, `sqToMelee`, `sqQuMelee`): pending. Once the main menu has run
  for `MENU_SETTLE_FRAMES` (30) with its pages built (`MENU_BUILT`, below) and a friend pending, the
  hook does what every menu exit in the game does (`muMenuMain`'s at `0x81176828` included): the
  scene manager's exit code (`+0x284`) 25, With Friends > FIGHT!, then its process step (`+0x288`)
  2. The manager ends the menu and `sqMenuMain::setNext` sends 25 through Orca's block to the local
  Versus character select ("Online menu"), so B from there comes back to the ONLINE page with With
  Friends selected (before `ux=18`, the With Friends page with FIGHT! selected). Both machines do it
  at the same boundary; the friend's game shows the host's menu for half a second first.
- A friend who plugged in during the boot is taken along once the menu has built and run 30 frames;
  one who plugged in on Versus' character select (or in a Versus match) is already where friends
  play, and nothing happens. B back to the main menu with the friend still in stays there: only a
  newly plugged port counts. Since `ux=19` that select is online: its CPUs go and a friend whose
  panel had one takes it at once ("No CPUs online", the join).
- Never in a queue room (a header of casual or ranked): its opponent plugs in on the character
  select. What happens with a friend plugged in is never announced (`orca menu`), as before.
- **Never before the menu has built its pages** (branch `q9-friends-jump`, `ux=17`). The main menu
  loads its files for a while after it starts, then builds every page in one frame: muMenuMain's
  byte `+0xAC8` (`MENU_BUILT`) is 0 from its start (the step going 1) and 1 from that frame on.
  Through 0.3.19 the 30 frames counted from the step: a friend who plugged in during the host's boot
  (or while a menu coming back from the character select loaded) moved both games before the build,
  and both froze for good (below). Now they count from the build: the move comes 30 frames after it,
  so the friend sees the host's menu, drawn, for half a second.
- State in the match block's last free bytes, `+0x290..+0x297` (`MatchBlock.h` `FRIENDS_*`: a tag,
  the ports seen at the last boundary, the pending flag, the frame the menu was first seen built and
  running with a friend pending). `+0x180..+0x18F`, first planned for this, is Project+'s
  `SAVED_RSS`. Solo play never writes them (nothing kept and no friend: no write), so
  `free-space.py`'s runs are untouched; the first friend to plug in is news from the dead code's
  bytes on. Ports plugged in before frame 300 (`FIRST_FRIEND_FRAME`; no friend plugs in that early,
  a keyframe is never of the host's first 300 frames) were there from the start: a harness's
  `ORCA_UX_TEST_NAMES` ports never move a run. Run again at the same boundary on what it wrote, the
  hook writes the same (`OrcaOnlineMenuDropIn` tests).

**A join held in a single-player mode** (`Rollback/OnlineMatch.cpp` `HostEvents`): while the host's
game is in Classic, All-Star, Boss Battles, Events, Training, Home-Run Contest, Target Smash,
Multi-Man Brawl, the Subspace Emissary or the Vault's screens (`SequenceHoldsDropIn`, the scene
manager's sequence at `+0x10`), a friend who arrives waits: no keyframe is made and nobody is taken
in. The host prints `orca state friend-holding`; the room tells every joiner without a keyframe
(`{"k":"hold","on":true}`, again every second while it holds, `false` once), and the joiner prints
`orca state friend-waiting` and shows "<host> is playing a single-player mode: you'll join when
they're back in the menus". The join's 90 s limit doesn't run while held. Back in the menus the
host prints `orca state friend-joining` again and the join goes on (the joiner: `orca state joining
0`, then its percentages); the friend lands on the main menu with the host, then both go to With
Friends.

| stdout | Who | When |
|---|---|---|
| `orca state friend-holding` | host | A friend waits to join while this game plays a single-player mode. |
| `orca state friend-waiting` | joiner | The host's game is in a single-player mode: this join waits. |

Harness knobs (test runs only): `ORCA_UX_TEST_PLUG_AT=<frame>` plugs the ports after the first in
from that frame (port 1 alone before), as a friend's drop-in does at its frame S, for synctests of
the move; `ORCA_UX_TEST_UNPLUG_AT=<frame>:<port 2-4>` (`ux=19`) unplugs that port from that frame,
as a friend's leave does: out of the frame hook's ports, and its pad reports no controller (the
session's `UNPLUGGED_PAD`); `ORCA_UX_TEST_PADS_FOLLOW_PLUGS=1` makes every test port outside the
frame hook's ports for a frame (beyond `ORCA_UX_TEST_NAMES`, or before `ORCA_UX_TEST_PLUG_AT`)
report no controller, as a session's do, so a friend's controller appears to the game as they plug
in (without it the harness's four pads are always connected; not with `ORCA_TEST_QUEUE_PICK`);
`ORCA_TEST_HOLD=<from>-<to>` holds joins over those frames as if the host played a
single-player mode (`online-menu-dropin.py --hold`).

Measured 2026-10-04 (M4, headless, fresh user folders):

- A friend's plug-in on the main menu (`ORCA_UX_TEST_PLUG_AT=1600` in Brawl, `320` in Project+)
  moves both games 30 frames later (1,630; 350) and the character select comes 4 frames after
  that (1,634; 354). `YG_SYNCTEST=7` over the plug-in, the move, the character select, the stage
  select and the match start: Brawl 1,550-2,800 (8,701 re-run frames), Project+ 300-1,500 (8,351),
  **0 RAM mismatches** (the device-state-only ones are the usual icache).
- `online-menu-dropin.py` over a real dev room, Brawl: PASS (the friend loaded the host's keyframe
  of frame 2,149 on the ONLINE page; both moved at 2,248 and reached the character select at 2,253;
  12 checksums matched; nothing printed). `--hold`: PASS (held ~34 s, from the friend's arrival to
  the host's frame 4,200; then the keyframe, the join and the move at 4,305 on both).
- `online-menu.py --pplus` from the new boot: casual, ranked, friends-0, back-casual and
  back-versus PASS.
- The JIT warm-up's seed lists apply whole on the new paths (no synctest, through With Friends to a
  match): Brawl 31,534 of 32,625 blocks compiled ahead, 66 already compiled, 1,025 not in RAM (the
  list's Donkey Kong and Final Destination, as before); Project+ 27,752 of 28,054 ahead and the
  other 302 already compiled. What the game compiled itself were blocks no list holds (none "not
  reached yet" but 9): in Brawl's loading frames (0-169) 106, 90 of them at frame 169; in
  Project+'s loading image (frames 0-27) 154, 101 at frame 18; afterwards a few a frame, and
  ~1,000-1,400 more once the scripts' fuzz begins (moves the recorded matches never made, there
  with either boot). Nothing here needs the lists re-recorded.

**The freeze when a friend joined during the boot** (0.3.18 and 0.3.19, Brawl; branch
`q9-friends-jump`). A friend who dropped in while the host booted (`occluded-test.py --friend`
plugs in at 314, `direct-link.py --quick` at ~980) stopped both games at frame 1,474: the move at
1,473, then `Invalid read from 0x00000008, PC = 0x806cec6c`, and no frame after it. Proved under
the harness, one Orca, `ORCA_UX_TEST_NAMES=ada,bo ORCA_UX_TEST_PLUG_AT=314` (the same freeze at
the same point, in two seconds of emulation; a session's frame hook numbers that boundary one
lower than the harness, whose move is at 1,474), with `ORCA_UX_PROBE` watches and a dump of
sora_scene's code read at run time:

- `0x806CEC6C` is in muMenuMain's exit (`0x806CEBF8`), which the manager runs once the step is 2.
  It frees three objects (`+0x7B4..+0x7BC`), then for each of the 29 page procs at `+0x35C` that
  isn't 0 calls its destructor through the vtable at proc `+0x668`, then releases the tables at
  `+0x738` and `+0x3E0`.
- At the move (the menu's frame 43, its step 1 since frame 13) every proc slot still held the
  heap's fill, `0xCCCCCCCC`: not 0, so the exit called through it, read a null vtable and faulted.
  The menu's process (`0x806CE730`) calls its build (`0x806CD510`) every frame until its files are
  in; the build makes every page, the procs and the rest in one frame and sets `+0xAC8` to 1
  (`0x806CE194`). Watched with no move: built at frame 1,502, 58 frames after the step went 1 at
  1,444; coming back from the character select 121 frames after it (1,862 to 1,983); Project+ 1
  frame after it (its files load at once), so Project+ never froze, though its move runs the same
  code. Before the step is 1 the byte is the last menu's (1), which the start zeroes.
- Not a page transition: an idle host froze the same way (no press on the menu at all), and with the
  fix, moves landing in every page transition and on the game's own exit pass (below). Not the
  settle being short as such: no fixed count is right when the load takes 1, 58 or 121 frames.
- The same freeze, by other addresses, on a menu coming back from the character select with a
  friend plugging in while it loads (`ORCA_UX_TEST_PLUG_AT=1850` over a B out of the character
  select): 0.3.18 moved at 1,892, its exit freed the last menu's leftovers (`0x8002DF50`,
  `0x8019D8EC`); the fix moves at 2,013, 30 frames after the build at 1,983.

Checked 2026-10-05 (M4, `ux=17`): `OrcaOnlineMenuDropIn.AMenuStillLoadingIsNeverLeft` and the other
419 `Orca*` tests pass. Harness (its frame numbers), Brawl: the boot plug-in moves at 1,532 (built
1,502) with the host idle or mashing A (20 placements of A, B and DOWN around it; the boot's first
menu takes no press before about 1,540 anyway). 30 plug-ins from 1,505 to 1,562 (moves at
1,535-1,592) land the move in the Group page's way in (A at 1,540, the page's proc at 1,542), its
way back out with B (1,562), a cursor move, A on Solo (1,592), and the frames around the game's own
exit at 1,583: no invalid access, every one reaches the character select. Project+: a plug-in at 300
still moves at 330, one on a menu coming back from the character select at 649 (built 619).
`YG_SYNCTEST=7 YG_SYNCTEST_FROM=1400` over the boot plug-in, the move, the character select, the
stage select and the match to 2,800: 9,751 re-run frames, **0 RAM mismatches**. Two Orcas over a
real dev room: `direct-link.py --quick` PASS (the friend in at 981, both moved at 1,531, the match
at 2,081, 59 checksums matched each), `occluded-test.py --friend` PASS (in at 314, moved at 1,531,
every phase 59.9 fps, 201 checksums matched each), `online-menu-dropin.py` PASS (the ONLINE page:
moved at 2,248, as before, 12 checksums). A host on Versus' character select, or a queue room's
header (`ORCA_TEST_QUEUE=casual`), still never moves.

## Matchmaking

Orca 0.4.0 plays a stranger the queue found: With Anyone > Casual or Ranked in the game's own
ONLINE menu, the YouGame page's queue (the rooms Worker's `/match`, Elo and distance windows, the
same Orca build and disc to meet), then the existing drop-in into the matched room. Ranked is a
beta: a best of 3, rated once per set, with the host's rules (YouGame `docs/ORCA_ONLINE_UX.md`).

### The flow

1. The pick prints `orca menu online casual|ranked` (no `local`: the app's `caps` has `host`). The
   game is on the local Versus character select; the overlay shows "Searching for an opponent…
   (B: back)", with "RANKED BETA" over it for Ranked, until `host`, `join`, `queue-cancel` or a
   back-out. B out of the character select to the main menu, alone, prints `orca menu cancel`.
2. The page queues a ticket in the queue's mode (`orca-rsbe01-ranked`, `orca-pplus32-casual`...)
   with Orca's own lobby exactly (the compatibility key). On `matched`, slot 0 gets `host <code>`
   and the other `join <code>` 1.5 s later.
3. `host <code>`: only while this game is alone (no friend plugged in or on the way, not joining or
   catching up, no keyframe wanted or being made, nothing of drop-in waiting), else
   `unsupported host`. The game leaves its room and hosts `<code>` where it is (normally the
   character select): Orca's ticket for that room goes through the bridge's `mpTicket` with
   `room: <code>`, which the page mints as the queue's ticket with `match_room` (the keyframe store
   needs a ticket signed for that room). A former joiner moves its pad to port 1 first: solo, so
   only its own game changes, and the next keyframe carries it. Once the room's welcome says its
   queue, the host's next boundary writes the match block's header (its next alone one, or one with
   the opponent on the way: `HeaderFreeAt`), and the locks follow
   ("Online rules": the ruleset's rules in both games, casual's stage pick ("Casual stage pick"), no
   way out of the character and stage select; `ux=8`, which replaced the beta's Brawl-only rules
   preset; `ux=9` adds the character select's A gate; `ux=10` queue2 and the casual stage pick). A
   keyframe for the opponent waits for that header. The overlay says "Opponent found · waiting for
   <name>…" until the opponent plugs in.
4. `join <code>` is the existing drop-in: the joiner lands on port 2 of the host's character
   select.
5. Once the opponent is plugged in past its catch-up (`friend-joined`), the host's Orca begins the
   room's match (`{"t":"begin","revision":R}`: room host on seat 0, no match running, two
   participants, nobody away; on a `command-error` it tries again at the next roster change, five
   times at most). Both Orcas learn the match id from `start`.
6. Each game's result is read from the game's memory as its frames become final and reported by
   both Orcas, the same bytes (below). Casual: each game is a `finish`, the room stays, and the host
   begins the next match. Ranked: each game is a `game-report` (`g1`..`g7`); when one player has 2
   agreed wins (`game-result` verdicts), both send `finish {winner}`; 7 games without that, `finish
   {void}` ("limit"). The room settles each report by agreement on the outcome (winner, draw,
   void): different outcomes void the game; a detail read differently does not (since YouGame PR
   #489 the room keeps the lowest slot's, the host's, and logs both). It rates the set once.
7. After a ranked set's verdict the room stays open (`set-over`, Orca 0.3.17, `ux=13`: "Set over"
   under "Set end on screen"): both games stay in their session on the results screen, with the
   page's chat, until each player leaves; each Orca leaves the room by itself (as the app's `leave`
   does) and its own character select comes back not ready. Only a room still open five minutes on
   closes with 4004 "match-over": a clean end. The host then prints `orca state friend-left
   match-over` and opens a fresh private room (never the closed code); the joiner prints `orca
   state host-left match-over`, moves to port 1 and opens its own room too.

Leaving: `leave` in a ranked set is a forfeit (the room's rule, 15 s). The survivor of a ranked set
whose opponent went (the room still holds a match without a verdict) stays in the room with no
session until the room's verdict (a forfeit or void, then `set-over`, and it leaves) or 30 s, then opens a room of its
own. In casual the host leaves the queue's room for a fresh private one as soon as the opponent is
gone. A desync voids the match in progress (`finish {void}`) before the players part.

### Lines

| stdin | |
|---|---|
| `host <code>` | With `caps host`: host the matched room (above). An invalid code, or a game that isn't alone: `unsupported host`. |
| `queue-cancel` | With `caps host`: the page's search ended; the overlay's search lines go. Nothing else changes. |

| stdout | |
|---|---|
| `orca caps ... host results locks queue2 pick-casual pick-ranked` | `results` only where the result reader is verified (Brawl rev 2, Project+); the room hears the results either way. `locks` where the profile has a ruleset (the same two): queue rooms play the online rules ("Online rules"). Information only: the locks don't wait for the app's answer. `queue2` and the pick caps with `locks`: the page answers `pick-casual` / `pick-ranked` only for a queue it can search right now, and again on a later `caps` line when that changes ("Casual or Ranked with friends in the game"). 15 words at most; the app's filter passes 16, and more takes a desktop release first (`OrcaOnlineMenuLobby.EveryCapFitsTheAppsCapsLine`). |
| `orca menu online casual\|ranked kept` | A host kept its friends for the pick (no pick cap): nothing armed, never a search. |
| `orca menu online casual\|ranked` | The pick, with `host` agreed (`... local` without). |
| `orca menu cancel` | The player backed out of searching (above). |
| `unsupported host` | The `host` was refused. |
| `orca result <json>` | With `results` agreed: the room's verdict, when it arrives (not at this Orca's own read). At most 300 bytes. |
| `orca state friend-left match-over` / `orca state host-left match-over` | A ranked room closed after its result (since 0.3.17 only at set over's five-minute cap). |

`orca result` shapes (the local player first; `n` the game's number in the casual pairing or the
ranked set; `score` agreed wins; `d` the settled detail, the host's):
- casual game: `{"q":"casual","k":"game","n":3,"out":"won"|"lost"|"draw"|"void","why"?:"mismatch"|"nocontest"|"ports"|"unrecorded"|"desync","d"?:{"st":2,"c":[mine,theirs],"s":[mine,theirs],"to":0|1}}`
- ranked game: `{"q":"ranked","k":"game","n":2,"out":...,"why"?:...,"score":[mine,theirs],"d"?:{...}}`
- ranked set: `{"q":"ranked","k":"set","out":"won"|"lost"|"void","by":"games"|"forfeit","why"?:"desync"|"limit"|"unrecorded","score":[mine,theirs],"r"?:[before,after],"rank"?:"<24 chars>"}`

The overlay toasts a ranked game's: "You won game 2 · 1–1". A set's verdict, and a casual game's,
show as panels instead: VICTORY or DEFEAT with the rating's change, "<name> forfeited", "No contest"
("Set end on screen").

### Room messages (queue rooms only: the welcome's `room.queue` casual or ranked)

- Read: `room.queue`, `room.stay` (welcome); `participants` (`id`, `connectionId`, `slot`),
  `players` (`away`), `revision`, `matchId`, `host` (welcome, join, leave, back, away, lobby, start).
  The page's mirror (`mpRoom`) carries the real `queue` and `matchId`.
- Sent, both Orcas the same bytes: ranked `{"t":"game-report","matchId":M,"id":"g<n>","winner":"<participant id>","detail":D}`
  (or `"draw":true,"detail":D`, or `"void":true`); casual `{"t":"finish","matchId":M,"winner":...,"detail":D}`;
  a ranked set `{"t":"finish","matchId":M,"winner":"<id>"}` or `{"t":"finish","matchId":M,"void":true}`.
  D is `{"f":F,"st":S,"c":[c1,c2],"s":[s1,s2],"to":0|1}`, integers in port order, these keys in
  this order: F the session frame of the first final results-screen reading (the same on both
  machines), S the stage, c the characters, s the stocks left (-1 in a timed match), to the fight
  ran out of time. A report waits for the room's match (60 s at most). Friends rooms never begin
  or report.
- Handled: `start` (the match id), `game-result` (a ranked game's verdict: the set's tally),
  `result` (a set's or a casual game's verdict), `set-over` (the room stays open after the set:
  "Set over" under "Set end on screen"), `command-error` (logged; a refused begin tries again; in
  set over every lobby command is refused, "The set is over"), the socket's 4004 close.

### Results (`Core/Orca/UX/Results.*`, read only)

At every frame boundary, first runs and re-runs, the frame hook stores a small reading of the
frame just run: the scene (`scMelee` the fight, `scMemoryChange` between scenes, `scVsResult` the
results screen) and, on the results screen, the result info. OnlineMatch consumes the readings in
frame order once their frames are final (`Session::ConfirmedFrame`, at a boundary that isn't a
re-run, so every rollback has been re-run), so both machines see the same readings: a re-run's
reading replaces the guess, and nothing is written to the game. A game is a fight entered at a frame
after the opponent's plug frame (the same on both machines: a fight running when the joiner dropped
in never counts), ended by the results screen (its verdict) or by any other scene (void
"nocontest"); back into the fight from between scenes (sudden death) is the same game. Going solo
or loading a keyframe forgets everything read (`Events::NoteResync`).

The verdict: exactly two human players on ports 1 and 2 (else void "ports": a CPU, a third port),
a fight that ended normally (the pause menu's L+R+A+Start is void "nocontest"), one of them placed
first (a win; both first, a draw). Port s is seat s and room slot s, which names the participant.

Where (Brawl rev 2; Project+ runs the same executable), from the probes (ORCA_UX_PROBE dumps of
scripted matches won by each port, a timed match, a quit, two stages):
- g_GameGlobal `0x805A00E0` -> `0x90181300`: +0x08 gmGlobalModeMelee (`0x90180F20`: +0x1B the
  stage, +0x20 the time limit in frames, +0x98 the players' init data, 4 x 0x5C: +0x00 character,
  +0x01 player type 0 human / 1 CPU / 3 none, +0x04 starting stocks, 0xFF in a timed match);
  +0x18 the result info (`0x9017F420`); +0x1C the rules (`0x9017F360`).
- The result info, filled as the fight ends (before the results screen's first frame), much of it
  live during the fight: +0x01 mode (0 time, 1 stock), +0x04 time limit (minutes, u32), +0x08
  stocks (u32), +0x0C the stage (u16), +0x0E..+0x0F the count of players tied for first (read by
  the beta as how the fight ended, 1 normally and 2 a no contest; a level time-up's sudden death
  also leaves 2, so the no contest is now the decision's), +0x1378 the decision (9 from the fight's
  start, so a no contest; 1 the frame time runs out; 2 a deciding KO). One record per port at
  +0x24 + port x 0x2AC: +0x00 character, +0x01 player type, +0x0A stocks left (0xFF timed),
  +0x0E place (0 first; a 1v1's loser 1), +0x10 KOs (u32), +0x14 falls (u32), +0x18
  self-destructs (u16).
- The rules (gmSetRule): +0x02 mode (0 time, 1 stock), +0x03 a timed match's minutes, +0x04
  stocks, +0x08 a stock match's time limit in minutes (0 none), +0x0C item frequency (0 none). The
  game copies them into gmGlobalModeMelee when a match starts, so a write on the character select
  holds for the next match. The fresh save's defaults (a session's empty NAND): a 2-minute timed
  match, 3 stocks, items on. Project+'s: 4 stocks, 8:00, items off.

Checked: the harness (`YG_RESULTS=1` logs each game as the reader sees it) on Brawl wins by port 1
and port 2, a timed match, a pause-menu quit (void "nocontest") and Project+'s scripted 1v1, the
latter also under `YG_SYNCTEST=7` (0 RAM mismatches, the same result); `OrcaResults*` and
`OrcaRankedSet*` unit tests; `Tools/orca/queue-e2e.mjs`.

### Tests

`Tools/orca/queue-e2e.mjs <nogui> <disc> [--pplus <dol>] [--queue ranked|casual] [--scenario cancel]`
(and with `--q2`, `--scenario kill`: "Set end on screen"):
two headless Orcas (one for `cancel`), each behind a stand-in for the desktop app's bridge that
plays the page's part exactly (caps, Orca's own tickets, the queue ticket and `/match`,
`host`/`join`, the `match_room` rewrite) and serves the app's controller stream, whose pad the
script presses through the menus, the character select and the stage; in the fight port 2 walks off
the stage until it has no stocks left. It checks the stdout sequence and that both Orcas print the
same verdicts, each from its side. Dev tickets by default (nothing recorded; a dev game of the
run's own, so checks at once never meet); `--slug <listing> --cookie-a/--cookie-b <file>
--id-a/--id-b <id>` uses real tickets as two signed-in players. Run it as a check in the run queue
(`Tools/orca/run-lock.py check -- node Tools/orca/queue-e2e.mjs ...`), never while Luis plays.

## Free space

The online rules (casual locks, ranked sets) need room in the game's own memory: a match block of
about 96 bytes (the header and the set state, which every rollback snapshot and drop-in keyframe
must carry), and code caves for the lock patches. Both come from one place, the same in Brawl rev 2
and Project+ v3.2: the dead code of Brawl's online sequences in sora_scene. The addresses are
constants in `Core/Orca/UX/FreeSpace.h`; nothing uses them yet.

| What | Where | Size |
|---|---|---|
| Match block (data only) | `0x806F2380`-`0x806F2580`, in sqNetAnyOkiraku::setNext; it can grow to `0x806F2618` | 512 bytes |
| Code caves | 12 runs from `0x806F2674` to `0x806F82A4` (the header lists them) | 16,908 bytes |

### Why it is free

- **sora_scene is resident and shared.** The game loads `sora_scene.rel` (module 1) once at boot,
  at `0x806BB480` (text `0x806BB554`-`0x806F8ADC`), and never reloads it. Project+ ships no copy of
  its own, and its other modules don't refer to these addresses.
- **Nothing enters the online sequences.** sqNetAnyOkiraku (With Anyone's Basic Brawl),
  sqNetFriendList, sqNetFriendMelee, sqNetFriendKumite, sqNetFriendHomerun (With Friends),
  sqNetAnyTeamMelee and sqNetAnyWatch start only from sqMenuMain::setNext's entries for exit codes
  24-27 and 29-31, which Orca's patches already send to its own block ("Online menu").
- **Nothing else refers to their code.** Read from the RELs' relocations:
  - every reference into these runs, in sora_scene, in every other module on the disc and in
    Project+'s modules and plugins, comes from those sequences' own vtables and state tables;
  - only the sequences' own code reads those tables.
- **Not free, and left out:**
  - each sequence's create function, which runs at boot when sora_scene registers its sequences;
  - the 25 words the loader relocates against sora_melee (module 27). sora_melee is loaded at boot
    and stays, so they hold the same values all along, but a relink would rewrite them;
  - `0x806F3DA4`, which Project+'s codeset nops every frame ("Online Handicap Disable", in
    sqNetFriendMelee), so every cave word is the same in both games. That nop is the only Project+
    code in these runs (its NETPLAY and NETBOOST codesets parsed, every direct write checked).

### Proven under the harness

`Tools/orca/free-space.py <nogui> <disc> [--pplus <dol>]` runs one scripted loop through every scene
online play visits: boot; PLAY ONLINE > With Anyone > Casual; B out of the character select
(Orca's back out); Ranked; two whole matches with their results; B out; the top page;
Group > Brawl; B out. It runs that loop twice:

- **observe:** every word of the block and every 64th byte of the caves are watched at every frame
  boundary, and MEM1 is dumped at 16 points in Brawl, 17 in Project+ (MEM2 at 5). The block and
  the caves hold the game's own code in every dump, the same bytes, and no watch moves after the
  module loads.
- **trap:** 5 frames into the first scene the block and every cave are filled with illegal
  instructions (primary opcode 0, every word different). No unknown instruction ever runs, the
  fill is intact in every dump, and no watch moves after it. A drop-in keyframe taken in the first
  match carries it (`YG_KEYFRAME_TEST`: loaded, and 30 re-run frames identical).
- **both:** the two runs reach the same scenes at the same frames, and every dump is byte-for-byte
  the same outside the block and the caves, in MEM1 and MEM2. Nothing the game does reads them.

Results on 2026-10-03 (branch `rs-spike` on the matchmaking tip, Apple Silicon, `-v Null`):

| Game | The loop | free-space.py | Rollback re-runs with the fill (`YG_SYNCTEST=7`, scratch run) |
|---|---|---|---|---|
| Brawl rev 2 | 33 scene changes, to frame 20,014; matches of 2:00 | PASS: 16 points, the fill seen from frame 74, keyframe at 5,000 | frames 1,400-3,600 (title to the match's start): 15,351 re-run frames, 0 RAM mismatches |
| Project+ v3.2 | 33 scene changes, to frame 11,137 | PASS: 17 points, the fill seen from frame 8, keyframe at 4,000 | frames 150-1,800 (boot to the match): 11,501 re-run frames, 0 RAM mismatches |

- **Project+ against Brawl:** in the same loops, every word of the seven runs and the create
  functions is the same in both games except `0x806F3DA4`, Project+'s nop.
- **A wider fill:** a scratch run in Brawl filled the seven whole runs (18,944 bytes, the short
  pieces between relocated words included) and gave the same result.
- **The loader:** the dumped code equals sora_scene.rel's own bytes except at relocation sites,
  which confirms the load address.

### Using it

- **Before Orca writes it, the block holds code**, never zeros (its first word is `li r17, 0`,
  `0x3A200000`). A reader checks the block's magic and treats anything else as no block.
- **The block is data only.** Orca's frame hook writes it, as a pure function of memory on both
  machines; no patch group ever does (a unit test checks the shipped files). It is 32-byte aligned,
  so it shares no cache line with a cave.
- **A cave is a guarded group** in `Data/Sys/Orca/<profile>.patches` whose originals are the dead
  code that is there (`GamePatches.h`). It lands once per boot, the same in both games, and any MEM1
  address is a relative `b`/`bl` away (MEM1 is 24 MB, the branch reaches 32 MB).

### Places that don't work

| Candidate | Why not |
|---|---|
| sqVsMelee+0x18 (its heap block's slack) | Orca already keeps the exit code there ("Online menu"); 4 bytes are left. |
| `0x806DC8C8`-`0x806DC91C` | sqMenuMain::setNext's old online entries: Orca's own block and back-out helper. |
| The record's event area, `0x9017B96C` | Save data (gmGlobalRecord's Event scores): the game loads it from the save, writes it back, and the Events menu shows it. Ruled out for what it is, not tested. |
| Low MEM1, `0x80001800`-`0x80003000` | Free in Brawl under Orca (8 bytes at `0x80001804`), but Project+'s code handler fills `0x80001800`-`0x80002160` and its codes keep variables there (`0x800022A4`, MUSIC_SELECT_STEP `0x80002810`). |
| The Network heap | Both games decompress fighter files in it (the design's research). |
| The online scenes (scNetFriendList, scNetWatchBet, scNetTimeResult), `0x806C6998`-`0x806C7F18` | Most likely dead too, but which sequences create them was not traced. A reserve. |

## Input gate

The rules for queue matches take input away from a player: the port whose turn it isn't on a stage
strike, B on the stage select, a locked character. The gate does that per port and per frame where
the game reads its controllers, so no game code has to be found and patched for each control
(`Core/Rollback/InputGate.*`).

- **What a mask does.** For one port: buttons the game sees released (L and R also zero their
  analog trigger, A and B their analog value) and either stick centred. `ALL` leaves a connected pad
  with nothing pressed. An unplugged port stays unplugged, and the origin and error bits are left
  alone.
- **Where.** `Rollback::InputOverride`, which `SI_DeviceGCController::GetPadStatus` calls for every
  poll and for the SI relatch. Everything else stays raw: the local pad sampled for the wire, the
  session's pads (`RingPort`), and the harness's recording (`YG_PADREC`, through
  `RawInputOverride`). So both machines apply the same masks to the same pads, a re-run applies
  them again, and a loopback replays a recording made under the same masks.
- **When.** The rule code registers one source (`InputGate::SetSource`): a pure function of
  emulated memory. The frame-boundary hook evaluates it as its last step, just before the SI
  relatch: after the frame callback, after the session's step (a rollback's load, a joiner's
  keyframe) and after the harness's (a sync test's or a keyframe test's load). So a frame's masks
  always come from the state that frame starts from, on every pass, and nothing of them outlives a
  load: there is no host state to carry in snapshots or keyframes. A boundary where the game isn't
  running yet (a launcher's hook before it goes live) clears them.
- **Not from the frame callback.** It runs before the session's step at the boundary, so a load
  can follow it: a rollback's (OnlineMatch runs the callback again), and a sync test's, a keyframe
  test's or a joiner's keyframe (nothing runs it again). Masks set there would belong to the frame
  before the load. The source may not write memory either (the boundary's snapshot is taken by
  then): what it needs from the session's synced data (the ports in a seat plan, names) the frame
  callback writes into memory earlier at the same boundary, and the source reads it back.
- **Compatibility.** No source is registered yet, so this build computes exactly what one without
  the gate does. A source changes what the game computes: bump `UX::kCompatVersion` with it.
  Friends rooms register none, or one that returns no masks. (Since `ux=11` every session's game,
  friends and solo too, masks one thing: Z on the results screen, "The results screen's replay".)
- **The stick centred while A is down** (`Mask::a_centres_stick`, `ux=18`): while the player holds A
  (the raw pad's, masked or not) or the rule presses it, the main stick reads centred. The queue's
  own character select's BACK takes A only where the hand stood ("The character select's buttons").
- **A read as another button** (`Mask::a_as`, q2-stages): while the player holds A the game sees
  these buttons pressed (after the mask, like `press`). The casual stage pick masked A and read it
  as X, which vanilla Brawl's stage select ignores and Project+'s marks as a strike, so the rule saw
  the press in the game's own pad memory and the screen never took the stage. Since `ux=12` nothing
  uses it: the stage select's two cursors read the raw pads through the latch.
- **The raw view of a masked button** is the latch (`InputGate::SetLatch`, "The queue's character
  select", "The stage select's two cursors"): the rules see a press the game didn't.

**Test knob** `ORCA_TEST_GATE` (a test override: the compatibility key says so; set the same value
in both processes). Comma-separated rules `<port 1-4>:<what>[:<when>]`: `<what>` joins with `+` any
of `A B X Y Z L R START UP DOWN LEFT RIGHT STICK CSTICK ALL`; `<when>` is `always` (the default)
or `mem/<n>` (n 2-64; `mem` is `mem/2`), on one frame in n: those where byte (port - 1) of an XXH3
of the game's static data (0x80500000-0x80600000, both games) is a multiple of n. That changes
from frame to frame, so masks computed from the wrong frame's memory show in a sync test, a
keyframe test or a drop-in; a larger n leaves the scripts' menu presses mostly intact. Every 600
first-run boundaries the log says how many each port was masked and how often the masks changed.
Example: `ORCA_TEST_GATE=1:A+STICK+CSTICK+X+Y+Z+R:mem/8,2:START`.

Tests: `OrcaInputGate*` unit tests (what a mask does, masks that follow the state at every
boundary and nothing else, the game's read gated and the raw read not, the knob's parser).

Checked 2026-10-03 on an M4 Pro with `bf-mario-link-results.txt` (Brawl) and `pplus-1v1.txt`
(Project+), under `ORCA_TEST_GATE=1:A+STICK+CSTICK+X+Y+Z+R:mem/8,2:A+STICK+CSTICK+B+X+Y+Z:mem/8`
unless said otherwise: both ports masked on about one frame in eight, the masks changing on about
40% of frames, and the game's RAM apart from an ungated run's from Brawl's frame 99 and Project+'s
frame 267 on.
- `YG_SYNCTEST=7` across the whole loop (character select, stage select, a match, results,
  character select again): Brawl frames 1600-10600, 62,951 re-run frames; Project+ 150-5600,
  38,101; 0 RAM mismatches. With `YG_SYNCTEST_SECTIONS=1` (Project+) the SerialInterface section
  never differs; the device-state-only differences are the usual ones (icache, GPU managers).
- `YG_KEYFRAME_TEST` at the character select, stage select, match and results: Brawl 1760, 1930,
  3000, 10000; Project+ 300, 420, 450, 1500, 5000. All pass. 450's window crosses the stage
  select's exit: a keyframe test's load now restores the harness's scene as a sync test's does,
  without which it failed 30 of 30 frames with or without the gate.
- A recording made under the masks replays through a loopback session with rollbacks: Brawl to
  frame 4000 (186 rollbacks, 399 checksums matched), Project+ to 2400 (208, 239).
- `YG_PADREC` keeps the raw pads: Brawl's recording under the masks has the same pads as one
  without them on every frame before the stage select (164 with port 1 pressed), while their RAM
  checksums differ from frame 99 on.
- Two Orcas over a real room, `dropin-commands.py --env ORCA_TEST_GATE=...`, pass in both games: a
  keyframe at the character select, a match together, the friend leaving and rejoining mid-match
  from a second keyframe, the host leaving. Checksums matched 116 and 20 (Brawl), 83 and 21
  (Project+), no desync.
- A negative control: the same Brawl sync test with the masks evaluated where the frame callback
  runs (an experiment build, never committed) fails, 385 RAM mismatches in 4,851 re-run frames
  from the character select's frame 1756.
- With no source and no knob, `YG_HASHLOG` equals the base build's (`51987e7894`) on every frame:
  Brawl 10,606 frames, Project+ 8,010.
- `ORCA_TEST_GATE=1:START`: Brawl never leaves the character select (port 1's scripted Start never
  reaches the game).

## The results screen's replay

In both games the results screen's Z saves the fight's replay (the "Z REPLAY" under each player's
panel). A session can't save one: its NAND has no save file (a session starts from an empty NAND,
and both profiles' patches skip the game's "create a save file?" prompt), Brawl's SD card is absent
and Project+'s read-only. Z still opened the save: "There is no save file. Unable to load or save
data.", then the save's destination picker with both destinations crossed out ("Please insert an SD
Card" or "This SD Card is locked", and "There is no save file" for the Wii). A and Start never leave
that picker; only B does, any player's. Players who press A or Start after a game, the way they
leave a results screen, stayed on it for good, both games alike (game logic over the synced pads,
the same on both machines).

- **Found** while reproducing a two-Orca `queue-e2e.mjs --q2 --queue casual` failure (Project+, a
  simulated bad link, `--fuzz 25`: both pads pressing A, B, X, Y or Z at random, sticks too; game 1
  ended while the fuzz still pressed). Mashing A with Z on the results screen opens it, as the fuzz
  can; Z alone, A alone (both players' A leave the screen as before) or A then Z at a few chosen
  frames did not.
- **That failure itself was the harness's scene log** (`YG_SCENES`), which the e2e scripts follow.
  The run said "ada: never reached scVsResult (at scMemoryChange)", and bo's log had the results
  screen for two frames, then `scMemoryChange` for good; both Orcas' hashlogs had both games on
  `scVsResult` for the run's last 14,559 frames, every checksum matched. The log compared each
  first pass's reading with the scene a rollback's re-run had just left, so a scene a re-run
  entered (ada's results screen) was never logged; and at a boundary where an online session had
  just rolled back (OnlineMatch loads before the harness's hook runs) it logged the loaded state's
  scene as the frame just run's (bo's `scMemoryChange` at 4911, whose RAM hash is frame 4906's).
  The e2e waits for the host's `scVsResult` line before it presses anything on the results screen,
  so it waited out its 240 s with nobody pressing: both games simply sat on the results screen.
  Live 0.3.14's friends drop-in run with the fuzz failed the same way (its "never reached
  scVsResult" 240 s after the game ended). The harness now logs at first passes that didn't just
  roll back, against the scene it last logged, with the frame the scene was entered: a run without
  rollbacks logs exactly as before (the runs above, plain and under the sync test, print the same
  lines), and a re-run's scene shows at the next first pass. (`YG_HASHLOG` is unchanged: at such a
  boundary it still hashes the loaded state under the frame just run's number.)
- **Vanilla, not Orca's rules:** the same with no header at all (`pplus-1v1.txt` and
  `bf-mario-link-results.txt` with the mash added, as in a friends game or solo play), with the
  queue's header (`ORCA_TEST_QUEUE=casual:solo`), and in both games. Nothing Orca writes or masks
  acts on the results screen (the results reader, the locks, the toggled character select patch,
  the gate's masks, the character order, the set block, the queue's Start, the casual stage pick's
  `a_as`); 0.3.13's gate masked nothing there either.
- **The fix** (`ux=11`): the input gate masks Z on the results screen (`scVsResult`) for every port
  in every session's game, header or not (`Rules::SessionMasks`, merged into the gate's one source
  in `UX.cpp`). Nothing else reads Z there; the queue's Z hold (its skip) reads the raw latch, on
  its own character select. The "Z REPLAY" labels still show.
- **Checked 2026-10-04** (branch `q3-results-hang` on `rel-0.3.15`, M4 Pro, headless, `-v Null`):
  `Tools/orca/inputs/pplus-queue2-results-z.txt` and `bf-queue2-results-z.txt` (the whole casual
  flows with both players mashing A, B, X, Y and Z from game 1's results screen's first frame) stay
  on the results screen with rel-0.3.15's build and reach game 2's character select and its stage
  select with this one (Project+ results 2558-2854, character select at 2857; Brawl 4001-4297, at
  4300); so do the 1v1 inputs with the mash, without a header (Project+ results 3035-3411, Brawl
  9861-10237). `YG_SYNCTEST=7` over both new inputs, the 0.3.14 env: Project+ 200-3200 20,951
  re-run frames, Brawl 1440-4700 22,771, **0 RAM mismatches**. Hashlogs of the default inputs equal
  rel-0.3.15's on every frame (Brawl 10,816, Project+ 9,011). Unit test
  `OrcaOnlineRules.TheResultsScreenNeverTakesZ`; 373 `Orca*` tests, 356 pass, 17 skipped.

## Online rules

Phase 1 of the online rules for queue matches (YouGame `docs/ORCA_ONLINE_UX.md`; the design's
phases and rulesets): rules locked in both games, casual's Start going to the stage select's
two-turn pick ("Casual stage pick"; before Orca 0.3.14, a random legal stage),
no way back from the character and stage select to the menus, ranked's no-ready timer and its
stall rule. Friends rooms are unchanged: nothing here acts without the match block's header, and
only a queue room's host writes one (`Core/Orca/UX/OnlineRules.*`, `MatchBlock.h`).

### The match block's header

- **Where:** `MatchBlock.h`, at FreeSpace's block (`0x806F2380`, both games). Header: `YGM1`,
  version 1, mode (0 none, 1 casual, 2 ranked), ruleset (1 Brawl Supernova 2025, 2 Project+ 2024).
  Then the set state (`+0x10`, Phases 2-3), the step state (`+0x80`, Phase 2), Phase 1's ready
  timer (`+0xC0`), and the player's own rules saved when the header was written (`+0xD0`, and
  Project+'s stage switch data at `+0x100`).
- **Who writes it, and when:** only at a boundary no other game runs from a state before it: one
  where the game is its player's alone (Events.h `alone`: nobody plugged in or on the way, nothing
  anyone replays), or one where a friend on the way still waits for a keyframe not yet made
  (OnlineMatch `HeaderFreeAt`: solo, no session, no keyframe being made, downloaded or stored for
  this header and still fresh, nobody plugging in; the keyframe comes at that boundary or later,
  after the frame hook, so it carries the header). The header this machine's room calls for
  (OnlineMatch `WantedRulesMode`): a casual or ranked queue room's mode, coin and room hash while
  this game hosts it (port 1), or waits in a ranked room for the set's verdict; the queue's own
  character select's (`FLAG_SOLO`); none otherwise. None clears it, gives back the saved rules and
  zeroes the block; a block that never had a header (the game's dead code) is never touched.
  Everything else in the block is game logic at every boundary.
- **The keyframe waits for it** (Orca 0.3.15): a keyframe for an arrival is made, and a stored one
  offered, only while the game's memory carries the header the room calls for as the keyframe is
  made (`Rules::HeaderInPlace`, read by the boundary's frame hook; the room's welcome can come in
  after the hook, so the call is made again first) and was made under that header
  (`Rules::WantedGeneration`: one that wasn't, nobody having taken it, is deleted). The header
  lands at the boundary after the room's welcome, even with the opponent's hello right behind it.
  A queue room never makes one without it: after 300 boundaries (5 s) with the opponent waiting the
  host leaves the room with `orca error mismatch That match couldn't start` and the bye reason
  `mismatch`, which the joiner, still waiting for its keyframe, reports as its own `orca error
  mismatch` (both pages search again). Before 0.3.15 a keyframe went out after 120 boundaries
  whatever the header said, so a host whose header was still its own character select's could
  start the match on it (port 2 masked, Start withheld in both games). A friends room keeps that
  bound: its joiner refuses a header that locks.
- **The joiner checks it** at the first frame hook after its keyframe loads: its room's own header
  (`Rules::HeaderFitsRoom`): the mode its room's queue calls for and its own profile's ruleset, the
  coin and hash of the room's code, the queue flags its own app plays (`FLAG_QUEUE2` with
  `queue2`), never `FLAG_SOLO`; none at all for a friends room. Anything else (before 0.3.15 only
  the mode and ruleset were compared): it leaves at once, with `orca error mismatch That match
  couldn't start` and the bye reason `mismatch`.
- **Checked 2026-10-04** (branch `q3-header-flags` on `rel-0.3.15`, M4 Pro, headless): 347 `Orca*`
  unit tests pass (the room's header, the joiner's refusals, the host's wait and its one failure);
  `OrcaLive.AHostsMismatchReachesItsJoiner` with `ORCA_LIVE_TEST=1`; `queue-e2e.mjs --q2 --queue
  casual` against yougame.co with dev tickets, Project+ and Brawl: PASS (the room's header written
  before the keyframe, the joiner's check on every field, 2 verdicts the same, both back on their own
  character select); the Project+ casual flow under `YG_SYNCTEST=7` (`pplus-queue2-flow.txt`, the
  test knob's header): the room's header at 824 as before, 20,951 re-run frames, 0 RAM mismatches.
- **Lifecycle:** casual unlocks when the opponent unplugs (the host leaves the queue room for a
  room of its own: header none). Ranked stays locked while the set is open, whoever is plugged in
  (the survivor waits in the room for the verdict), and clears once the room closes.

### The locks (while the header says casual or ranked; first runs and re-runs, only bytes that differ)

| | Brawl (Supernova 2025) | Project+ (2024) |
|---|---|---|
| gmSetRule | stock, 3 stocks, 8:00, handicap off, ratio 1.0, team attack on, pause off | the same with 4 stocks |
| Items | frequency None, every item off (record menu data `+0x00`, `+0x08`, `+0x0C`) | the same |
| Stage choice | Choose (casual's turns, "Casual stage pick"; ranked's strikes) | the same |
| Legal stages | Random Stage Switch: Brawl page `0x04005007` (BF, FD, Delfino, YI, Lylat, Smashville), Melee page `0x00020000` (Pokemon Stadium); FD's bit off while a human player's pick is Ice Climbers (`0x10`): the match's init data (gmGlobalModeMelee +0x98, 0x5C per port: the character select writes each pick there as it is made; the panel's own +0x1B8 is only the hovered character), on the character select and on the way to the fight (a Random character resolved) | the 2024 Proposed list as its preset 3 (`Switch03.rss`) holds it: its first `0xA8` bytes of RSS_EXDATA (`0x8042C4E8`: page 0 the 9 legal stages, all in the random set, hazards off), written on the character select; casual also clears the strike table |
| Input gate | character select: B but a press's first frame with that player's token down (it takes the token back up, below), and A unless the hand is in the grid (below; on the queue's own select also a new press on BACK); stage select: B; fight: Start | the same, plus the D-pad on the character select (its Code Menu) and L, R and Z on the stage select (alternates, hazards, L+R+A) |

- Casual's Start: since q2-stages, the stage select and its two preferred-stage turns ("Casual
  stage pick"). Before, Random skipped it (the game's own RNG picking from the legal list).
- No way back to the menus: sqVsMelee::setNext's state 5 takes the character select's result and
  sends anything but "go on" to state 16, the menu. A toggled group (`RSBE01.patches`,
  `PPLUS32.patches`, conditions on the header's magic and mode) flips that one store to state 4,
  the character select again, the way the stage select's B comes back to it: the BACK button (and
  held B) reload the character select instead (checked under the harness: A on BACK with
  `ORCA_TEST_QUEUE=casual` brings back a fresh character select; without it, the menu). The gate
  also lets B reach the character select only as the first frame of a press with that player's
  token down, which takes the token back up ("B only takes the token back up" below), so held B
  never backs out (before `ux=12` it took B away there entirely). Project+'s own hooks in
  sqVsMelee (MusicSelect, SSSButtons, css.asm) are elsewhere.
- Pause off: Brawl's rule, and no Start in a fight for Project+'s hold-Start pause.
- The rules screen no longer opens (A on Rules is masked, below); were it to, whatever is changed
  there goes back at the next boundary.

### The character select's buttons (A only in the grid)

On a later game's character select the hands come back on the bottom panels, each on its own
player-type button, where A makes the player a CPU (the first ranked e2e played game 2 CPU against
CPU, a voided game). The same A on an empty panel's button adds a CPU, and the screen's other
buttons change the rules, teams or names. (Without a header, With Friends and a friend's drop-in
keep A off the player-type and name buttons since `ux=19`: "No CPUs online".) So while the header locks, the gate passes A to a port
only when its hand is in the character grid or on its own token (`OnlineRules.h`, "The character
select's hands"); B was already masked there.

- **The hand** (both games; found under the harness with `ORCA_UX_PROBE` watches over full-screen
  rasters): player area i's `+0x1A8` is its hand (`muSelCharHand`). `+0x80` is what it points at as
  of the frame that just ran: 0 nothing, 1 a button (`+0xAC` says which: 0x1D a player-type button,
  any panel's; 0x1C a name button; 0x1B the own panel's picture, a costume; 0x03 the BRAWL tab,
  teams; 0x05, 0x06, 0x07 the rules bar's arrows and value), 2 the grid with the token down, 3 the
  player's own token, 4 a button that leaves (0x02 BACK, 0x04 Rules), 5 a spot under the grid's
  bottom edge (`+0xAC` 8, not identified), 6 and 8 the token on its way up or down, 7 the grid with
  the token in the hand. `+0x90`/`+0x94` are its x and y (y up; the grid spans about -5 to 16, the
  buttons above 16.3 and below -17.4). Brawl moves the hand 0.83 a frame at full tilt, Project+ 1.0.
- **A acts where the stick has just moved the hand.** A pressed on the frame the hand arrives on a
  button acts on that button (probed: A with each step right along the bars' height made panel 3 a
  CPU on the very frame the hand reached its button). The gate decides from the state before the
  frame, so it passes A only to the targets 2, 3, 6, 7 and 8 and only for y strictly between -3.0
  and 15.0: one frame's travel (1.0 at most) from there never leaves the grid. Its top edge lies
  between 15.84 and 16.26 in both games and nothing above it starts lower than 16.26 (the rules
  bar's value, Project+; Brawl's from 16.46); at the bottom the grid reaches -3.96 or lower
  everywhere probed (target 5 starts between there and -4.79). The characters sit well inside:
  Brawl's rows span about -4.1 to 15.5, Project+'s about 0 to 15. An unreadable hand gets no A.
  **Brawl's bottom edge is -4.2** since Orca 0.3.19 (`CSS_A_BOTTOM_BRAWL`): its bottom row (RANDOM
  at the right end, x 22.8..28.7) reaches -4.07, so -3.0 swallowed A over the bottom quarter of
  every tile in it, which is how a player coming up from the panels first meets them. Brawl's grid
  goes on below that row to -5.35 or lower (`css-gate-runs/bottomscan.log`: target 2 down to -5.35
  at x -27.48, nothing at -5.70, the own panel's picture, 0x1B, from about -5.8; the raster's row at
  -4.98 is grid all the way across), so one frame's travel (0.83) from above -4.2 still never leaves
  the grid. Project+ keeps -3.0 (its rows end above -1.5).
- **What still works.** Picking (A on a character with the token in the hand), taking the own token
  up and putting it down, the character order's lock-in (A in the grid until `ux=12`, Start since),
  costumes with X and Y.
  Joining needs no A: a hand that moves into the grid joins its panel (probed: kind 0 to 1 as the
  hand arrives, with A masked on the button).
- **BACK on the queue's own character select** (`ux=18`, `OnlineRules.h` `CssBackTakesA`). Luis,
  2026-10-05: "anytime you could press B to go back right now, clicking A over the back button
  should also work". Held B backs out of that select (`FLAG_SOLO`: the rules leave B alone and the
  loop lock stands aside), so A on BACK does too: port 1's A reaches the game when its hand points
  at BACK (target 4, button 0x02) and A is a new press (not held on the frame that just ran, the
  queue's latch), so a held A sliding onto BACK never backs out, as the game itself wants a press
  there. While A is down the main stick reads centred (the gate's `a_centres_stick`), so the press
  acts on BACK where the hand stood. Nothing else is within one frame's travel of BACK anyway (the
  2026-10-03 rasters, `css-gate-runs/raster.log` and `pplus-map.log`: BACK spans x -32 to -23.6
  and y 18.3 to 21.5 in Brawl, x -32 to -23 and y 17.8 to 21.5 in Project+; the BRAWL tab starts at
  x -20.4, the grid ends at y 15.85, the space between is nothing). While the player is ready
  (searching) the queue keeps that press from the game for one frame: it un-readies (`orca queue
  unready`), and the next frame Orca presses A for the game, which backs out (`orca menu cancel`):
  one press, and the page never sees the game leave while it still searches ("The queue's
  character select"). Queue rooms keep A masked on BACK (casual finds someone else with a long Z,
  ranked is played), Rules (0x04) never takes A, and friends rooms and local play have no header,
  so their BACK was always the game's.
- **Proof** (2026-10-03, branch `orca-css-gate`, M4 Pro, headless, `-v Null`):
  - Brawl: `bf-ranked-bo3.txt` now presses A on game 2's character select with both hands still on
    their own player-type buttons, then port 1 moves onto panel 3's button and presses A, and port 2
    goes up into the grid and locks in. Without the gate (rel-0.3.12's build) port 1 becomes a CPU at
    frame 3963, port 2 at 3973, the order counts both presses as lock-ins and the stage select comes
    at 3977 with two CPUs. With it, `YG_SYNCTEST=7 YG_SYNCTEST_FROM=1700` with `ORCA_TEST_QUEUE=ranked
    ORCA_UX_TEST_NAMES=ada,bo ORCA_TEST_SET_RULES=1,8` to frame 9000: 51,051 re-run frames, **0 RAM
    mismatches**; no panel's kind ever changes (an `ORCA_UX_PROBE` watch on +0x1B4); port 2's A in the
    grid locks it in (frame 4093), port 1's time runs out, and the stage select comes at 6666 as before.
    Game 1's players join by moving into the grid (their A on the buttons is masked).
  - Project+: `pplus-ranked.txt` now presses A on game 2's buttons on each player's turn. Without the
    gate the winner becomes a CPU at 5106 and its press counts as its lock-in. With it, `YG_SYNCTEST=7
    YG_SYNCTEST_FROM=170` with `ORCA_PROFILE=PPLUS32 ORCA_TEST_QUEUE=ranked:1 ORCA_UX_TEST_NAMES=ada,bo`
    to frame 15000: 103,761 re-run frames, **0 RAM mismatches**, no kind changes, the same 2-0 set
    and order steps (5077, 7777, 10477) as the ranked sprint's run.
  - Unit tests `OrcaOnlineRules.TheCharacterSelectPassesAOnlyInTheGrid` and `.TheHandsAreReadPerPort`;
    1,345 tests pass.

### B only takes the token back up (`ux=12`)

Luis, 2026-10-04: "Pressing the B button should still work for me getting off my character and
choosing another" (B had been masked on the character select since `ux=8`, so a pick could only be
undone with A on the own token, which nothing on screen says). Both games tell the two B's apart
by the token, not by any state of their own worth patching:

- **Found under the harness** (Brawl rev 2 and Project+ alike, no header, `ORCA_UX_PROBE` watches
  on port 1's area +0x1F8, +0x1B8 and its hand's +0x80; `Tools/orca/inputs/bf-css-b.txt` and
  `pplus-css-b.txt` on the Versus character select): a B pressed with the token down puts it back in the hand at once (+0x1F8 0 to
  1 on the next boundary, the hand's target 0x0A, "on its way back", for 3 frames, then 7, holding);
  one frame of B is enough, and a second frame changes nothing. B held from there on backs out to
  the menus 31 frames after the press began (Brawl: pressed at the character select's frame 430,
  the scene gone at 461; Project+: 400 and 431). Shorter B's (1 and 2 frames) with the token in the
  hand afterwards never back out. A down token's flight takes 19 frames (+0x1F9 1).
- **So the gate** (`OnlineRules.h` `CssBUnpicks`, while a header locks, not on the queue's own
  character select) passes B to a port only when its token is down (+0x1F8 and +0x1F9 0, on a
  character, a human's area) and its raw B wasn't held on the frame that just ran (the queue's
  latch, `MatchBlock::QUEUE` +0x08: since `ux=12` written under every header that locks). The game
  then sees at most one frame of any press, and only while the token is down: it takes the token
  up, and the held B that backs out never reaches it. Ports 3 and 4 get no B. The rules on top mask
  it further: game 1's locked picks and a locked-in (ready) player (Queue.h), the waiting player in
  the character order (CharOrder.h, which applies the same rule to its pickers, so its test knob's
  header-less runs play alike).
- **Proof** (2026-10-04, branch `q4-css-lockin`): in `pplus-css-lockin.txt` and `bf-css-lockin.txt`
  (game 2 of a ranked queue2 set) each player holds B 60 and 140 frames from a token down: the token
  comes up once, on the press's first frame, and the character select stays; in `*-css-casual.txt`
  the same in casual's game 2. Unit tests
  `OrcaOnlineRules.BReachesTheCharacterSelectOnlyToTakeTheTokenUp`,
  `OrcaCharOrder.BReachesTheGameOnlyAsAPressWithTheTokenDown`.

### Ranked's character select timer and stalls

- **No-ready timer** (block `+0xC0`): before the set's first fight, with ports 1 and 2 plugged in
  (block `+0xC4`, written from the synced ports at every boundary), once one of them has picked
  and the other hasn't, 60 s; then that port is the no-show (`+0xC2`). OnlineMatch reads
  it at final frames (`Rules::ConfirmNoShow`): the no-show's own Orca leaves the room (bye reason
  `no-show`), which the room awards as a forfeit; the other waits there for the verdict. Later games
  keep their characters, so no timer.
- **Stalls:** a ranked set's player whose game has waited 15 s on the opponent's inputs, or whose
  opponent went silent (the existing 10 s limit), sends `finish` naming itself the winner, once
  (`Online::ReportStall`). The room then never hands the set to the one who stalled (the survivor's
  report goes against them) and awards it here when they leave. A joiner whose host went silent mid
  set now waits in the room for the verdict instead of leaving it (which was its own forfeit).

### Compatibility and tests

`ux=19` (8 brought the locks and the rules preset of `ux=6` went; 9 the character select's A
gate; 10, Orca 0.3.14, queue2's character select, Project+'s boot to the main menu with friends
dropping in from the menus, the character select's title, and the casual stage pick; 11, Orca
0.3.15, no Z on the results screen in any session, "The results screen's replay"; 12, the
character select's A to pick, B to take the token back up and Start to lock in, "B only takes the
token back up", "Character order", "The queue's character select"; 13, Orca 0.3.17, the set over;
14, Orca 0.3.18, native text; 15, Orca 0.3.19, Random as a pick and no skip in ranked, "Random and
ranked's no skip"; 16 is design F's, branch `q6-ui-f`; 17, branch `q9-friends-jump`, a friend's
drop-in from the menus waits for the main menu's build, "Boot and friends from the menus"; 18,
branch `q10-menus`, With Friends straight to the character select and B from there back to the
ONLINE page, "Online menu", and A on BACK on the queue's own character select, "The character
select's buttons"; 19, branch `q12-no-cpu`, no CPUs online and only the seats in use, "No CPUs
online"), caps
`locks`
and `queue2`. Test knob `ORCA_TEST_QUEUE=casual|ranked`
(a test override): the header as a queue room's host writes it, in a harness run or a dev game's
room, both Orcas set alike. Unit tests `OrcaOnlineRules*` (the header's lifecycle and saved rules,
each ruleset's bytes, Ice Climbers and FD, Project+'s list, the gate's masks, the ready timer,
idempotence, friends games untouched); `OrcaOnlineMenu*` and `OrcaGamePatches*` cover the toggled
group in the shipped files. 225 `Orca*` tests pass.

Checked 2026-10-03 (branch `sp-casual`, Apple Silicon, `-v Null`), `YG_SYNCTEST=7` under
`ORCA_TEST_QUEUE`: character select, Start, a match that port 2 ends by running off the stage,
results, the character select again with both ports holding B for 150 frames (nothing happens),
Start, a second match, results, the character select. Ranked also presses B on the stage select
(nothing happens) before picking.

| Run | Frames | Re-run frames | RAM mismatches | Stages |
|---|---|---|---|---|
| Brawl casual | 1500-6500 | 31,451 to frame 6000 | 0 | 0x01 Battlefield, 0x13 Lylat Cruise (Random, no stage select) |
| Brawl ranked | 1500-6500 | 34,951 | 0 | 0x02 Final Destination (picked) |
| Project+ casual | 140-5000 | 33,971 | 0 | 0x0C, 0x2D (Random, from the 2024 Proposed list: Frigate Husk, Dream Land) |
| Project+ ranked | 140-5000 | 33,971 | 0 | 0x06 (picked) |

Also under the harness: A on the character select's BACK button with the header reloads the
character select (before `ux=9`, which masks A there too); held B for 30 frames without it goes to
the menu (so the gate's B and the flip are what keep a queue match in).

Not yet (Phase 1 leftovers; since `ux=19` a CPU panel on any online select is made empty and the
record it comes back from is cleared, "No CPUs online"; since `ux=20` Project+'s Code Menu is off
and at its defaults and Giga Bowser and Wario-Man play as Bowser and Wario, "Project+'s Code Menu"):
the ready timer's no-show and the stall claim were not run live; the room doesn't yet treat a stall
claim as more than the survivor's word (YouGame rooms). Until `ux=20` this paragraph said the Code
Menu "can't open": wrong, it opened mid-fight in ranked on launch night.

## No CPUs online

Luis, 2026-10-05: "If you go into any other mode and create a CPU, and then you exit and go into
ranked, that CPU actually remains, and it creates a glitched illegal condition. Whenever we're in an
online mode, we should be clearing out any bots and continuing to disable it. ... If I go into
casual or ranked, I only see one character possible, which is me. When someone else joins, they
take the player 2 slot." Since `ux=19` (branch `q12-no-cpu`, `Core/Orca/UX/OnlineSeats.*`) no CPU
exists in any online mode, and online character selects show only the seats in use. Local Versus
(Group > Brawl alone, everything offline) keeps its CPUs as the game has them.

**Where the CPU came from** (found under the harness, q12; `ORCA_UX_PROBE` watches, both games):

- The Versus character select builds its panels from a record the game keeps between visits,
  gmSelCharData (`[0x805A00E0]+0x10`, `0x90180B40` in both games; each port at `+0xB8 + port x
  0x5C`: `+0` the character, `+1` the player's state, 0 a human, 1 a CPU, 3 nobody). Building, the
  select reads each port's state (`0x80685AFC`: 0 a human panel, 1 a CPU's, anything else empty)
  into the player area's kind (`+0x1B4`: 0 empty, 1 human, 2 CPU). It saves its panels back into
  the record only as it goes on to the stage select (`0x806878D4`), never when B leaves to the menu.
- Vanilla Brawl resets the record's players whenever Versus starts from the main menu (every online
  pick included); Project+ doesn't. So in Project+ a CPU from a local game that reached the stage
  select came back on With Anyone's Casual and Ranked selects and on With Friends', and into the
  match: game 1 fought three-way (a CPU on panel 3, Random, `mm3 = 1E 01` at the fight's first
  frame), or a CPU on panel 2 kept the joiner's hand from ever taking it, so the room's select never
  left (steering gave up at 600 frames); a no-character human on panel 3 (a second local player)
  had Orca pressing Start forever. Brawl's menu path doesn't reproduce; there A on an empty panel's
  player-type button made a CPU on With Friends' select (which had no gate).
- The stage select's end copies the record into the fight's players (sqVsMelee, `0x806DCF6C`, every
  port whose state isn't 3): writing the fight's own data does nothing, the copy overwrites it.

**What is online** (`OnlineSeats::Online`, a pure function of memory and the synced ports): the
match block's header locks (the queue's own character select, a casual or ranked queue room), or the
main menu's pick Orca keeps in the running sqVsMelee (`+0x18`, "The character select's title") is
With Friends (24-27), Casual (30) or Ranked (31), or two ports or more are plugged in (a friend
dropped in, wherever the host was, even onto Group > Brawl's select).

**What Orca writes there** (the frame hook, first runs and re-runs, only bytes that differ):

- **The record**, between two scenes (`scMemoryChange`) only: a CPU's state becomes nobody's (3), and
  so does a human's on a port nobody plugged in (Project+'s memory of a second local player). The
  menu's pick is in memory from the first such boundary after the menu (Project+ 1304, its panels
  from 1310; Brawl 2654, panels from 2688), so the select starts with only the players who are
  there and a joiner takes panel 2 as always. Never on the stage select or in a fight: by the first
  `scMemoryChange` boundary after the stage select the copy into the fight is made (watched: the
  fight's players change at that boundary), so no fight ever loses a player to this (a friend who
  leaves during the stage select still fights idle, as before). The characters stay; only states.
- **The panels**, on the character select: a CPU's (kind 2) becomes empty (0). The select saves that
  kind as it leaves, so the CPU plays no fight. The panel's picture isn't redrawn (a CPU's plate
  stays), so the seats below hide it, or, on a port someone plugged into, the join below makes it
  theirs. With the record cleared first this only acts on a select a friend dropped into while it
  had a CPU, or any path not yet found. A human's panel whose port nobody plugged in (a friend who
  left) is the game's: it empties one itself 17 frames after the controller goes, its token back
  in the hand and its highlight off the grid (both games, under the harness with the session's
  unplugged pad, `ORCA_UX_TEST_UNPLUG_AT`), and builds none for a port with no controller
  (Project+'s record, the stage select's B). Orca writing it first (tried in review) left the
  leaver's highlight on the grid, so it doesn't.
- **The join** (the input gate, `OnlineSeats::PressesJoin`): a panel a CPU had can't be joined by
  moving the hand into the grid: its token went down with the CPU and is gone with it, so the hand
  finds the grid with no token (target 2, never 7, holding). The game's own way back is A on the
  panel's player-type button (with that port's controller in, empty becomes the player's: HMN),
  as a local player rejoins after removing a CPU (found under the harness, both games, local
  Versus: the button goes empty, HMN, CPU, empty with that port's controller in, empty to CPU
  without one; after CPU to empty, by A or by Orca, the hand reaches the grid at target 2 and never
  joins, and A on the button makes it HMN, the token where the CPU left it). So on an online
  select without a header, Orca presses A for a plugged port whose own panel is empty with its
  token neither in the hand nor flying while its hand rests on that panel's own player-type
  button, where a hand starts (hand `+0xB0`: the panel whose button it points at). A friend who
  drops into Group > Brawl's select whose CPU was on their panel takes it at once, the game's
  PLAYER plate and their token where the CPU's was (on Random: they play Random until they move
  it); one who plugs into a panel whose CPU was emptied while hidden finds the same state and the
  same press. A fresh empty panel holds its token in the hand and is joined in the grid as before;
  a player's or a CPU's panel never gets the press (A there makes a CPU).
- **The buttons** (the input gate): where no header locks (With Friends, a friend's drop-in, Casual
  or Ranked before the queue's header lands), A never reaches a player-type button (0x1D: a CPU of
  an empty panel, or of a human's own) or a name button (0x1C), nor any hand at y below -16.4 (the
  buttons start between -17.4 and -18.3 in both games, the 2026-10-03 rasters; a hand moves at most
  1.0 a frame, so no frame's stick takes it there with A down). Brawl's panel picture (a costume,
  0x1B) ends at -17.4, Project+'s at -16.6: A loses its bottom edge only (X and Y still turn the
  costume). An unreadable hand (the select being built or torn down) gets no A either, as under a
  header. Under a header the online rules already pass A only in the grid ("The character
  select's buttons"). Joining a fresh panel needs no A: a hand moved into the grid joins it (the
  player's own A on their empty panel, which also joined, no longer reaches the game there; the
  join above presses it where it is needed). The gate reads the plugged ports from the friends
  bytes the frame hook keeps (`MatchBlock::FRIENDS_SEEN`).
- **The seats**, on any character select: an empty panel (kind 0) whose port nobody plugged in is
  hidden while the select is online, except the opponent's seat of a queue select (panel 2: always
  in a queue room, and on the queue's own select while it searches). Each panel is player area i's
  39 game models (MuObjects at area `+0xB0..+0x14C`, one slot unused; each one's nw4r `ScnMdl` at
  `+0xC`, its `ScnObj` flags at `+0xCC`, `0xA0000000` on all 156 in both games): Orca sets `0x60`
  (draw neither opaque nor translucent) on the hidden ones and clears it on the others, only on a
  model whose flags are exactly the game's or the game's with those two bits and whose first word is
  a vtable in the executable. The select's code never stores to those flags (it shows and hides
  parts with VIS animations), and they are rebuilt with the select, so nothing outlives it; a select
  that stops being online (the friend left Group > Brawl) gets every panel back at the next
  boundary. Hiding is drawing only: a hidden panel's logic and buttons stay (the gates above keep
  them dead), the select's play is unchanged (the queue's and steering's log lines alike with and
  without, both games), and only drawing-side memory (the GX FIFO, some draw state) differs from an
  unhidden run, the same on both machines.

  So: **the queue's own select** shows the player alone while picking, then the opponent's seat
  (design F's SEARCHING plate and the kit's ring) once it searches; **a queue room** shows both
  players (panels 3 and 4 hidden); **With Friends** shows the players plugged in, a friend's panel
  appearing as they plug in (empty until their hand reaches the grid). Panel 2 while searching was
  Luis's to pick (his "only me" taken literally would hide it until the opponent joins); it stays
  the SEARCHING seat Luis approved in design F, the place the opponent takes. Hiding it too is one
  condition in `OnlineSeats::SeatHidden` (the kit's ring would then have to go, `Overlay.cpp`).

  Not the game's own 2-player layout: the select builds 1, 2 or 4 panels from a mode byte (task
  `+0x648`, `0x806834D8`), and forcing 2 works under the harness (P1 far left, P2 far right) but
  changes game logic (the joiner's steering, Random's draw), is fixed when the select starts, can't
  serve a 3-4 player friends room, and puts the overlay's per-panel places wrong.

**Determinism:** every write is a pure, idempotent function of emulated memory (the header, the
pick, the record, the areas, the queue's ready flag) and the session's synced ports, at the frame
hook (the record and panels after the online rules, the seats after the native text); the gate's
masks and its join press read memory alone (the hands, the areas, the friends bytes). A joiner
loads the host's keyframe, which carries all of it. `ux=19`.

**Test inputs.** The default two-player inputs (`bf-mario-link-results.txt`: A at the character
select's 53 and 83; `pplus-1v1.txt`: 60 and 70) join with A on the player's own player-type
button, with no visit qualifier, so on every character select. In a session two ports are plugged
in, so the select is online, and since `ux=19` that A is masked: each player joins when the
stick's climb takes the hand into the grid (from 113 in Brawl, 90 in Project+). Two-Orca runs of
these inputs (`xplat.py`, `selfmatch.mjs`, `direct-link.py`, `dropin-rooms.py`,
`occluded-test.py`) therefore play differently from a pre-19 build's from the first character
select on (the joins later, and that A on later selects no longer reaching the game): their frames
and hashes don't compare with a pre-19 run's. Harness-only runs (`perf.mjs`, the release's
hashlogs: port 1 plugged in alone, a local select) are unchanged, so the hashlogs still compare
with the live release's, and the inputs stay as they are for that reason. A harness walk that
plays ports 2-4 from its script on an online select needs those ports plugged in
(`ORCA_UX_TEST_NAMES`, and `ORCA_TEST_PADS=4` for ports 3 and 4): a port the frame hook doesn't
have is nobody's there (its record entry emptied between scenes, so the next select has no panel
for it), and `ORCA_UX_TEST_PADS_FOLLOW_PLUGS=1` gives such a port no controller, as a session
does.

**Checked 2026-10-05** (branch `q12-no-cpu` on `rel-0.3.23`, M4, headless, muted, under the solo
lock; scripts and summaries in `~/Documents/brawl-dolphin/q12-nocpu-runs`, screenshots in
`ui-shots/q12-no-cpu`):

- Unit tests `OrcaOnlineSeats.*` (16: what is online, the record's and the panels' states in every
  mode, the entries past the fourth untouched, only between scenes, idempotence, which seats hide in the queue's own
  select, a room and With Friends, a select that stops being online, only known models, the A
  mask by hand and mode); 468 `Orca*` tests pass.
- Luis's bug, Project+, no test knob (`Tools/orca/inputs/pplus-cpu-online.txt`): local Versus makes a
  CPU on panel 3 (kind 2 at 496), the stage select saves it (record `1E 01`), B rebuilds it (local
  keeps its CPUs); then PLAY ONLINE > Ranked: the pick (31) at 1304, where the record's CPU is cleared
  (`1E 03`), the select's panels at 1310 are port 1's alone (kinds 1 0 0 0, panels 2-4 hidden), and
  A on panel 4's player-type button does nothing (before: a CPU at 1434). Casual (1254/1260) and
  With Friends (1154/1160) the same.
- Brawl, With Friends (`bf-cpu-friends.txt`) and Ranked: the game's own reset at the menu's exit,
  panels 2-4 hidden from the select's first readable frame (2538, 2688), A on panel 4's button
  nothing (before: a CPU at 2634), port 1 joins by moving into the grid. Local control (Group >
  Brawl again): A on panel 4 makes a CPU and the stage select takes it (record `24 01`); no model
  flag written on a local select.
- Queue rooms, the test knob (`ORCA_TEST_QUEUE=ranked:solo`, `pplus-queue2-ranked.txt`, the pick at
  874) with CPUs written into the record at the main menu (ports 2 and 3, 3 alone, 2 alone): the
  record cleared at 571, the select at 577 with the player alone, the opponent's seat at 775
  (searching), the room's header at 874, the joiner's pick in at 952 (78 frames, as without CPUs),
  both ready at 1195, the stage select at 1200, the fight at 4030 with two players (ports 3 and 4
  `3E 03`). Before, a CPU on 3 fought game 1 and one on 2 kept the room on its select. Brawl
  (`bf-queue2-ranked.txt`, the pick at 2106): hidden at 1837, the seat at 2037, the pick in at 2200
  (94 frames), both ready 2427, the stage select 2432, the same frames as rel-0.3.23's run.
- `YG_SYNCTEST=7`, **0 RAM mismatches**: Project+ Luis's walk, 300-1700 (local CPU, its stage
  select and rebuild, the record cleared on the way into Ranked, the hidden seats, A on panel 4):
  9,751 re-run frames; Project+ queue room with the CPU record, 400-4300 (the clear, solo, searching,
  room, stage select, fight): 27,251; Brawl With Friends' walk, 1500-2950: 10,101; Brawl queue room,
  1500-3200: 11,851.
- Two Orcas over yougame.co, dev tickets: `queue-e2e.mjs --pplus --queue ranked --q2 --probe
  Tools/orca/inputs/pplus-cpu-record-probe.txt` (both players' games carry CPUs on ports 2 and 3
  from the main menu): **PASS**, both records cleared before their own selects (frames 623 and
  1258), a 2-0 set with every fight two players on both machines, 3 verdicts alike, **231 checksums
  matched** on each side, both back on their own select. `online-menu-dropin.py` (With Friends with
  a friend dropping in), Project+ and Brawl: PASS, 12 checksums matched each.
- Not run: Mac <-> PC (`xplat.py`), the release's hashlogs (local Versus inputs, which this leaves
  untouched).

**After review, checked 2026-10-05** (same branch; harness ports with
`ORCA_UX_TEST_PADS_FOLLOW_PLUGS=1`, so a port nobody plugged in has no controller, as in a session;
summaries and scripts in `~/Documents/brawl-dolphin/q12-fix-runs`, screenshots `fix-*` in
`ui-shots/q12-no-cpu`):

- A friend into Group > Brawl's live select with CPUs on panels 2 and 3 (`bf-cpu-dropin-live.txt`,
  `pplus-cpu-dropin-live.txt`). Without the join (ux=19 as first written) both CPUs went at the
  plug-in but the friend's panel kept its CPU plate and their hand never took it (target 2, no
  token): stuck. With it: Brawl, CPUs cleared at 1866, the friend's panel theirs at 1871 (BO's
  plate, their token on Random), the fight at 2202 with ports 1 and 2 only (3 and 4 `3E 03`);
  Project+ 634, 639, the fight at 889 the same.
- A friend who leaves With Friends' select (`bf-friend-leaves.txt`, `pplus-friend-leaves.txt`, three
  players, `ORCA_UX_TEST_UNPLUG_AT`): the game empties the leaver's panel 17 frames after the
  controller goes (Brawl 1834 to 1851, Project+ 554 to 571; the same with no online rule at all, a
  local control), Orca hides it then, and the fight has ports 1 and 2 only. Orca emptying it at
  once (the review's suggestion, tried) left the leaver's highlight on the grid and was dropped.
  A select built from the record for a port with no controller has no panel there (Project+, the
  stage select's B), so the record race the review raised never shows a human either.
- `YG_SYNCTEST=7`, **0 RAM mismatches**, the final build: the friend into a live select with CPUs,
  Brawl 1700-2450 (5,201 re-run frames), Project+ 450-1150 (4,851); the friend who leaves, Brawl
  1650-2300 (4,501), Project+ 300-950 (4,501); Luis's walk, Project+ 300-1700 (9,751; the record
  cleared at 1304, the panels hidden at 1310 as before). With the leaver's write still in (the same
  join and masks) also the Project+ queue room with CPUs in the record (27,251; the pick in at
  952, both ready 1195, the fight at 4030 with two players) and Brawl's queue room (11,851; pick
  2200, ready 2427), at the frames of the first round.
- Two Orcas over yougame.co (dev tickets), the final build: `Tools/orca/versus-cpu-dropin.py`
  (new: the host makes CPUs of panels 2 and 3 on Group > Brawl's select, the friend drops in
  there; both machines clear the CPUs at the same frame and give the friend panel 2 at the same
  frame with no input from them), Brawl **PASS** (cleared 2239, theirs 2244, 14 checksums matched
  each), Project+ **PASS** (1156, 1161, 13); `queue-e2e.mjs --pplus --queue ranked --q2 --probe
  pplus-cpu-record-probe.txt` **PASS** (2-0 set, 3 verdicts alike, 229 checksums each);
  `queue-e2e.mjs --queue ranked --q2` (Brawl) **PASS** (224 each); `online-menu-dropin.py`, both
  games, **PASS** (12 each).
- Unit tests: 18 `OrcaOnlineSeats` (the join's press by hand, panel, kind and token; the leaver's
  panel left to the game); 470 `Orca*` pass.
- Not run: Mac <-> PC (`xplat.py`, needs the PC), the release's hashlogs (local selects get no
  writes, presses or masks from this).

## Project+'s Code Menu

`ux=20` (branch `fix-codemenu`, 2026-10-06). On launch night players reported "the debug menu" in
ranked and casual: flight, infinite shield, Giga Bowser and Wario-Man, a frozen match. It is
Project+'s own Code Menu (Netplay), in the pinned v3.2 SD card that Orca boots, not anything of
Orca's. ORCA.md "Project+'s Code Menu and banned fighters" has the rules; this is how they were
found and checked.

**The menu** (`Source/Netplay/Net-CodeMenu.asm` in the SD card's source; `NETPLAY.txt` includes it):

- "[CM: Control Codes] Load Code Menu" (hook `0x8002D4F4`) reads `pf/menu3/dnet.cmnu` as is to
  `0x804E0000` (9,504 bytes to `0x804E2520`). It is there by the session's frame 2.
- "[CM: Code Menu] Control Code Menu" (C2 hook `0x80029574`), every frame: ORs every GC pad's game
  buttons (`0x805BAD04 + 0x40 x i`); `(buttons & 0x64) == 0x64` (L, R, D-pad Down) requests the
  menu (state `0x804E0034` = 2). Wiimote and Classic paths exist but Orca forces Wiimotes off.
- Code Menu Activation (`0x804E0C38`): 0 Default opens anywhere; 1 PM 3.6 refuses only while the
  in-match flag `0x804E0758` is set; 2 OFF turns every request back into state 0. The P+ main menu's
  own Code Menu button sets state 1, and A or Start then opens it **without** the activation check.
- Open (state 4) it saves the pause word `0x805B8A08` at `0x804E006C`, writes 1 there every frame
  (both games freeze: a pause in a mode where Start is masked), and reads every pad. Start, or B on
  the root page, closes it: state 3, the pause word back; the same frame's hook then applies each
  port's Character Select line to its fighter and sets the state from `0x804E0030` (0).
- Every frame with the menu shut, the values apply on their own: per-port Infinite Shield, D-pad
  percent, Disable DPad, Input Buffer, ALC; Flight (`[CM: DBZMode]`, `0x804E23BC`); the debug
  bytes `0x80583FF7..FFF` copied from the Debug lines (skipped only in a match with activation 1 and
  the menu shut); gameplay constants copied to `0x80B87AA8`.. by "Constant Overrides" (hook
  `0x80023D60`, unconditional). So a value set offline, or by a joiner in an earlier set, carried
  into the next locked match without anyone opening the menu.
- Each line: `+0` size, `+2` type (0 selection, 1 integer, 2 float), `+4` colour flags (bit 3:
  changed, drawn blue), `+8` value, `+0x10` default. The 73 forced words are every value line but
  the four Character Select lines (`0x804E0F48`, `0x804E1424`, `0x804E1688`, `0x804E18EC`; "Add New
  Character Buffer" syncs them to the fighters), Autosave Replays, Save Previous Replay (a state-3
  file write), Tag-Based Costumes, Crowd Cheers, Tag Hex x4 and HUD colours x4. Checked word by word
  against `dnet.cmnu` (SHA-256 `eea59cba...d479`); `0x804E1F88` is Walljump Horizontal Multiplier
  (its float `0x3F666666` reads as text `?fff`).
- Not reset: the colour flags and the pages' changed counts, so after a locked match a default value
  the player had changed offline is drawn blue until X or Y resets it. Cosmetic.
- `Source/Project+/Debug/modifiedDebug.asm` has its own L+R+Down debug toggle (`gr2 ^ 1` on the word
  at `0x80583FFC`); the menu's per-frame copy of Debug Mode undoes it within the frame, and the gate's
  `drop_with_lr` stops it reaching the game at all.

**The lock** (`OnlineRules.cpp` `ForceCodeMenu`, every boundary under a Project+ header, first runs
and re-runs): `Rules::CodeMenuPresent` checks the layout (page pointer inside the file, root page
`0x804E07D8` at `+4` as the menu itself checks, Activation's line size `0x58` and self-pointer,
Flight's line size `0x30` and its `+0x18` word `0x804E0F00`); then state 4 gives the pause word back
from `0x804E006C`, the state becomes 0 whatever it was, and the 73 words take their defaults with
activation 2. `WriteHeader` saves the activation at match block `+0xDA` (`SAVED_FLAGS` bit 2) when
it writes a fresh header and the menu is present, and gives it back on clear. A joiner's block is
the host's, so a joiner gets the host's saved activation back, as with the saved rules.

**Banned fighters.** Project+'s "Hold Shield for Special Fighter" (`Source/ProjectM/CSS.asm`, hooks
`0x806847BC`, `0x8068482C`, `0x80685BE4`) runs in the function that saves the panels into the
record as the character select ends (`0x806846E0`): a Bowser (slot `0x0C`), Wario (`0x15`) or Ice
Climbers (`0x10`) pick whose player holds the shield button becomes slot `0x38`, `0x36` or `0x37`,
character `0x2C` Giga Bowser, `0x2D` Wario-Man or `0x11` Sopo. Legacy TE's AltCostume hook at
`0x8068480C` takes R (`0x20`) out of the shield mask (R picks costumes), so on a GC pad it is L.
Under the harness R held through Start did nothing; L held through Start made both specials.
The init data gets the special character right there, and again from the record on the way from the
stage select to the fight. `ForceLegalFighters` (character select, stage select, between) rewrites
both: init data `+0x98 + 0x5C x port`, the record's characters (`+0xB8 + 0x5C x port`) and slots
(`+0x0A + 4 x port`). With only the init data rewritten (first version), the transition copied
`0x2C`/`0x2D` back in, the next boundary fixed it, the fight was still Bowser and Wario, but
`scMemoryChange` took 56 frames instead of 1 (it had started loading the banned fighters' files).
With the record rewritten too, it is 1 frame and nothing banned is ever loaded. Sopo is not banned.

**Checked** (Mac, 2026-10-06, Release build of `fix-codemenu`, headless, under the solo run lock;
inputs and probes in `~/ClaudeWorktrees/orca-codemenu-runs/`):

| Run | What | Result |
|---|---|---|
| Control, offline, no header | `pplus-1v1`'s way to a fight; port 1 holds L+R, presses Down at fight frame 330, Start at 480 | state 4 and pause word 1 at frame 918, both back at 1068: the script opens the menu, and offline keeps it |
| `ORCA_TEST_QUEUE=ranked:1`, `pplus-ranked` + the chord | L+R+Down by port 1 (twice) and port 2 mid-fight, and both on the results screen | activation 2 from frame 3; state and pause word never change; the fight runs to the results (port 1 won 4-0); screenshot at the chord: Pikachu shielding, timer running, no menu |
| Same, cheats by probe | Flight, P1 Infinite Shield, P2 ALC, hitstun 1.0, activation 0 on the main menu; Flight, Infinite Shield, state 4 and pause 1 at fight frame 200 | every value back to its default at the next boundary; the open menu closed, pause word 0, the fight went on |
| Same, Giga pick | port 1 on Bowser, port 2 on Wario, L held through Start | the select wrote `0x2C`/`0x2D` (init and record) and slots `0x38`/`0x36`; next boundary `0x0C`/`0x17` and `0x0C`/`0x15`; `scMemoryChange` 1 frame; results characters `[12, 23]`; screenshot: Bowser vs Wario |
| `YG_SYNCTEST=7` from 170 to 5600, final build | the chord input; the Giga input (L through Start) | each 37,961 re-run frames, 0 RAM mismatches |

Unit tests: `OrcaOnlineRules.TheCodeMenu*`, `AnOpenCodeMenuClosesAndTheGameRunsAgain`,
`ClearingGivesBackTheCodeMenusActivation`, `ProjectPlusBannedFightersPlayAsTheirBaseFighters`,
`ProjectPlusNeverSeesTheCodeMenusChord`, `OrcaInputGate.DownDropsOnlyWhileLAndRAreBothHeld`.

Open: friends rooms keep the menu (Luis's call). `TRIGGER_HELD` (a quarter press) is a guess at
where the game counts an analog trigger as held; the activation lock does not depend on it.

**Release review (2026-10-06, candidate `codemenu-q14`: `fix-codemenu` + `q14-play-again-clean`).**

- `ForceCodeMenu` read each word through `Writer::U32` (4 `Valid` + 4 `Read8` per word, 584 MMU
  calls a boundary, first runs and re-runs). It now reads each word once (`Read32`) and calls
  `U32` only for one that differs: same writes, same `changed` count. A `static_assert` keeps every
  word inside the range `CodeMenuPresent` checked, so the plain read is always mapped.
- ORCA.md said "the chord never arrives": the gate drops Down only while both triggers read held
  (button, or analog at 0x40); the game's own threshold isn't known. Activation OFF is the lock.
- Kept as is: values a player set offline aren't given back after a queue (they come from the
  file again at boot); a joiner gets the host's saved activation back, as with the saved rules;
  Sopo stays legal (the spec; Luis to confirm for ranked).
- Re-run on the candidate build (Mac, Release, headless, under the run locks):
  - the cheats probe (`preset.probe`, ranked header): the same frames as before, every value back
    at the next boundary, the forced-open menu closed and the pause word 0 at fight frame 201;
  - the Giga pick (`giga3-ranked.txt`): `0x2C`/`0x2D` at frame 548, `0x0C`/`0x17` at 549, the
    fight Bowser vs Wario (`[12, 23]`);
  - `YG_SYNCTEST=7` from 170 to 5,600, the chord input and the Giga input: 37,961 re-run frames
    each, 0 RAM mismatches;
  - hashlogs against 0.3.27 (the released Orca.app): `pplus-1v1` frames 0-9,000 and
    `bf-mario-link-results` 0-10,800 identical.
- Two Orcas over prod rooms (`queue-e2e.mjs --pplus <dol> --q2 --queue ranked --pick bowser --giga
  --chord --probe watch-cm.probe`, dev tickets): both readied Bowser (`orca queue ready ranked 12
  0`) and held L through the shared select's Start. At frame 2,361 both games wrote `0x2C` for both
  ports; at 2,362 both read `0x0C`. 17 L+R+Down chords by both mid-fight and one each on both
  results screens: the menu's state and the pause word never changed, activation 2 from the header
  on. Set 2-0, `orca result` game and set lines on both sides, characters `[12, 12]`, 238 checksums
  matched each, no desync, no `orca error`.
- Two-Orca performance comparison against 0.3.27 (the released Orca.app), one run each, the same
  conditions: a Project+ ranked set over prod rooms (`queue-e2e.mjs --pplus <dol> --q2 --queue
  ranked --shots 0 --shots-who ada,bo`: both render with Metal), `ORCA_TEST_NET_DELAY_MS=40`,
  `ORCA_TEST_NET_JITTER_MS=8`, Mac (arm64), load 6-13. Per side at the end (host, joiner):

  | | candidate | 0.3.27 |
  |---|---|---|
  | frames | 14,401 / 16,087 | 14,401 / 16,031 |
  | rollbacks, re-run frames, deepest | 26, 47, 2 / 30, 60, 3 | 27, 108, 5 / 30, 109, 5 |
  | stalls | 3 / 6 | 8 / 18 |
  | hitches here | 14 / 17 | 27 / 32 |
  | snapshot ms each | 0.98 / 0.80 | 1.42 / 1.23 |
  | input delay (the link chose it) | 4 / 4 | 2 / 3 |
  | checksums matched | 204 / 233 | 203 / 231 |

  No regression: the candidate is at or below 0.3.27 on every count. The input delay differs (each
  run's link measured its own round trips), so the rollback counts aren't like for like; snapshot
  time and hitches are. One run each on a shared machine: noisy.

## Ranked sets

A ranked queue match is a best-of-3 set followed in the game's own memory, so both Orcas, every
rollback re-run and every drop-in keyframe see the same score (`Core/Orca/UX/RankedSet.*`,
`Core/Orca/UX/SetBlock.*`). The room hears each game from there once its frames are final, and
rates the set once.

### The match block

`FreeSpace::kMatchBlock` (`0x806F2380`, both games) and its free tail up to `kMatchBlockLimit`
(`0x806F2618`). Only Orca's frame hook writes it, as a pure function of memory and the frame
number, first runs and re-runs alike, and only the bytes that differ. Every rules track writes the
same header; the set as this track follows it sits at `+0x210`, past the 512-byte block, clear of
the other tracks' sets, steps, saved rules (`+0x10`-`+0x1FF`) and the character order (`+0x200`).
Layout (big-endian):

| Offset | |
|---|---|
| `+0x00` | Header: `u32` magic `YGM1`, `u8` version 1, `u8` mode (0 none, 1 casual, 2 ranked), `u8` ruleset (1 Brawl / Supernova 2025, 2 Project+ 2024). Nothing else below `+0x210` is touched here. |
| `+0x210` | Set: `u8` games recorded, `u8[2]` wins (ports 1, 2), `u8` done (1 won, 2 void), `u8` winner port, `u8` last game's winner, `u8` tiebreak next, `u8` ties in a row. |
| `+0x218` | The fight being followed: `u8` on, `u8` flags (1 between scenes, 2 sudden death, 4 time ran out), `u8[2]` ledge grabs, `u32` its first frame (the game's id), `u8[2]` stocks and `u16[2]` percent when time ran out, `u8[2]` on the ledge last frame. |
| `+0x230` | 8 game records of 12 bytes: `u32` first frame, `u8` winner port (`0xFF` a tie), `u8` how (bits 0-3: 1 results screen, 2 stocks, 3 percent, 4 ledge limit, 5 tie; `0x10` a tiebreak game, `0x20` time ran out), `u8` stage, `u8[2]` characters, `u8` stocks left (port 1 high nibble, port 2 low; `0xF` unknown), `u8[2]` ledge grabs. |

The header follows the room this game is in (`RankedSet::SetRoomMode`). A host that takes a queue
room (`host <code>`) asks for its queue's mode; any room of its own again (`ReopenRoom`: after a
pairing, a set, a leave) asks for none. It is written at the next frame boundary where the game
plays alone (no friend plugged in or on the way), so never in a frame anyone else replays: the
joiner gets it with the host's keyframe. A new or different header starts a fresh set; none
clears the block. `ORCA_TEST_MATCH_BLOCK=ranked|casual` (tests only, no real room; since the ranked-sprint
integration, `ORCA_TEST_QUEUE=ranked|casual`, see "Ranked sprint integration") asks for a
header at boot.

### Following a set

At every boundary of a ranked block, `RankedSet::Step` reads the scene (`scMelee` fight,
`scMemoryChange` between, `scVsResult` results):
- a fight scene starts a fight (its first frame is the game's id). Brawl's sudden death stays in
  `scMelee`: the fighters are gone for a few frames and come back with 1 stock each, which is how
  it is told (also a way back into a fight from between scenes);
- during the fight (Brawl), `RankedSet::ReadLiveFight` reads the fighters: sora_melee's
  `g_ftEntryManager` (module 27's section 6 + `0x2E88`, through the OS module list at
  `0x800030C8`), each entry (`0x244` bytes: `+0x0A` active instance, `+0x28` ftOwner, `+0x30`
  instances, `+0x58` the port); the owner's data `+0x34` stocks (`u32`) and `+0x24` damage (`f32`);
  the fighter `+0x60` -> `+0xD8` -> `+0x70` the status module, `+0x34` the status (`0x73`-`0x75`
  catching or holding the ledge). The result info's decision (`+0x1378`) is 9 during the fight and
  turns 1 the frame time runs out (the game freezes on "TIME!"), 2 at the deciding KO. It counts
  each player's ledge grabs (entries into those statuses) and keeps stocks and percent as they were
  when time ran out; nothing after time-up or in a sudden death counts;
- the results screen judges the game (`RankedSet::Judge`) and records it; any other scene ends a
  fight without a game.

The verdict:
- not exactly two humans on ports 1 and 2, or a no contest: no game.
- Brawl (Supernova 2025): a KO is the results screen's winner. Out of time: the ledge-grab limit
  first (over 35 grabs loses, Meta Knight over 20; both over, ignored; 11 and 6 in a tiebreak
  game), then more stocks, then lower percent, as they were when time ran out (the game's own
  sudden death is ignored). A sudden death the clock didn't cause (a simultaneous last-stock loss),
  or a time-out still level, is a tie: the next game is a tiebreak game with 1 stock and 3:00
  (`RankedSet::WriteSetRules`, on the menus; 3 stocks and 8:00 otherwise). Five ties in a row void
  the set.
- Project+ (2024): the results screen's winner. Its codeset already decides a time-out by stocks
  then percent and gives an exact tie overtime ("Tiebreaker v2.0", "RoA Overtime v2.1").

Two wins end the set; 8 games without that void it. What the other tracks read: the score
(`+0x211`), the last winner (`+0x215`, the loser picks next), the stages each port won on (the
records' winner and stage, for Dave's Stupid Rule), the tiebreak flag (`+0x216`: the next game is
played on the last record's stage with its characters; locking those is the stage select's and
the input gate's part, not done here) and the set's end (`+0x213`). The menus (character and stage select, the
results screen) show the score at the top centre: "Game 2 · 1–0", "Tiebreak game · 1–1",
"Set over · 2–1" (`RankedSet::ScoreLine`, the local player's wins first).

### Reports

The results tracker (`Results.h`) reads the block at every boundary and, once a frame is final,
reports each game the block recorded since: `{"t":"game-report","id":"g<first frame>",...}`, the
winner named by the room match's participants as `start` gave them (port s is slot s, whoever left
the room since). The detail adds `"k"` how, `"lg"` ledge grabs and `"tb"` a tiebreak game (`"f"` is
the fight's first frame). A game whose fight began before the opponent's plug frame isn't
reported. A tie is a draw. When the block ended the set with a game, both Orcas send `finish` with
that winner (or void) once the room's verdict on that game is in. The room agrees on the outcome
only (YouGame `rooms/src/game-lobby.ts`): a detail that differs is stored, not compared. A game
already final when a session ends (a leave, an unplug) is confirmed before the readers start over.
Leaving, quitting the app or losing the connection mid-set is the room's forfeit (15 s).

Checked: `OrcaSetBlock*`, `OrcaRankedSet*` unit tests (layout, a Bo3, void games, the Brawl
time-out order with the ledge limit, sudden death in the fight scene, ties and tiebreak games,
Project+, the score line, the tracker's reports); RAM probes of Brawl fights (the fighter chain,
stocks and damage, statuses `0xBD` dead and `0xBE` rebirth as BrawlHeaders names them, the
decision turning 1 at time-up, sudden death staying in `scMelee`); a harness set with
`ORCA_TEST_MATCH_BLOCK=ranked ORCA_TEST_SET_RULES=1,1` (a level time-up is a tie, the tiebreak game
and one more game win the set 2-0, a fourth game isn't recorded), also under `YG_SYNCTEST=7`, and
Project+'s 2-0 under `YG_SYNCTEST=7`. Not checked live: a real ledge grab count.

## Set end on screen

Luis, 2026-10-04: "after winning or losing a set, we should get a victory or loss overlay that
shows the results and elo change ... when someone leaves, I'm just kind of stuck in the match
confused about what's going to happen ... maybe add a countdown that's like seconds until opponent
forfeits". `Core/Orca/UX/SetEnd.*` (the model, pure, and its layout), drawn with the overlay's
widgets (`Core/Orca/UX/Widgets.*`: panel, title, line, big number, countdown, rating change; every
colour and shape is there, and since 0.3.18 each is drawn with the kit: "Seamless UI"). Local
display: nothing is written to the game, and the compatibility key is unchanged.

- **VICTORY / DEFEAT** (ranked). The match block's set over on the results screen (first runs,
  `RankedSet::Frame`) opens the panel at once: the title, the set's score with the player's games
  first ("2–1"), "vs <name>", and the rating. The room's verdict on the set opens it too when the
  block didn't (and settles the score). The rating counts from the old number (the page's `queue
  rating` going in, or the room's `before`) to the new one over 1 s, eased, then pops (the number
  and a "+16" green / "–15" red / "±0" grey badge swell 28% and settle in 0.45 s). The new number is
  the room's (its `result` carries the site's ratings) or the page's second `queue rating`; the
  page's old number sent again is ignored. "Rating updating…" under the old number until it comes,
  and no rating at all after 10 s. The panel stays 7 s, or 3 s after the count lands, 15 s at most.
- **Set over** (Orca 0.3.17, `ux=13`; YouGame `docs/ORCA_ONLINE_UX.md` "3B set over", PRs #521 and
  #522). Luis, 2026-10-04: the players stay connected on the victory screen and can talk there;
  only leaving it disconnects them, and each can hold Z to leave at once. After a ranked set's
  verdict the room sends `{"t":"set-over","matchId","until","ms":300000}` and stays open, relay and
  chat, for five minutes at most (then 4004). Orca keeps the session running through the results
  screen (nobody goes solo at the verdict) and each game leaves the room by itself
  (`OnlineMatch.cpp` `SetOverLeave`, the app's `leave` path), once:
  - its results screen is over: the newest final reading's scene (`ResultsTracker::FinalScene`) is
    no longer the results screen. Both players pressing on in the game's own flow moves both games
    on at the same frame, so both leave together; a verdict that finds the game elsewhere (a
    forfeit's, mid-fight) leaves at once;
  - the player holds Z 1.5 s on the results screen (this player's own pad as the boundary read it
    for the game, before the input gate, which keeps Z from the game there since `ux=11`). The
    overlay shows "Hold Z to leave" near the bottom, with a bar that fills as Z is held;
  - with the opponent's game gone (their leave, the host's session idle, or a joiner solo), Start
    or A ("Press Start to continue · Hold Z to leave").
  Nothing is a forfeit in set over: the opponent leaving the room shows "<name> left" (the room's
  leave, or their game going quiet; gone once this player leaves too), the game that stays keeps the room
  (`AfterFriendsGone`, `HostGone`) and leaves the same way, no stall is claimed (`MatchLive` is
  false once the set is decided) and no countdown shows. A host still waiting for a forfeit's
  verdict (`Linger`) stops waiting at `set-over`. The joiner's game keeps the room's header while
  it stays (`WantedRulesMode`). Inputs still stream through set over: the results screen is a
  session frame like any other (the room relays for up to five minutes).
- **No search by itself after a ranked set** (Luis, 2026-10-04). A ranked queue room where a fight
  was played (a set played there, finished or forfeited; `NoteRankedFight`, the newest reading's
  scene) puts back the player's own character select **not ready** (`Queue::AfterRestore(false)`):
  no `orca queue ready`, so the page doesn't search; Start does. The Start (or A) that left the
  results screen is often still held as the player's own game comes back: it readies nobody until
  it is let go (`Queue::FLAG_SWALLOW_START`, written at that game's first alone boundary, as the
  ready flag's clearing is). Casual searches again by itself as before, and so does a ranked room
  left before any fight (a no-show, a mismatch).
- **Casual:** each game's verdict shows a smaller YOU WON / YOU LOST / DRAW with the sitting's
  score in this room ("2–1  vs bo"), 6 s.
- **The opponent disconnected** (ranked, mid-set): a centred notice, "<name> disconnected · They
  forfeit in" with a ring that drains round the seconds left ("0:09"). The truth end to end:
  - The room's `leave` (their socket closed: a quit, a crash, a killed app): the room awards the
    forfeit its GRACE, 15 s, after it (`rooms/src/game-lobby.ts`, `tick`), unless they come back in
    time. The countdown runs from the leave reaching this Orca. A queue room's host that was waiting
    on their inputs unplugs them at once (`LeftSeatsInPlay`; before, it waited out the 10 s
    silence limit frozen) and plays on meanwhile; HostEvents hears the leave at the next boundary.
  - No leave, just silence (a frozen game, a dead network that keeps the socket): the session's
    stall gives up after 10 s (`PEER_SILENCE_LIMIT`) and claims the set (`ReportStall`), which the
    room lets stand at once, since it too has heard nothing for 9 s (`STALL_SILENCE_MS`) by then.
    Shown once the stall is 1.5 s long (so "0:09"); their inputs coming again take it away.
  - A leave during a stall counts from the leave. Past zero the notice stays at 0:00 until the
    verdict, 20 s at most; the end of `VERDICT_WAIT` without one takes it away.
  - The verdict, a forfeit won: "<name> forfeited · You win the set" for 2.5 s, then VICTORY "by
    forfeit" with the rating.
  The plain OSD lines this replaces in queue rooms ("Your friend's game stopped responding", "<name>
  left: you play on", "You left <name>'s game") are no longer shown there.
- **Casual leave:** "<name> left · Searching again", 4 s.
- **This player left** a ranked set (the app's `leave`, or no character picked in time): "You left ·
  This set counts as a loss" ("Time's up · You didn't pick in time..."), 4 s, over its own character
  select.
- `ORCA_UX_SETEND_DEMO=victory|defeat|countdown|casual|left|stay` (tests only, with test knobs
  allowed) plays one of them through every 16 s from the first overlay frame, for design checks
  (`stay`: set over's VICTORY, the hint, Z held, the opponent leaving).

Checked 2026-10-04 (Orca 0.3.17, M4 Pro, headless, Project+): 396 `Orca*` unit tests pass (18
skipped): `OrcaSetEnd*` (the count's phases, easing, pop and retargeting, whole seconds left, the
leave and stall countdowns and which wins, back in time, only mid ranked set, the block's panel
once, the room's and the page's ratings, the waits, void sets, a new match, leaving, casual, set
over: no forfeit after the verdict, "<name> left", the leave hint and Z's bar, and every screen
drawn at every stage without an ImGui assert) and
`OrcaQueue.AStartHeldThroughTheWayBackIsSwallowedUntilLetGo`. `YG_SYNCTEST=7 YG_SYNCTEST_FROM=170`
over `pplus-ranked.txt` (`ORCA_TEST_QUEUE=ranked:1 ORCA_UX_TEST_NAMES=ada,bo YG_RESULTS=1`, to frame
9400): 61,761 re-run frames, 0 RAM mismatches, through game 1 and its results screen into game 2's
stage select (the input predates the stage select's two cursors, so game 2's fight isn't reached).
Two Orcas over yougame.co (`Tools/orca/queue-e2e.mjs --q2 --queue ranked --pplus <dol>`, dev
tickets, with `--shots 0 --shots-who ada,bo --shots-size 1280x720 --shots-out <dir>` for the
screenshots):
- the set (`--scenario match`): 2-0, VICTORY on one side and DEFEAT on the other, the page's ratings
  counted in (1532 to 1548, 1488 to 1473); `set-over` on both with the verdict; both stayed on the
  results screen (3 s and more, nobody leaving by itself), then both pressed on and both games left
  the room at the same frame; both back on their own character select not ready; 231 checksums
  matched on each side;
- `--scenario zleave`: the joiner held Z on the results screen and left (1.5 s); the host showed
  "bo left", no forfeit, stayed, and left on Start; both back not ready, with no `orca queue ready`
  in the 5 s after (the first run of it caught the Start that left the results screen readying the
  host's own character select: `FLAG_SWALLOW_START`); 230 checksums matched on each side;
- `--scenario kill`: the joiner's Orca killed mid-fight in game 2; the host's countdown from the
  room's leave (heard 108 ms after the kill), the forfeit verdict 14,963 ms after it (the countdown
  said 15 s), `set-over`, the host out of the room at once, VICTORY by forfeit with the page's
  rating over its own character select, not ready.

## Ranked stage flow

A ranked set is a real best of three on the game's own stage select: game 1's strikes, later
games' strikes or bans and the loser's pick, turn by turn, with timers, and the set's score kept
in the game's memory. Project+ first (`Core/Orca/UX/RankedPPlus.*`); Brawl reuses the same step
machine and block on its own track.

- **The step machine** (`Core/Orca/UX/RankedSteps.*`, shared by both games): pure functions of a
  ruleset and the set so far. Stages are the ruleset's legal list, starters first, as bit masks.
  - Game 1: the coin's port strikes 1 starter, the other strikes 2, then the coin's port picks one
    of the last two (the same as striking one: "1-2-1, and the first striker picks from the last
    two").
  - Later games: the last game's winner strikes 2 (Project+) or may ban 1 (Brawl, optional), then
    the loser picks; stages the loser already won on are out from the start (full DSR).
  - Brawl only: the Meta Knight clause (exactly one Meta Knight: the other player picks any legal
    stage, game 1 too) and Ice Climbers never on Final Destination. Project+'s list has neither.
  - Timers: game 1's steps 30, 30 and 10 s, a later strike, ban or pick 30 s, each with 3 s of
    grace (the clock shows 0:00 meanwhile). Defaults: the strikes still owed fall on the first
    allowed stages in list order; Brawl's optional ban is none; a pick is the first allowed stage.
- **The match block** (`Core/Orca/UX/MatchBlock.*`): 0x90 bytes at the start of FreeSpace.h's block, in the ranges the casual track reserves (header +0x00, set +0x10, steps +0x80).
  - Header: magic `YGM1`, version, mode (none, casual, ranked), ruleset (1 Brawl Supernova 2025,
    2 Project+ 2024), and the coin: game 1's first striker, one bit of an FNV-1a hash of the
    room's code. The server makes the code, neither player picks it, and both games read the coin
    from memory.
  - Set: games counted, the score, the last winner, the stages each port won on, done and the
    winner, the fight in progress and its stage, a record per game.
  - Steps: the flow (idle or running on the stage select), the step, the active port, strikes made
    in the step, the stages struck this game, the step's deadline (the frame hook's frame number),
    and the default pick when a pick's timer ran out.
  - Lifecycle: the host of a matchmade room writes a new header (a new set) at the first alone
    boundary after it takes the room (`HostInPlay` -> `RequestHeader`), before any keyframe a
    joiner loads. At an alone boundary where the game neither still hosts that room nor plays in
    a matchmade room, the header is cleared, so a friend who drops in later meets no block.
    Everything else is game logic: a pure, idempotent function of memory, the frame number and
    whether ports 1 and 2 are both plugged in (the session's synced plan), first runs and re-runs
    alike (a rollback runs the hook again on the loaded state: a second pass changes nothing).
- **Project+** (`RankedPPlus.*`), while the block says ranked and Project+:
  - Outside the stage select, `RSS_EXDATA` (0x8042C4E8; its first 0xA8 bytes, past which every preset Project+ ships is the same) holds the 2024 Proposed preset
    (`pf/stage/switch/Switch03.rss`): page 0 is the 9 legal stages, hazards off. Its page-0 slots
    give each position's stage kind through the preset's slot table (+0x104): positions 0-8 are
    GHZ, Bowser's Castle, Frigate Husk, Dream Land, Temple of Time, Smashville, Battlefield, PS2,
    Luigi's Mansion (kinds 0x23, 06, 0C, 2D, 09, 21, 01, 2E, 04, `pf/stage/stageslot/<kind>.asl`).
  - On the stage select (since `ux=12`, with the two cursors: "The stage select's two cursors")
    the striker's X on their own cursor strikes, and the flow draws the struck art through
    Project+'s Stage Striking table (before, Project+'s own Stage Striking, the hook at 0x806B586C,
    struck with X and the hook counted the active player's new bits in `STAGE_STRIKE_TABLE`; the
    game's X is now masked). At each boundary the hook writes the table (0x8042C822, page 0's
    positions 0-30 at +2/+4) the block calls for: stages not in play (game 1's counterpicks, the loser's
    won stages), the strikes so far, and every stage of pages 1-4. A change it makes is redrawn by
    Project+'s code, which redraws the page's struck art whenever `PAGE_INDEX` (0x8042C821) isn't
    `CURRENT_PAGE` (0x80496000): the hook sets it to 0xFF. Project+'s own check then refuses a
    struck stage (it clears the selection).
  - Turns (before `ux=12`; since, "The stage select's two cursors"): the stage select's controller
    field (task +0x278) was the active port, and the gate masked every other port entirely. The
    active port kept its stick and the step's button, X to strike or A to pick; B (clears strikes,
    back to the characters), L and R (alternate stages, and L+R+A back to the characters, which
    reads every port), Z (hazards), Start and Y were masked, and A and X on Random (it would strike
    every stage outside the random set, or pick for them). Now every port is masked and the flow
    acts through the game's cursor, so none of those can reach the game.
  - A pick whose timer ran out: every other stage shown struck, the selection (+0x248) set to the
    default, and the gate presses A for the player (`Mask::press`, new) with everything else masked.
  - Results: a fight entered from the flow counts at its results screen (Results.h `Judge`): the
    winner's score, the stage won on, the record. A draw or a void counts nothing (the same game
    again). Two wins decide the set, and the stage select is free again.
  - The overlay's top-centre lines (UI only, published from first runs): "Ranked · Game 2 · ada
    1–0 bo"; on the stage select the top line also says whose turn it is, how many stages they
    still strike or ban, and the time ("Ranked · Game 1 · ada 0–0 bo · bo strikes 2 · 0:24"), and
    under it, for the player whose turn it is, the keys ("Your turn: X on a stage strikes it").
- **Compatibility:** `UX::kCompatVersion` 8.

**Test knob** `ORCA_TEST_RANKED=<0|1>` (since the ranked-sprint integration
`ORCA_TEST_QUEUE=ranked:<0|1>`; a test override): a ranked header from the first frame for
the running game, game 1's first striker port 1 (0) or 2 (1), written on every pass. Set the same
value in both processes. With `ORCA_UX_TEST_NAMES=ada,bo` the harness plugs in ports 1 and 2.

Tests: `OrcaRanked*` and `OrcaMatchBlock*` unit tests (both rulesets' steps, DSR, the clauses, the
timer defaults, the coin's balance; the block's round trip; Project+'s flow over a fake of its
memory: the preset, game 1's turns, a strike on a struck stage, the fight and its result, game 2's
winner strikes, the timers' defaults and the default pick, the set's end, an opponent who leaves,
the overlay's lines, and every boundary applied twice).

Checked 2026-10-03 on an M4 Pro, Project+ (`Tools/orca/inputs/pplus-ranked.txt`, `ORCA_PROFILE=PPLUS32
ORCA_TEST_RANKED=1 ORCA_UX_TEST_NAMES=ada,bo`):
- `YG_SYNCTEST=7 YG_SYNCTEST_FROM=170` over the whole set, frames 170-8656: 57,561 re-run frames
  checked to frame 8400, **0 RAM mismatches**. It covers the character select, game 1's stage
  select (port 2's X strike on Battlefield through the cursor, port 1's two strikes and port 2's
  pick by their timers, the pick made with the gate's pressed A on Luigi's Mansion), the fight, the
  results counted into the set, game 2's stage select (the winner's two X strikes, Battlefield and
  Bowser's Castle, then the loser's pick by its timer, PS2), the second fight and the 2-0 set.
- Without the sync test, the same set with a probe that set the selection (+0x248) before each X
  and A: every strike and both picks through the players' presses, the table and the struck art
  as the block said (page 0 bits 0x0F for the counterpicks at game 1's start, then 0x2F, 0x6F,
  0x7F), Project+'s own check refusing a struck stage under the cursor (Bowser's Castle), and the
  stage the fight loaded the one picked.
- The probe tool's one-shot writes don't repeat on a sync test's re-runs: a rules write from a
  probe at the character select shows as RAM mismatches at that frame, so the sync test runs
  without one.

## Ranked stages (Brawl)

A ranked set's stage flow on vanilla Brawl's own stage select (RSBE01 rev 2, Supernova 2025 as
Luis confirmed it; `Core/Orca/UX/BrawlStages.*`, the steps in `RankedSteps.*`). Project+ has its
own flow. Casual and friends games are untouched.

- **Game 1:** characters first, as the beta picks them. On the stage select the five starters
  (Battlefield, Pokemon Stadium on the Melee page, Lylat Cruise, Smashville, Yoshi's Island) are
  struck 1-2-1 with X (since the ranked-sprint integration: 1-2, then the first striker picks of
  the last two with A, and the coin is the header's), the first striker a fair coin from the game's own RNG seed (`0x805A00BC`,
  through murmur3's finalizer: the same on both machines, nobody's port). The last starter is the
  stage, and the flow selects it.
- **Later games:** the previous game's winner may ban one of the seven legal stages (X) or skip
  (Y, or let the timer run out); then the loser picks with A, with full DSR (never a stage the
  loser has won on this set; if that leaves nothing, DSR is dropped).
- **MK clause:** exactly one Meta Knight (gmCharacterKind `0x18`): the other player picks any
  legal stage, over strikes, the ban and DSR. **IC/FD:** with Ice Climbers (`0x10`) on either
  port, Final Destination is never allowed.
- **Turns:** since `ux=12` each player acts on their own cursor ("The stage select's two
  cursors"): X strikes or bans on the turn's player's own turn, Y skips the ban, the picker's A
  picks, anyone's A proposes. The game's own cursor and pads are the flow's (the gate masks every
  port; before, the controller field was the turn's port and the gate left the others nothing).
- **Grey tiles:** every stage the current step doesn't allow (struck, banned, DSR, IC/FD, not
  legal) is a plain grey tile (its icon's frame 0.0), re-applied every frame on whichever page
  shows. Allowed sets only shrink within one stage select, so nothing is restored.
- **Timers:** strikes 30, 30 and 10 s, the ban 30 s, the pick 30 s, each with 3 s of grace at
  0:00. When one runs out: the first remaining starters are struck in list order, the ban is
  skipped, the pick takes the first allowed stage.
- **The flow's own pick** (the last starter, a timer's default): it moves the cursor there (through
  the page button when the stage is on the other page) and the gate presses A for the turn's port
  on every other frame until the game takes it.
- **A stage taken against the steps** (a press in the very frame the cursor crosses onto another
  icon): the game's choice is replaced before the screen copies it out, by the stage the player had
  rested on in a pick, or by what the steps' defaults leave if it happens during strikes. While a
  stage is taken nobody's pad does anything.
- **Overlay:** "Game 2 · You 1–0 Ada" on the character select; on the stage select the same top
  line also says whose turn it is, how many stages are left to strike or ban, and the timer
  ("Game 1 · You 0–0 Ada · Ada strikes 2 · 0:24", "Game 2 · You 1–0 Ada · You may ban 1 · 0:30",
  "Game 2 · You 1–0 Ada · Ada picks · 0:30"), and under it, for the player whose turn it is, the
  keys ("X on a stage strikes it", "X on a stage bans it · Y skips", "A on a stage picks it").

**Where it runs.** The match block's header (the casual track's MatchBlock.h layout, read by
offset: `'YGM1'`, version 1, mode 2 ranked, ruleset 1 Brawl) turns it on; `ORCA_TEST_RANKED=brawl`
(a test override) does too, for harness runs without the header. Without either, nothing is
written or masked (and a state left over is cleared).

**Determinism.** The flow's state is the block's step range, `0x806F2400`-`0x806F2440`: the phase,
the step, its strikes and timer, the coin, the characters, each port's last buttons, the stage
select's clock and what the cursor rests on, and its own record of the set. The set the steps
follow is the results track's (SetBlock.h, `+0x210`: wins, the last winner, each game's winner and
stage) when the block holds one, else that record. The frame hook writes only the bytes that differ
and is idempotent at its boundary (a boundary run again after a load changes nothing): presses are
edges against the buttons the block kept, and the timer and the cursor's rest tick only when the
stage select's own frame counter (task `+0x250`) moved; a counter back at the start is a new stage
select, so a set that ended mid-select never leaves steps behind. The gate source reads the block
back. `UX::kCompatVersion` 8.

**Where (Brawl rev 2, probes):** the stage select task is the scene's `+0x3AC`: `+0x8C` the icons
(each `+0x14` -> `+0x10` -> `+0x18` its picture's frame), `+0x200` the cursor (`+0x3C`/`+0x40` x and
y, -30..30 and -19.5..19.5, up is +y; a write moves it, and the hover follows the next frame),
`+0x224` the state (0 choosing, 2 a stage taken; the screen leaves about 6 frames later), `+0x228`
the page (0 Brawl, 1 Melee), `+0x244` the hovered item (the slot + 2; `0x35` the page button,
`0x36` Random, `0x37` Back, 0 nothing), `+0x248` the hovered slot (kept after the cursor leaves),
`+0x250` frames since it opened, `+0x258` the taken stage's srStageKind (a write before the screen
leaves changes the match's stage), `+0x278` the cursor's port (`0xF0` any). Each page's stages:
MuStageTblAccess's table `0x806B9298` (8 bytes a page: the list, the count; page 0 is 31 stages,
page 1 is 10). Battlefield, Final Destination and Delfino are page 0's slots 0-2, Yoshi's Island 11,
Lylat 13, Smashville 20, Pokemon Stadium page 1's slot 9. The pads the game read: gfPadSystem
`[0x805A0040]+0x40`, 0x40 a port, gfPadButtons (X `0x400`, Y `0x800`). Characters for the coming
game: gmGlobalModeMelee `+0x98`, 0x5C a port, already filled on the stage select. On vanilla's stage
select X, Y, Z, L and R do nothing, and Start takes a stage.

**Checked** (2026-10-03, Apple Silicon, `-v Null`):
- `YG_SYNCTEST=7` over a scripted set (`Tools/orca/inputs/bf-ranked-bo3.txt`,
  `ORCA_TEST_RANKED=brawl`, 1-stock rules, port 2 walking off): game 1's strikes by X with the turns, the flow's pick across pages onto Pokemon Stadium
  (the match's stage `0x2E`), the match, results; game 2's ban timing out, the loser's A on
  Battlefield, the match, results (2-0): frames 1,700-6,700, 34,951 re-run frames, 0 RAM
  mismatches.
- Probes: a cursor write at the page button and A turns the page; the taken-stage override (A on
  Battlefield, `0x2E` written at `+0x258`: the match on Pokemon Stadium); the state written back to 0
  is no cancel (the screen stays faded on READY TO FIGHT).
- `OrcaRankedSteps*` and `OrcaBrawlStages*` unit tests (the steps, the coin, DSR, the MK clause,
  IC/FD, timers and defaults; the flow through a fake of the game's memory with every boundary run
  twice; the header; the results track's set; the backstop).

**Not yet:** a screenshot check of the grey tiles and the overlay lines (Luis tests the look); a
two-Orca drop-in mid-select; Brawl's exact Supernova order (Phase 4).

## Casual stage pick

A casual queue match's stage (Luis, 2026-10-04: "A stage is picked at random between the two
player's preferences. Players have 15 seconds to select stage."), both games, on the game's own
stage select with the two cursors ("The stage select's two cursors"; `RankedSteps.h` Prefer and
`PlanCasual`; Brawl's glue in `BrawlStages.*`, Project+'s in `RankedPPlus.*`). Since q2-stages
casual's stage choice is Choose: Start on the character select goes to the stage select, as in
ranked.

- **Both at once** (since `ux=12`; before, two 7 s turns, the header's coin's port first): one step
  for both players (`Step::port` `BOTH`), 15 s (`Ruleset::casual_seconds`) with half a second of
  grace (`casual_grace_frames` 30). Each player's A on a legal stage under their own cursor names it
  (a marker in their colour on the tile), B takes it back, A on another stage changes it.
- **The same stage starts the match at once.** Two different ones: the coin (`CasualChoice`) takes
  one (`CasualResult`), the moment the second player names theirs.
- **Time's up:** a player who named nothing gets a random legal stage (`CasualDefault`), then the
  same rule.
- **The flow picks it** for one player (Brawl: the game's cursor moved there, through the page
  button when needed, and A pressed every other frame; Project+: the selection set and A pressed 4
  frames in 8, every other stage shown struck), as for a ranked pick whose timer ran out.
- **The coin and the random stages** come from `CasualSeed(room, coin, game)`: the room code's hash
  (header `+0x08`), the header's coin and the game number (fights begun from the stage select, + 1),
  through murmur3's finalizer, so neither player can tell which way it falls and both machines, every
  re-run and a drop-in's keyframe compute the same.
- **Legal stages** as the locks have them: Brawl's 7 (BF, FD, Delfino, YI, Lylat, Smashville, PS1;
  never FD while either player is Ice Climbers, no Meta Knight clause), grey tiles for the rest;
  Project+'s 2024 Proposed 9.
- **Only with both players in** (ports 1 and 2 in the session's synced plan): alone, the stage
  select is the player's own.
- **Overlay** (top centre, UI only): "Stage pick · 0:15", under it for each player "A on the stage
  you want · the same pick plays there, else a coin takes one of the two", then "You want
  Smashville", and what the other wants ("bo wants Battlefield · A on it to agree"); then
  "Smashville (bo's pick)", "(your pick)" or "(both picks)".
- **State.** Brawl: `BrawlStages`' region (`+0x80..+0xBF`, the block's step state; its own game):
  `+0xA3`/`+0xA4` the two preferred stages, `+0xA8` the seed, `set.game` counting casual fights.
  Project+: `MatchBlock::State` (`+0x10..+0x8F`): `+0x10` counts casual fights, `+0x40`/`+0x41` the
  preferred stages, the steps' fields at `+0x80` as ranked uses them, `+0x8F` why the flow picks
  (`AUTO_CASUAL`). The cursors: "The stage select's two cursors".

Tests: `OrcaRankedSteps.CasualIsOnePreferStepForBothOnTheWholeList`,
`.ProgressCasualBothNameThenTheSameOrTheCoin`, `OrcaBrawlCasual.*`, `OrcaRankedPPlus.Casual*`.

## The stage select's two cursors

Luis, 2026-10-04: "it'd also be really helpful if each player had their own cursor in stage
select ... People should also be able to just preselect stage with A and skip the formal banning
process if they'd like." Queue matches only (a casual or ranked header and both players in); friends
rooms and solo play keep the game's own stage select (`Core/Orca/UX/StageCursors.*`, the flows in
`BrawlStages.*` and `RankedPPlus.*`, the steps' `Propose` in `RankedSteps.*`, the drawing in
`Overlay.cpp`).

**How it plays** (both games):
- Each player has a cursor of their own, moved by their own stick, both at once, drawn by Orca's
  overlay in the player's colour (port 1 red, port 2 blue) with a tab "P1 ada"; the player whose
  turn the steps say it is has a white ring round theirs.
- **A on a stage proposes it** (a frame round the tile in the proposer's colour with their name);
  B takes the proposal back. When both players propose the same stage, the match starts on it at
  once, whatever steps are left (game 1's strikes, a later game's ban and counterpick). Agreement
  takes any legal stage in play this game that isn't struck or banned (game 1: the starters; later
  games: all seven or nine, DSR ignored; never FD with an Ice Climbers in Brawl). A strike or ban on
  a proposed stage takes the proposal off.
- **The steps are the same as before**, on the turn's player's own cursor: X strikes (game 1's
  1-2-1, Project+'s later strikes) or bans (Brawl's later ban), Y skips Brawl's optional ban, and in
  the pick step the picker's A picks (a stage DSR keeps from them still proposes). Turn timers and
  their defaults as before. Casual: "Casual stage pick".
- **Example** (Luis's): game 2, the loser's A on Battlefield (a proposal), the winner's Y (no ban),
  then the winner's A on Battlefield: Battlefield.
- **Brawl's pages:** A on the page button (Melee Stages / Brawl Stages) turns the page for both
  (Pokemon Stadium is on the Melee page); a proposal on the other page is in the top line, not drawn.
  Project+'s nine are all on page 0: no page turns.
- **The game's own cursor** is nobody's: the gate masks every port entirely on the stage select
  while the cursors run (Project+'s own strikes, L, R, Z, Random and B never reach it). The flow
  writes its position at every boundary: where the cursor that moved last is (its hover shows that
  stage's preview and name), on the page button while a page turn waits, on the stage once it is
  picked; the task's controller field (`+0x278`) is that port, and the gate's synthesized A
  (`Mask::press`) is the flow's only press: the page turn's, and the pick's (Brawl every other frame
  once the game's hover reads the stage; Project+ the selection set and A 4 frames in 8).
- **Top line** (UI only): whose turn, what to do, how many and the timer as before
  ("Game 2 · You 1–0 bo · bo may ban 1 · 0:30"); under it the keys for the turn's player, then
  what the other proposes ("bo wants Battlefield · A on it to agree"), or else "A proposes a
  stage"; "Agreed: Battlefield" once both did.

**Determinism.** The cursors are synced state in the match block (free bytes on rel-0.3.15, the
integrator's assignment to this track), written only as a pure function of memory and the synced
pads, integer math only:

| Where | What |
|---|---|
| `+0xF4` | u8 tag `0xC2` while the cursors run (anything else: off; read only under the block's magic) |
| `+0xF5` | u8 the layout's game (1 Brawl, 2 Project+) |
| `+0xF6` | u8 the follower: the port whose cursor the game's cursor follows (the last one that moved) |
| `+0xF7` | u8 a page turn: bit 7 on, bit 0 the port that asked, bit 1 the page it was on |
| `+0xF8` | u8 frames the page turn has waited (gives up after 90) |
| `+0xF9..+0xFF` | free |
| `+0x1B4`, `+0x1BA` | port 1's, port 2's cursor: s16 x, s16 y (1/16 of the stage select's own cursor units, up is +y), u8 the buttons of the frame that just ran, u8 the buttons the frame hook has seen |

- **The latch** (`InputGate::SetLatch`, after Queue's): at every boundary, right after the masks,
  with ports 1-2's raw pads of the frame about to run (the session's or the harness's), each stick
  moves its own cursor: past a dead zone of 24 (of 128), the deflection clamped to 100, times the
  layout's speed over 100 (truncated), clamped to the cursor's range; and the buttons (A, B, X, Y,
  Start, Z) go into the cursor's "now". The latch runs after the boundary's snapshot or keyframe was
  saved or loaded, on every pass (first runs, re-runs, a joiner's first frame after its keyframe),
  from the state that boundary starts from, so the next boundary's frame hook reads the same cursors
  on every machine. Only while the tag is on and the scene is the stage select.
- **The frame hook** takes each port's presses as edges of "now" against "seen" and marks them
  seen, so a boundary run again (a rollback's load) sees none and writes nothing new; every action
  it takes is a pure function of the block and the game's memory. The flows turn the cursors on
  when a stage select's steps begin (both at the layout's start points) and off when they stop (off
  the stage select, the opponent gone, the header gone); a new header clears both ranges
  (`OnlineRules` FreshSet).
- **Which tile** a cursor is on: the layout's tile rectangles (`StageCursors::Layout`, integers in
  the cursor's units x 16), probed in each game (below). The overlay maps the cursor's units onto
  the picture (the presenter's target rectangle) with the layout's probed linear map, floats, UI only.
- **Overlay:** the frame hook publishes the cursors, the proposals and whose turn it is on first
  runs (`StageCursors::Publish`); `Overlay.cpp` draws them before its text. Nothing it draws reads
  emulated memory.

**Found under the harness** (2026-10-04, `ORCA_UX_PROBE` writes and watches, no header, two
players; the Project+ runs with the 2024 Proposed preset written on the character select):
- **The game's cursor** (the task's `+0x200` -> `+0x3C`/`+0x40`, floats, both games): a write moves
  it and the hover follows; the game clamps a write to its range (Brawl x -30..30, y -19.5..19.5;
  Project+ x -31..31, y -23.5..20.5), so it can't be parked off the screen. Brawl's moves 1.6 a
  frame at full tilt (the two cursors use the same speed, 26/16). It is a pulsing red crosshair
  whatever the controller field says, and starts on Random (Brawl 3.5, -17.5). Writing its picture's
  frame or a scale to hide it was not found in the time there was: the overlay draws each player's
  crosshair opaque and a little larger over the follower's position instead.
- **The tiles:** a sweep of the cursor over each page, 0.5 apart, two frames a point, the hovered
  item (`+0x244`) read at each: Brawl's page 0 is 31 stages in six staggered rows (item = slot + 2),
  page 1 (Melee) ten; the page button (`0x35`) is x 15.5..30, y -19.5..-17.5 on both, Random
  (`0x36`) left of it, Back (`0x37`) top left. Project+'s page 0 is two rows under the preview:
  positions 0-3 (GHZ, BC, FH, DL) at y -11..-3.5, 4-8 (ToT, SV, BF, PS2, LM) at y -17.5..-11.5,
  item = position + 2 (`+0x248` the position). The layouts' rectangles (`StageCursors.cpp`) are the
  swept extents a quarter unit out.
- **The picture:** with the cursor written at known points under `ORCA_TEST_PRESENT=1280x720`, its
  ring's middle in the screenshots: the cursor's (0, 0) at (639.8, 358.4) in a 1250 x 720 picture
  (bars at x 15 and 1265), 15.18 pixels a unit across and 14.42 down, the same in both games. The
  overlay maps through the presenter's target rectangle, so other window sizes and aspect modes
  follow. The picture a frame shows is the game's drawing of the frame before (the follower's
  crosshair peeked out a frame's travel ahead of the overlay's while moving), so the overlay draws
  the view published at the boundary before the last.

**Checked** (2026-10-04, branch `q4-sss-cursors` on `rel-0.3.15` with the clean-leave and
results-hang merges, M4 Pro, headless):
- Unit tests: 386 `Orca*`, 368 pass, 18 skipped (the live, network and disc ones), 0 fail; new:
  `OrcaStageCursors.*` (the latch: dead zone, speed, bounds, each stick its own cursor, the
  follower, off the stage select; edges seen once; every legal tile reads its own stage, none
  overlap), `OrcaBrawlStages.ThePickersAPicks`, `.AgreementSkipsTheStepsButNeverTakesAStruckStage`,
  `.LaterGamesAgreementIgnoresDsrNotTheBan`, `.TheWinnerSkipsTheBanWithYAndBothAgree`,
  `.APageTurnGoesThroughThePageButton`, `.TheGamesCursorFollowsWhoeverMovedLast`,
  `.TheGameSeesNoPadWhileTheCursorsRunAndTheFlowPressesForIt`, `OrcaBrawlCasual.BothPickAtOnce*`,
  `.TwoStagesTheCoinTakesOneAndTimeRunsOut`, `OrcaRankedPPlus.BothProposingOneStageSkipsTheSteps`,
  `.CasualBothNameAStageAtOnce`, `.CasualTwoStagesTheCoinAndTimeRunsOut`,
  `OrcaRankedSteps.AgreementIsLegalInPlayAndNotStruck`, `.ProgressCasualBothNameThenTheSameOrTheCoin`
  (every boundary run twice, the second changing nothing).
- Brawl, `Tools/orca/inputs/bf-sss-cursors.txt` with `ORCA_TEST_QUEUE=ranked ORCA_TEST_SET_RULES=1,8
  ORCA_UX_TEST_NAMES=ada,bo YG_RESULTS=1`, `YG_SYNCTEST=7 YG_SYNCTEST_FROM=1700` to frame 9000:
  game 1's stage select at 1824, both cursors moving at once, port 2's proposal (Smashville), port 1's
  strike (Battlefield), port 2's strike (Yoshi's Island) while port 1 turns the page, port 1's and
  port 2's proposals of Pokemon Stadium on the Melee page: agreed, the fight on it; game 2 (port 1
  won): the loser proposes Battlefield, the winner skips its ban with Y and proposes Battlefield:
  agreed. 51,051 re-run frames, **0 RAM mismatches**, the same steps as the plain run.
- Project+, `pplus-sss-cursors.txt` with `ORCA_PROFILE=PPLUS32 ORCA_TEST_QUEUE=casual`,
  `YG_SYNCTEST_FROM=170` to 2700: game 1 both name Smashville (the second after moving over): played
  there at once; game 2 Battlefield against Dream Land: the coin took Battlefield. 17,661 re-run
  frames, **0 RAM mismatches**. `pplus-sss-cursors-ranked.txt` (`ORCA_TEST_QUEUE=ranked`) to 1200:
  port 1 strikes Temple of Time, port 2 Luigi's Mansion, both propose Battlefield: agreed, the
  fight on it; 7,161 re-run frames, **0 RAM mismatches**.
- Screenshots (`-v Metal ORCA_TEST_PRESENT=1280x720`, deleted after): both crosshairs in their
  colours with their tabs, the gold ring on the turn's player, the proposal frames and tabs ("ada",
  "bo", "ada + bo" in gold once agreed), the game's own crosshair covered by the follower's while it
  moves, Brawl's grey tiles and Project+'s struck art as before, and the top lines ("Game 1 · You 0–0
  bo · bo strikes 2 · 0:30" / "bo wants Smashville · A on it to agree", "Agreed: Pokemon Stadium",
  "Stage pick · 0:14" / "You want Smashville").

**Not yet:** the two cursors in a real two-Orca room and Mac <-> PC (the release checklist's); a
drop-in keyframe mid-select (the cursors are block bytes like the rest, the latch's write comes
after the keyframe); the game's own crosshair is covered, not hidden; a long harness run's
screenshots went black from about frame 8000 on whatever was drawn (the game 2 stage select wasn't
shot; its steps are in the sync test).

### Whose turn it is (Luis, 2026-10-05)

Luis, 2026-10-05, after a ranked set where he was Meta Knight, so the Meta Knight clause made the
stage select one pick by Hbox (no strikes, no ban) and nothing on screen said so: "for both brawl/P+
I really want it to be very obvious whose turn it is to act both for character select and stage
strike. We should have like a blinking cursor on who is supposed to be acting." Overlay only (no
emulated memory, `ux` unchanged):

- **The acting player's cursor blinks** (`Kit::Attention`): 2.5 beats a second (under the 3 flashes
  a second limit), a white ring lit for 60% of each beat, a glow in their colour, the cursor swelling
  15%, and a burst of three ripples for 0.8 s when the turn comes to them.
- **A tag by it** (`Kit::TurnTag`, texts from the pure `StageCursors::TurnText`, names as the
  cursors' tabs write them): "YOUR TURN" / "X STRIKES A STAGE · 2 LEFT" (the other screen: "HBOX'S
  TURN" / "STRIKING 2"), "YOUR TURN" / "X BANS 1 · Y SKIPS" ("HBOX MAY BAN 1"), "YOUR PICK" / "A
  PICKS THE STAGE" ("HBOX PICKS"), under the clause "YOUR PICK" / "ANY STAGE · META KNIGHT CLAUSE"
  ("HBOX PICKS ANY STAGE" / "META KNIGHT CLAUSE"), with the turn's timer. Gold with a blinking rim
  on this player's own turn, the other player's colour on theirs. A waiting local player's cursor
  goes grey with a small "WAITING FOR HBOX" tab. A casual pick (both at once): each player who hasn't
  named a stage blinks in their own colour until they have.
- **The Meta Knight clause** is said for the stage select's first 3 s, centred: "META KNIGHT
  CLAUSE" / "HBOX PICKS ANY STAGE · NO STRIKES" ("YOU PICK ANY STAGE · NO STRIKES" on the picker's
  screen).
- **Character order:** the picker's portrait frame blinks on the same beat, with "YOUR PICK" /
  "HBOX PICKS" where LOCKED IN goes. Not yet: a ring on the game's hand (it needs the hand's
  position measured), PRESS START on game 1's ready step.
- **No "Game N"** (Luis: "After match 1, It says game 2 on top which is weird ... we have the 3 dot
  UI on top"): the overlay's score line is empty for an ordinary game (tiebreak, set over and void
  stay), Brawl's stage select line reads "You 1–0 Hbox", Project+'s "Ranked · ada 1–0 bo", the
  result toast "You won · 1–0" / "You lost · 0–1" / "Draw · played again" / "That game didn't
  count". Brawl's native rules bar still starts game 1 with "Game 1: " (NativeText writes the
  game's memory: dropping it needs a `ux` bump).

## Character order

A ranked set's later games pick characters in order on the game's own character select
(`Core/Orca/UX/CharOrder.*`; the ranked rules' Phase 4, Luis's spec of 2026-10-03):

- **The order is one constant** (since q2-stages: `CharOrder::kLaterGameOrder`, both games):
  - `Order::WinnerFirst`, the default since Orca 0.3.14 (Luis, 2026-10-04: "Winner chooses
    character first", then the loser; also the standard rulesets: Project+ 2024 Recommended B.6):
    the previous game's winner picks first ("Ada picks first · 0:42"), the loser sees that pick and
    picks second. 45 s each.
  - `Order::Free` (Luis's earlier pick the same day: "losing player chooses character first then
    winning player chooses. Winning player can choose right away if they want"): the loser is asked
    first ("Bo picks first · 0:42"); meanwhile the winner may lock in at any time ("Bo picks first ·
    0:42 · A locks yours in any time", then "Waiting for Bo"), and nothing blocks either of them.
    Once the loser has locked in (or their 45 s are up), the winner's own turn with 45 s, unless
    they locked in already: then straight on.
  Flipping it is the one line `constexpr Order kLaterGameOrder`. Brawl used to pick both at once
  (the interim order): it now plays the same order as Project+. Supernova 2025's exact order (the
  ban, the loser's stage, then the winner may change character, then the loser) still needs
  results -> stage select -> character select -> match, and the Meta Knight clause re-checked once
  both final characters are known (`MkFreePicker`, unit-tested, not wired).
- Then the stage select as the stage tracks run it.
- Game 1 has no order (both pick at once, the ranked ready timer applies), and nothing here acts in
  casual or friends games.

**How it plays** (WinnerFirst, since `ux=12`; Luis, 2026-10-04: "press A to select character and
then press start after to lock in and ready up. Then after you lock in opponent chooses and presses
start to lock in and immediately we go to stage select"; Free is the same with the loser first and
the winner free to pick and lock in during the loser's turn, then nothing once locked in). On the
winner's turn the loser's controller does nothing. The winner picks as the game does: A on a
character puts the token there, B with the token down takes it back up (the game's B, which the
online rules pass only for that, "B only takes the token back up"), A on the own token takes it up
too. **Start locks the pick in**, with the token down on a character: final, the winner's
controller then does nothing (the game never sees that Start: the order reads it from the queue's
raw latch, "The queue's character select"). Then the loser's turn, with the same buttons; their
Start locks in, and the stage select comes at once: in a queue2 room the queue goes on with no
ready step after the order ("The queue's character select"), elsewhere Orca presses Start for the
second picker, every other frame until the screen goes. The tokens stay down from one game's
character select to the next, so Start alone keeps last game's character. The overlay's top line
says whose turn it is, what to do and the time left ("Your pick · Start locks it in · B to change ·
0:42", "ada locked in · bo: pick and press Start · 0:31"), and a LOCKED IN badge in the player's
colour sits on each locked-in player's panel.

A player whose 45 s run out with the token in the hand (taken up with B or A) can only move and put
it down; Orca puts it on the character under the hand if there is one, and the stage select waits
until both tokens are down ("Waiting for ada to put the token down").

**Before `ux=12` (Orca 0.3.14 and 0.3.15), A anywhere in the grid locked the pick in and B was
masked: Luis's "stuck" (2026-10-04).** The tokens are still down on last game's characters, so a
player who moved onto another character and pressed A to take it locked the old one in (the game
does nothing with A there), and their controller went dead for the rest of the order; B, the
game's way to take a token back up, did nothing at all. Reproduced under the harness with the
build before this change (below, "Checked").

**Determinism.** The state is 16 bytes right after the match block (`kArea` = `0x806F2580`, the
same dead code of `sqNetAnyOkiraku::setNext`, short of `kMatchBlockLimit`, so the rules tracks' own
layouts of the block can't collide with it): magic `YGCO`, the game it belongs to, the step, the
port that picks first, the lock-ins, the hook frame the step began at and the frames into it. The
frame hook advances it at every boundary, first runs and re-runs (a re-run at the same boundary
finds its own write and writes nothing new: no press counts on a step's first boundary, and every
change resets the step's frames to 0), and the input gate's masks and presses come from that
state and the character select's memory alone. The lock-in's Start (and whether B was held on the
frame that just ran) is the queue's raw latch, `MatchBlock::QUEUE` +0x08 and +0x0C ("The queue's
character select", Determinism), which since `ux=12` runs under every header that locks, and with
`ORCA_TEST_CHAR_ORDER` on its character select: memory too. The gate's masks gained a synthesized
press
(`Mask::press`, applied after the mask), which Done uses for Start and the put-down A.

**Where it reads the set.** The match block's header (`YGM1`, version 1, mode 2 ranked, ruleset 1
Brawl or 2 Project+) and the ranked set's state as the ranked sets track lays it out: the games
won by ports 1 and 2 (`+0x21`, `+0x22`: the game number is one more than their sum, so a tied game
keeps its number and its order, which leaves the characters as they were), whether the set is
decided (`+0x23`) and the last decided game's winner (`+0x25`). `CharOrder::ReadSet` is the only
reader; another layout changes `CharOrder::Block`.

**Found under the harness** (ORCA_UX_PROBE watches and dumps, Brawl rev 2 and Project+ alike): the
character select's task is the scene's `+0x400`; player area i (port i) at task `+0x44 + 4 * i`:

| Offset | What |
|---|---|
| `+0x0C` | u32 buttons held (PAD bits; the stick's directions as the D-pad's) |
| `+0x10` | u32 buttons newly pressed on the frame that just ran, as the game saw them |
| `+0x1B4` | the player kind (1 a human) |
| `+0x1B8` | the character under the hand while the token is in it, else the token's (`0x28` none) |
| `+0x1F8` | u8 1 while the token is in the hand, 0 once it is down (`+0x1F9` 1 while it flies) |

A on a character with the token in the hand puts it there; A on a player's own token picks it up;
B takes it back to the hand; A elsewhere with the token down changes nothing (both games).
`gfPadSystem` (`0x805A0040` ->) keeps only held buttons (port i at `+0x40 + 0x40 * i`), no newly
pressed ones.

**Test knob** `ORCA_TEST_CHAR_ORDER=<pplus|brawl>:<game>:<winner port 1-2>[:free|winnerfirst]` (a
test override, both processes alike): that set on every character select with no match block behind
it, so the order runs on the boot's character select once both players have joined; the order is
`kLaterGameOrder` unless given.

**Proof** (2026-10-03, `ORCA_TEST_CHAR_ORDER`, headless): `YG_SYNCTEST=7` from the boot's
character select through the order, Orca's Start, the stage select and the match's start, the
same step changes as the plain runs, 0 RAM mismatches:
- Project+ `pplus:2:1` with `Tools/orca/inputs/pplus-char-order.txt` (port 2 down first, port 1
  joins and picks first: its A locks in at frame 485, port 2's 45 s run out at 3185, Orca's Start,
  the stage select at 3189): 21,651 re-run frames.
- Project+ `pplus:2:2`, the same inputs (port 2 picks first and lets its time run out at 3065,
  port 1 puts its token down at 3165, the stage select at 3169): 21,651 re-run frames.
- Brawl `brawl:2:2` with `Tools/orca/inputs/bf-char-order.txt` (both at once; port 2's A on its
  own token picks it up and counts for nothing, its next A locks in at 1929, port 1's at 2009,
  the stage select at 2013): 5,201 re-run frames.
In every run the masked presses (the off-turn port's, B and Start) did nothing. `OrcaCharOrder*`
(18 tests) and every `Orca*` unit test pass. (That proof is of the A lock-in before `ux=12`; the
Start lock-in's is "Checked 2026-10-04, the character select's buttons" under "The queue's
character select".)

## The character select's title

Luis (2026-10-04): the part of the character select that says "versus" reads CASUAL or RANKED in a
queue match, and FRIENDS with friends (`Core/Orca/UX/CssTitle.*`).

**What it is.** The logo at the top left of the Versus character select, Brawl's brushed BRAWL and
Project+'s VERSUS, is a texture, not text: `MenSelchrPanelRule3.1` (Brawl 132x48, Project+ 128x56,
C8 with an RGB5A3 palette), one frame of the tab's pattern animation (3.2 is TEAM BATTLE, 3.3
SPECIAL BRAWL / SPECIAL VERSUS), in the character select's archive (`sc_selcharacter_en`, MiscData
30's bres; Project+ ships its own copy on the SD card). The character select's message files hold
only "PLAYER", "CPU", the handicap and the Wi-Fi texts. So no text to rewrite, and the pixels are
the game's art.

**How.** Orca marks the texture's palette and its texture pack draws each mark. The texture's
background is one transparent palette entry (Brawl's entry 0, `0x0EEE`; Project+'s entry 7,
`0x0000`). For a mode Orca moves that entry's blue by one to three steps (Brawl `0x0EED` CASUAL,
`0x0EEC` RANKED, `0x0EEB` FRIENDS; Project+ `0x0001`, `0x0002`, `0x0003`): alpha stays zero, so
nobody sees the colour, but the palette's hash changes, so Dolphin names the texture differently
for each mode (`tex1_132x48_44a1dd712ec999f0_<palette hash>_9`, `tex1_128x56_57564f7b1ba3bc1c_..._9`)
and the pack has a label for each: our own art (`Tools/orca/textures/make-labels.py`, SIL OFL
fonts), Brawl's in the gold of its other rule titles with small capitals like WINNER OUT's (Archivo
Black), Project+'s in VERSUS's mint with a black outline where VERSUS's letters sit (Anton), each
4x with mip levels like the Online menu's labels. A session always loads the pack ("Online menu");
without it the game's own title shows, unchanged to the eye (a tint of one blue step can reach the
letters' edges through filtering, at most).

**What it says**, a pure function of emulated memory and the ports in effect at the boundary (the
session's synced plan), so both machines write the same word at the same frame:

1. the match block's header: a casual or ranked queue room, CASUAL or RANKED;
2. the main menu's pick Orca keeps in sqVsMelee for the back out ("Online menu": `+0x18`, 24-27):
   a With Friends button, FRIENDS;
3. two ports or more (another machine's player plugged in) and no queue header: a friends room,
   FRIENDS (a friend who dropped into Group > Brawl or Project+'s boot character select too);
4. the pick again: With Anyone's Casual (30) or Ranked (31), CASUAL or RANKED (the character select
   a search waits on, before a room and after one);
5. else the game's own (Group > Brawl, Project+'s boot, everything offline).

The friends flag needed no new state: the pick is already in sqVsMelee's object (sora_scene's
sequences are made once at boot; the running one is the scene manager's `+0x10`), every snapshot
and keyframe carries it (a joiner's title is its host's), and the back out zeroes it. Friends
rooms' game behaviour is unchanged: the word is a colour the game only draws.

**Where** (Brawl rev 2 and Project+, from harness RAM dumps): the scene manager `0x805A0060` +4 is
the scene (`scSelctCharacter`), whose `+0x410` is the character select's gfArchive; one of its first
32 words is the archive image (`ARC\0`, named `sc_selcharacter_en` at +0x10). MiscData 30 is the
bres; its root group names `Palettes(NW4R)`, which names the PLT0 `MenSelchrPanelRule3.1` (format
RGB5A3; 96 entries in Brawl, 256 in Project+). The frame hook writes the word at every boundary,
first runs and re-runs, after the rules (so a header written at this boundary counts), only while
it holds the game's value or a mark and only when it differs. The archive loads fresh each time the
character select starts, so a mark never outlives it, and the game's value is written back when
nothing calls for one. It changes emulated memory: the UX compatibility version covers it (the
integrator's bump to `ux=10`).

**Proof** (2026-10-04, branch `q2-css-title` on `rel-0.3.13`, Apple Silicon, headless):
`Tools/orca/inputs/bf-css-title.txt` (Brawl: With Friends > FIGHT!, B out, Casual, B out, Ranked, B
out, Group > Brawl) and `pplus-css-title.txt` (Project+: its boot character select, then the same
three picks). The log's `the character select's title reads ...` lines give the word at each
character select: Brawl FRIENDS `0x0EEB` (frame 1707), CASUAL `0x0EED` (2262), RANKED `0x0EEC`
(2717), nothing written at Group > Brawl (3322, the game's `0x0EEE`); Project+ nothing at the boot
(163), FRIENDS `0x0003` (718), CASUAL `0x0001` (1273), RANKED `0x0002` (1728). Screenshots (`-v
Metal`, deleted after) show the labels in both games at each of them, and BRAWL / VERSUS where
nothing is marked, so the pack's names are the textures' real hashes. `YG_SYNCTEST=7` over the same
walks: Brawl frames 1500-4700, 22,351 re-run frames, Project+ 150-3600, 24,101; **0 RAM mismatches**
in both. `OrcaCssTitle*` unit tests (what the title says in each room, the pick read only from a
running sqVsMelee, the slot and its checks, the marks' values, idempotence and only that word
written) and the texture pack's (`OrcaOnlineMenuTextures*`, now with paletted names); 299 `Orca*`
tests pass.

**Rechecked** 2026-10-05 at `ux=18` (branch `q10-menus`, headless): With Friends now goes straight
to its character select and B out of it lands on ONLINE, so both scripts lost the old page's FIGHT!
and one B. Brawl: FRIENDS `0x0EEB` (select from frame 1657), CASUAL `0x0EED` (2212), RANKED
`0x0EEC` (2667), nothing written at Group > Brawl (3272). Project+: FRIENDS `0x0003` (424), CASUAL
`0x0001` (980), RANKED `0x0002` (1435), nothing written at VERSUS (2040).

Not yet: the header case (a queue room's) is proven by the unit tests only, not under the harness
(`ORCA_TEST_QUEUE` locks the way back out of the walk); team mode's TEAM BATTLE frame (3.2) is never
marked (online rooms don't use teams); a Project+ update that redraws VERSUS changes its hash, and the
game's title then shows until the pack is regenerated.

## Native text

Luis: Orca's UI must look native. Brawl draws its menu words from **message buffers**: a window
formats a message into a buffer of its own once, then draws that buffer every frame until the game
formats that window again. A buffer Orca rewrites is drawn in the game's font, size, colour and
place from the next frame on (`Core/Orca/UX/NativeText.*`, branch `q5-ui-native`, `ux=13`).

**The buffer** (Brawl rev 2, from one-frame RAM dumps of the character select, the stage select and
the results screen): an object whose first word is its vtable `0x804540E0`; `+0x28` capacity,
`+0x2C` length, `+0x30` the buffer, `0x01` after the length. The buffer is the window's set-up
(`0x17 ...`, ending with its colours `0x02 n 0x0C rgba`, `0x04 rgba`, then `0x03 0x00 0x00`, 43
bytes for the menus' windows), then the message: UTF-8 words between inline codes (`0x11` + 4
bytes, a width, it seems: a 44-character line written without the rules bar's codes was not drawn
at all; `0x12 0x02 0x0C rgba` a colour, `0x13` its end). Orca replaces only the words and keeps every code, so its text gets the
game's colour and fit (a longer line is squeezed to the box, as the game's own are). The font has
ASCII and Latin-1 (`Pokémon`, the full-width `１`) but no middle dot or en dash: the words are ASCII.
Proven by probe writes (`ORCA_UX_PROBE` `write`): one write, still on screen 150 frames later.

**Built (Brawl, the Versus character select, muSelCharTask = scene `+0x400`):**
- **Name plates.** Player area i (task `+0x44 + 4i`, whose `+0x1B0` is i) `+0x174` is its plate's
  buffer ("PLAYER 1", "NONE"). A joined port with a name shows it in capitals like the game's plates;
  until design F also in a ranked queue room with the rating from its queue identity ("BO 1532"),
  since then the name alone ("Design F" below). Any session with names (friends rooms too). The overlay's label over that panel goes; the LOCKED IN badge stays.
- **The rules bar** (task `+0x538`, the white bar right of CASUAL / RANKED that says "3-man
  survival test!" after the stock count): in a queue room and on the queue's own character select
  it says what happens now, for both players in the third person, with the timer:
  "Ranked: pick, then press Start to search", "Ranked: searching for an opponent...  B to cancel",
  "Game 1: Press Start to lock in  0:27", "Game 1: ADA locked in, BO to press Start  0:24",
  "ADA 1-0 BO: ADA picks, A then Start  0:43" (later games give the score in place of the game),
  "ADA wins the set 2-0". The overlay's top line, the set's score line and the character order's
  line go; this machine's own hints ("Hold Z to find someone else" in casual, "B to change") move
  up to the top line.

Both are a pure function of emulated memory (the header, the queue's ready flags and timer, the set,
the character order) and the synced ports (names, the queue identity's rating), written at every
boundary, first runs and re-runs, only where they differ, after the other writers (`UX.cpp`, after
`CssTitle`). Never "you": both machines write the same bytes. The game formats a plate again when a
port joins, leaves or picks a tag; the next boundary puts the name back (one frame of the game's
words). **Project+** (since 0.3.18, `ux=14`): the plates only. A Project+ dump has the same boxes at
the same offsets (the bar says "-stock Smash!", the plates "PLAYER 1"), but its bar's message has no
width code (`0x12 0x02 0x0C 000000ff -stock Smash! 0x13`), so a line longer than the bar would need
Orca to add one (Brawl's `0x11 0x29 ff ff ff`); until that is checked on screen the bar stays the
game's (`Apply`'s `rules_bar` false) and the overlay docks its lines on it ("Seamless UI").

**The survey: each Orca UI element, and where the game could show it**

| Element | Orca today | In the game | Stored as | Deterministic from the block? | Recommendation |
|---|---|---|---|---|---|
| Top status line on the character select (whose turn, what to do) | overlay, top centre | the rules bar | message buffer (task `+0x538`) | yes, third person | **native, built** |
| Timers (ready 0:30, character pick 0:45) | overlay, in the line | in the rules bar's line | message buffer | yes (block frames) | **native, built** |
| Searching / pick then Start (the queue's own character select) | overlay | the rules bar | message buffer | yes (alone) | **native, built** |
| Player names | overlay labels over the panels | the panels' name plates | message buffer (area `+0x174`) | yes (synced names) | **native, built** |
| Ratings | overlay labels ("ada · 1532") | the name plates ("BO 1532") | message buffer | yes (queue identity, synced) | **native, built** |
| Set score | overlay ("Game 2 · You 1–0 bo") | the rules bar ("ADA 1-0 BO: ..."); the results screen's rule panel (its "Brawl", "Stock" and "1" took "Ranked", "Set" and "1-0" in a probe write; an en dash drew as a box) | message buffers | yes (SetBlock) | **native**: built on the character select, next on the results screen |
| LOCKED IN | overlay badge over the panel | no per-player word; the plate's colour codes (`0x02`/`0x04` rgba in its set-up) could tint a locked-in player's plate; READY TO FIGHT is one texture for both | message buffer set-up | yes | kit badge now; next: tint the locked-in plate (native) |
| Stage select's top lines (strikes left, whose turn, timer) | overlay, top centre | none on screen: its only window is the Stage Builder name (hidden unless a custom stage is hovered); the stage name and BRAWL are textures, the stock count a digit model | message buffer (hidden) | yes | **game-styled kit** |
| Stage proposal markers, cursor owner | overlay rings and tags | the game has one cursor model, driven by one port; a second needs game code | model | n/a | **kit** |
| Victory / defeat, rating change | overlay panel (q5-set-end) | the results screen's winner name ("Mario" took "ADA"), mode ("Brawl" took "Ranked") and rule panel are message buffers; but VICTORY and DEFEAT differ per player and the new rating comes from the server after the frames are final | message buffers | the shared facts only | **hybrid**: "ADA wins the set 2-0" native on the results screen; VICTORY/DEFEAT and the rating count in the kit |
| Disconnect countdown | overlay notice (q5-set-end) | none: during a stall the CPU waits, no frame runs, the game draws nothing new, and only one side knows | — | no | **kit** (host-drawn, RepresentDuringStall) |
| "Hold Z to find someone else" (casual only), "B to change", the skip bar | overlay | per-machine hints | — | no (local hold) | overlay, one small line |
| READY TO FIGHT banner, the mode title | the game's | textures (MenSelchr...) | texture | a palette mark + Orca's pack (CssTitle's way) | keep the game's |

**Proof** (2026-10-04, `q5-ui-native` on `rel-0.3.16`, Apple Silicon, headless):
`Tools/orca/inputs/bf-native-text.txt` (a ranked queue2 set: the player's own character select,
Samus steered in for port 2, game 1, the character order of game 2, game 3's character select).
`YG_SYNCTEST=7 YG_SYNCTEST_FROM=1440` over it to frame 8000 (the menus, both character selects of
game 1, the stage select, two fights and results, game 2's character order, game 3's select after the
set): 45,871 re-run frames, **0 RAM mismatches** (PASS; the device-state-only count is the usual host
noise). Screenshots `~/Documents/brawl-dolphin/ui-shots/native/` (before: `rel-0.3.16`; after:
this branch). `OrcaNativeText*` unit tests (11): the boxes as the game formats them, anything else
refused, the codes and length kept, idempotence, the capacity, ASCII, the friends room's plates,
nothing off the character select, the same bytes on both machines, each queue state's line, the
character order and the set in the third person.

Not yet: Project+'s rules bar (the width code above); the results screen's
writes (the boxes are found, the writer is not); the plate tint for LOCKED IN; a header cleared while
the character select stays up leaves the bar's last words (the queue image's restore brings the
game's back in practice).

## Seamless UI

Luis, 2026-10-04: everything Orca puts on screen should look like the game drew it ("integrated into
the games UI itself ... SEAMLESS"; "native menus, no custom UI"). Orca 0.3.18 (`rel-0.3.18`, `ux=14`)
does that two ways, the same information as before in each place:

- **Native text** ("Native text" above): where the game has a text box on screen and the words are
  the same for both players, Orca writes them into the game's own message buffer, a pure function
  of emulated memory and the synced ports (it writes memory, so it is in the compatibility key).
- **The kit** (`Core/Orca/UX/Kit.*`): everything else is drawn by the host over the picture, local
  display only, with the kit's few shapes in the game's own fonts (read at run time from the
  player's disc, `system/font/`, never shipped) and the look of that game's menus: Brawl's rose
  plates in a white rim and a black line with cream-to-gold titles (the character select's RANKED
  plate), Project+'s flat teal slabs edged in black with white-to-mint titles (its RANDOM and PAGE 2
  buttons, its RANKED title). The frame hook picks the look from the session's ruleset. Without the
  disc (or before its fonts are read) the same shapes fall back to ImGui's font.

Every element and how it is drawn now (before 0.3.18: plain outlined text, ImGui shapes):

| Element | Brawl | Project+ |
|---|---|---|
| Queue character select's line (pick, searching, lock in, the ready timer, both locked in, time's up) | native: the rules bar, third person | kit: docked on its rules bar (below) |
| This machine's hints (Hold Z to find someone else, B to change, the skip bar) | kit: a small plate under the top edge | kit: a plate under the docked bar's right end |
| Names and ratings over the panels | native: the name plates ("BO 1532") | native: the name plates (since 0.3.18) |
| LOCKED IN | kit: a badge in the port's colour across the panel's picture | the same |
| Set score, character order's turn | native: the rules bar ("ADA 1-0 BO: BO picks, Start locks it in 0:45") | kit: the docked bar's second line |
| Stage select lines, strikes and bans, whose turn | kit: a banner, top centre | the same |
| Timers (ready, pick, strike) | native in the rules bar on the character select; kit timer plate on the stage select | kit timer plate (white, gold under 10 s, red and pulsing under 5) |
| Stage select cursors, name tabs | kit pointers: a ring in the port's colour, its tab, a gold ring on the turn's player | the same |
| Proposal markers | kit frames round the tile in the proposer's colour, gold with both names when both agree | the same |
| Search (Online menu), "Opponent found" | kit banner | the same |
| Results screen's set line ("Game 2 - 1-0", "Set over - 2-0") | kit banner | the same |
| VICTORY / DEFEAT panel, the rating's count and change | kit: the look's panel, VICTORY in the heavy face in gold (DEFEAT red), the rating in the heavy face, the change on a green or red badge | the same |
| Disconnect countdown | kit: a panel with the notice and a countdown disc | the same |
| Set over's "Hold Z to leave" | kit: a panel plate with the hold's bar filling in gold | the same |
| Toasts (joined, left) and chat | kit: panel plates, one per chat message, wrapped, the sender in the look's accent | the same |
| "Waiting for <name>..." | kit title, under the top centre's banners | the same |
| The link's lamp, ping and frame meter (and its graph) | kit: the panel disc with lit arcs, the lines beside it; the graph on a plate | the same |

**Project+'s rules bar.** Project+'s bar has the same message buffer as Brawl's but no width code,
so a line longer than the game's own isn't drawn ("Native text"). On a queue's character select
(`Queue::OnQueueCss`, its own or a room's) the overlay instead docks its lines on that bar: a teal
banner over the bar's place (x 520..1265, y 42..105 of a 1280x720 picture: 0.40..0.99 of the width,
0.056..0.148 of the height), what happens now on its first line and the set's line under it; the
hint and the timer sit on plates under the bar's right end, over the grid's empty corner. The
stock count the bar showed is the queue's locked rule anyway.

**Where the native boxes say it, the overlay doesn't.** `NativeText::Shown()` tells the overlay which
plates name their player (it drops that panel's tab; LOCKED IN stays) and whether the rules bar says
the queue's line (its top lines go; this machine's hint stays, small). The queue's own character
select shows no set line (a ranked header is there before any set).

**Design checks.** `ORCA_UX_KIT_DEMO=1` draws every primitive over the picture.
`ORCA_UX_SETEND_DEMO=victory|defeat|countdown|casual|left|stay` plays a set-end screen; with
`ORCA_UX_SETEND_DEMO_AT=<frame>` in a harness run it starts at that frame and runs on the game's
frames (`Harness::ShownFrame`), so a screenshot at a frame always shows the same moment.
`ORCA_UX_OVERLAY_DEMO=1` cycles a made-up friend's link (good, ok, bad with a stall) and adds a
toast and two chat lines each round.

**Checked 2026-10-04** (`rel-0.3.18`, M4 Pro, Mac build, headless, nice'd, under the run locks):
- Build: `dolphin-emu-nogui` and `tests`, no new warnings. Unit tests: `Orca*`: 417 pass, none fail (
  the disc, live and network ones skip without their switches; the kit's font test read both
  faces from the disc), including `OrcaNativeText.ProjectPlusKeepsItsRulesBar` and the kit's
  chat plate, banner width and badge size checks.
- Brawl, `YG_SYNCTEST=7 YG_SYNCTEST_FROM=1440` with `ORCA_TEST_QUEUE=ranked:solo
  ORCA_UX_TEST_NAMES=ada,bo ORCA_TEST_SET_RULES=1,8 ORCA_TEST_QUEUE_PICK=2106:3:2:-7.5:14.23:1532`
  over `bf-native-text.txt` to frame 8000 (the queue's own and the room's character selects with
  the plates and the rules bar written, two stage selects, two fights and results, game 2's
  character order, the select after the set): 45,871 re-run frames, **0 RAM mismatches**.
- Project+, `YG_SYNCTEST=7 YG_SYNCTEST_FROM=200` with `ORCA_PROFILE=PPLUS32
  ORCA_TEST_QUEUE=ranked:solo ORCA_UX_TEST_NAMES=ada,bo
  ORCA_TEST_QUEUE_PICK=874:43:2:-7.4:7.7:1532` over `pplus-css-lockin.txt` to frame 6700 (the
  plates written on the queue's own and the room's character selects and on game 2's, the stage
  selects, the fight, the results): 45,451 re-run frames, **0 RAM mismatches**.
- On screen: before/after pairs of every screen in both games,
  `~/Documents/brawl-dolphin/ui-shots/convert/` (README.txt there); 10-letter names with a rating
  ("CRUNCHYTES 1532") fit both games' plates, squeezed as the game squeezes its own.

Not yet: the results screen's native words (its boxes take text, the writer isn't built), a native
LOCKED IN (the plate's colour codes), Project+'s rules bar (the width code). No two-Orca or Mac<->PC
run for these changes: they come after the release, as for every release.

## Design F

Luis, 2026-10-05, after five directions and three rounds of renders
(`~/Documents/brawl-dolphin/ui-design/`, private): "relabelled for almost everything", B's character
portraits, the stage select's turn cursor pulsing, a subtle set count at the top; the results screen
as 0.3.18 has it; no rating by the names. Chat and the room's notices in YouGame's own look. Built on
`q6-ui-f` (`ux=16`):

- **Portraits** (kit): whoever picks now in the character order gets a frame in their port's colour
  with a soft glow round the portrait (`CharOrder::Picker`, pure, published as `CurrentPicker`); a
  locked-in player's is gold with a LOCKED IN ribbon across its top (it replaces the badge). On the
  queue's own select, while it searches, a gold arc goes round the empty seat.
- **The turn's cursor pulses** (kit `Pointer`, `TurnPulse`): it swells 14% on a cosine beat, a glow in
  the player's colour breathes with it, a ring ripples out once a second. No more gold ring.
- **The set's dots** (kit `SetDots`, `RankedSet::DotsOf`): three dots at the top centre of the
  queue's character select and the stage select, each decided game's filled in its winner's port
  colour, the game being played rimmed brighter.
- **Names alone on the plates** ("BO", not "BO 1532"). On the queue's own select, while it searches,
  the opponent's empty seat reads SEARCHING (the game's "  NONE" back when it stops).
- **Relabelled words** (`Core/Orca/UX/Relabel.*`): the game's big words say what to do now, through
  the texture pack (`Tools/orca/textures/README.md`), CssTitle's way: Orca marks a texture with a
  value nobody sees so it hashes to another name, one per label, and the pack has our art for each.
  - READY TO FIGHT! (`MenSelchrReady01_2`, C4 352x36, in the character select's archive): its
    transparent palette entry's blue (Brawl entry 0, `0x0222`; Project+ entry 15, `0x0000`) plus
    the label: PRESS START TO LOCK IN! (a queue room's ready step, until both are in, the match
    starts or the time runs out), WINNER PICKS FIRST! and LOSER PICKS NOW! (the character order;
    under its Free order the loser's turn alone has words).
  - STAGE SELECT (`MenSelmapBack07`, I4, Brawl 128x16 under the stage list, Project+ 189x30 over the
    preview, in `sc_selmap_en`'s MiscData 20, found by scanning the scene object's words for the
    archive): its first texel's high nibble plus the label (Brawl's is 1, Project+'s 0). Dolphin's
    texture cache always samples a texture's first 8 bytes (`GetHash64`), so the change is seen at
    once. P1 STRIKES 1-3, P2 STRIKES 1-3, P1/P2 BANS 1-2, P1/P2 PICKS, PICK A STAGE (a casual game's
    both-at-once pick): the step under way (`BrawlStages::CurrentTurn`, `RankedPPlus::CurrentTurn`,
    `Ranked::Turn`), the count the strikes or bans still to make.
  - The stage select's BRAWL / VERSUS (`MenSelchrRtitle.30`, I4 120x24): RANKED or CASUAL from the
    header.
  - Project+'s stage select legend (`CornerInfo` I4, `CornerButtons` IA8): STRIKE, PROPOSE, TAKE BACK
    over X, A, B while a ranked set's two cursors run (not casual's: X does nothing there).
  Each slot is written only when it holds the game's value or one of its marks (anything else is not
  the texture this knows), only where it differs, first runs and re-runs, a pure function of the
  block, the queue, the character order, the stage flows' steps and the synced ports. The archives
  load fresh each time their screen starts; with nothing to say the game's value goes back. Without
  the pack the game's words show (an I4 mark is one texel in a corner, up to 14 of 15 bright).
- **Timers in the game's lettering** (kit `GameTimer`): with READY TO FIGHT! up (both tokens down)
  and relabelled, the turn's timer sits at the band's right end in the band's lettering, and the
  rules bar leaves its own out (`Relabel::ClockInBand`, pure); with STAGE SELECT relabelled, the
  timer follows the label in its rose (Brawl) or grey (Project+), and the stage select's banners go
  (the label, the dots and the guide say it).
- **The control guide** (kit `Guide`, `GuideOf`): this machine's hints ("B to change", "Hold Z to
  find someone else", "X on a stage strikes it") as GameCube buttons and short words in the games'
  own corner for them, bottom right, in place of the small plate at the top; one line along the
  bottom edge, words without a button (a proposal, the casual pick's rule) in it too, drawn smaller
  (to 70%) when longer than the picture is wide. On Project+'s ranked stage select the relabelled
  legend says X, A and B, so the guide leaves them out.
- **YouGame's layer** (`Core/Orca/UX/YgCard.*`): chat and the room's notices as the SDK's slate cards
  (web/public/sdk.js), the sender's avatar in the site's colour and initial, YouGame's mark, in Roboto
  (`Data/Sys/Orca/Fonts`, Apache 2.0, ImGui's own copy). Not the game's look on purpose: they are
  YouGame's, in every game, like Steam's overlay.
- Stage select tabs read in capitals ("P1 ADA").

Not in F: the results screen, VICTORY/DEFEAT, the rating's count and the disconnect countdown are
0.3.18's.

## Ranked sprint integration

Branch `ranked-sprint`: `sp-casual` (Online rules), `sp-results` (Ranked sets), `sp-pplus-sss` (Ranked
stage flow), `sp-brawl-sss` (Ranked stages (Brawl)) and `sp-char-order` (Character order) on
`ranked-spec`, made into one. The sections above describe each track as built; where they differ,
this section wins.

**One header writer.** `OnlineRules::WriteHeader`, at an alone boundary only (since 0.3.15 also
one `HeaderFreeAt` allows: "The match block's header"), is the only code that
writes the match block's header (`MatchBlock.h`): magic, version, mode, ruleset, **coin** (+0x07:
game 1's first striker, one bit of the FNV-1a hash of the room code, the same in both games and on
both machines; the server makes the code, neither player picks it) and the **room's hash** (+0x08).
It writes only over sora_scene's dead code (its first word) or a header, never into memory the game
hasn't loaded the module into yet; a clear puts the dead code's first word back. Any new header
(from none, or another mode, ruleset, coin or room) starts every track's state fresh: the Project+
flow's `MatchBlock::State` (its "none" ports), `SetBlock`'s set and the character order. The tracks'
own writers (`RankedPPlus::RequestHeader/ApplyHeader`, `RankedSet::SetRoomMode/ApplyRoomMode`) are
gone, and so are their knobs (`ORCA_TEST_RANKED`, `ORCA_TEST_MATCH_BLOCK`): **`ORCA_TEST_QUEUE=casual`,
`ranked` or `ranked:1`** (port 2 strikes first) turns on either game's flows in a harness run.

**The block, by offset from `0x806F2380`:** +0x00 header (+0x0C the queue's flags, "The queue's
character select"); +0x10..+0x8F Project+'s flow
(`MatchBlock::State`); +0x80..+0xBF Brawl's flow (`BrawlStages`, its own game); +0xC0..+0xCF Phase
1 (the ready timer: `CSS_TIMER`, `CSS_NO_SHOW`, `FOUGHT`, `PLUGGED`, `CSS_TIMER_START`);
+0xD0..+0x1A7 the player's saved rules (Project+'s stage switch data from +0x100), except
+0xF4..+0xFF, the stage select's two cursors' shared bytes; +0x1B4..+0x1BF the two cursors (since
`ux=12`, "The stage select's two cursors"); +0x1C0..+0x1EF the queue's character select; +0x1F0 the saved rules' flags; +0x200..+0x20F the character order; +0x210..+0x28F the
ranked set (`SetBlock`, authoritative: the room hears it, Brawl's flow and the character order
read it).

**One step machine.** `RankedSteps.{h,cpp}` (namespace `Ranked`) is the only one. Brawl's flow now
runs on `Ranked::Brawl2025` through its `Progress` driver (`Begin`, `Strike`, `Skip`, `Pick`, `Tick`,
`AutoPick`): game 1 is strike 1, strike 2, then the first striker picks one of the last two with A
(the same as Project+); later games the winner may ban 1 (X, or Y to skip; the timer skips), then
the loser picks with full DSR. Its coin is the header's, no longer the RNG seed's.

**One input-gate source** (`UX.cpp`): the online rules' locks, Project+'s flow, Brawl's flow and the
character order, ORed (a button or stick any rule takes is taken; presses add up).

**One overlay slot**, top centre: Brawl's flow's lines, else Project+'s, else the set's score
(`RankedSet`); under it the turn, else the character order's line.

**Rules that changed in the merge:**
- The ranked ready timer counts from a start frame (`CSS_TIMER_START`), not by adding one per
  boundary, so a boundary run again after a load computes the same.
- Ranked Brawl's stocks and time are `RankedSet`'s (3 and 8:00; 1 and 3:00 in a tiebreak game); the
  locks no longer overwrite them every boundary.
- Meta Knight is `0x18` in gmCharacterKind (BrawlHeaders `gm_lib.h`), in the set's ledge limit as in
  the stage flows; `0x16` there is Ganondorf.
- The character order reads the set at `SetBlock`'s offsets (+0x211, +0x213, +0x215).

**Checked 2026-10-03** (M4 Pro, Mac build; the sprint's gates: build, unit tests, one sync test per
game, one queue e2e each way):
- Unit tests: 1,332 pass (`Binaries/Tests/tests`, run from `build/`).
- Brawl, `YG_SYNCTEST=7 YG_SYNCTEST_FROM=1700` with `ORCA_TEST_QUEUE=ranked ORCA_UX_TEST_NAMES=ada,bo
  ORCA_TEST_SET_RULES=1,8` over `Tools/orca/inputs/bf-ranked-bo3.txt` to frame 9000 (character
  select, game 1's strikes and pick, the fight, results, game 2's character order): 51,051 re-run
  frames, **0 RAM mismatches**.
- Project+, `YG_SYNCTEST=7 YG_SYNCTEST_FROM=170` with `ORCA_PROFILE=PPLUS32 ORCA_TEST_QUEUE=ranked:1
  ORCA_UX_TEST_NAMES=ada,bo` over `Tools/orca/inputs/pplus-ranked.txt` to frame 15000 (a whole 2-0
  set: game 1's X strike, timed strikes and pick, game 2's character order winner then loser, the
  winner's two strikes, the loser's timed pick): 103,761 re-run frames, **0 RAM mismatches**; the
  flow's score and `SetBlock`'s agreed at every game.
- `Tools/orca/queue-e2e.mjs` against yougame.co with dev tickets, Brawl: `--queue casual --games 2`
  PASS (Start went to Yoshi's Island, then Lylat Cruise; Ice Climbers in the match); `--queue
  ranked` PASS (2-0 set, 3 verdicts the same on both sides; game 1's stage by the strike and pick
  timers, Smashville; game 2's loser's pick by its timer, Battlefield; Final Destination grey for Ice
  Climbers on the stage select, the set line and the turn line on screen).

**Known gaps:** Project+'s flow still counts its own score from the results screens of the fights it
started (it agrees with `SetBlock` for Project+: both take the results screen's winner, and a tie
counts for neither). Brawl's exact Supernova order (ban, the loser's stage, then characters) is not
built: Brawl's later games are characters (both at once, 45 s), then the ban, then the pick.

## The queue's character select

Luis's spec of 2026-10-04 (Slippi's flow): Casual and Ranked land on a character select where the
player sees only their own panel, picks, and presses Start; only then does the page search. The
opponent arrives with their character already picked; both press Start to ready up (30 s in ranked
and in casual's first game; casual's later games wait); in casual a long Z finds someone else (ranked
has no skip: Luis, 2026-10-05, "You are forced to play a ranked match"). Orca's half of the `queue2` contract with the YouGame page (its half:
`lib/orca-queue.ts`, `OrcaEmbed.tsx`); code in `Core/Orca/UX/Queue.*`, the session's part in
`Rollback/OnlineMatch.cpp` (the queue image), tests `OrcaQueue*`.

| stdout (with `caps queue2`) | When |
|---|---|
| `orca queue ready <casual\|ranked> <char> <costume>` | On the queue's own character select the player has a character down and pressed Start: search with this pick. Printed again with a new pick, and again when the player is back on that character select still ready after a room (below). `<char>` is the character select's own id (`muSelCharPlayerArea` +0x1B8, 0x28 none), `<costume>` its colour number (+0x1BC). |
| `orca queue unready` | The player pressed B, took the token up, or the page's `queue-cancel` came, while ready: stop searching. Still on the character select, except after A on BACK (next line). |
| `orca menu cancel` | B while not ready backs out to the menus, as the game does, and so does A on BACK (`ux=18`): the search is over. A on BACK while ready prints `orca queue unready` first, then this. |
| `orca queue skip` | Casual only: Z held 1.5 s on the room's character select (both players in, before the game goes on): the page leaves the room and searches again. Local only: nothing in the game changes. Never in ranked (since 0.3.19, `Queue::MaySkip`): no line, no bar, no hint in any game of a set. |
| `orca queue timeout me\|them` | Casual's first game with this opponent: the 30 s ready timer ran out (at a final frame, the same on both machines; casual's later games have no timer, so never then). `me`: this player wasn't ready (the page leaves and stops searching); `them`: the opponent wasn't (the page leaves and searches again). Nothing moves on the character select meanwhile. |

The page owns the room: Orca never leaves on a skip or a timeout by itself.

### How it plays

- **The queue's own character select.** With Anyone > Casual or Ranked, with the app's `queue2`,
  starts `Queue::Begin`; the session then asks for a header with `FLAG_SOLO | FLAG_QUEUE2` (the
  header's +0x0C) at the player's alone boundaries (`WantedRulesMode`: no queue room, port 1, no
  session). The rules lock as in a queue room (`OnlineRules.h`), A only in the grid; ports 2-4 are
  masked entirely; Start never reaches the game (one player couldn't start a versus match anyway):
  it readies the player, through the raw latch below. While ready, B only un-readies. B while not
  ready reaches the game, and the loop lock's toggled group stands aside for `FLAG_SOLO` (`when
  806F238C & 01000000 == 00000000` in both patch files), so held B backs out to the menus.
  The B that stops the search is kept from the game until it is let go, so the token stays down.
  A on BACK backs out as held B does (`ux=18`, "The character select's buttons"); while ready the
  queue keeps it from the game for its first frame and un-readies (`FLAG_BACK_A`), then presses A
  for the game the frame after, so one press stops the search (`orca queue unready`) and backs out
  (`orca menu cancel`), in that order.
  Overlay: "Casual" / "Pick your character, then press Start to search", then "Casual ·
  Searching for an opponent…" / "B to cancel". Since `ux=19` the player's own panel is the only
  one on screen while picking; searching, the opponent's seat shows (SEARCHING, the kit's ring),
  and panels 3 and 4 never do ("No CPUs online"; before, every empty panel read NONE).
- **The pick's identity.** At `ready`, Orca keeps the pick (character, costume, and where its hand
  was over that character: the middle of its way over it, from where the hand first read it to
  where it put the token down, so inside the square even when the hand stopped on its edge) and the
  page's `queue rating`, as the
  player's queue identity (`Queue::Identity`, 13 bytes): it rides the session's value channel with
  the player's name and controls (`Events::PortInfo::queue`, the `nm`/`kf` entries' fifth element,
  the joiner's hello `qi`), so every machine has the joiner's at its plug frame.
- **The room's character select** (header `FLAG_QUEUE2`, written by the host with the app's
  `queue2`). Game 1: the host's pick is locked once down (A, B, X, Y masked); port 2's pick is put
  in from its identity by the input gate (`Mask::steer`, a held stick, and synthesized presses):
  its own controller does nothing; the stick stays centred 20 frames (the game takes a newly
  plugged pad's stick as its centre, its origin: steered then, the stick would read as pushed ever
  after), then takes the hand to that place (full tilt from 3 units out, the larger component never
  under 30, past the dead zone); A puts the token down once the hand is over that character with the
  token in it and has all but stopped (the character the hand reads lags it by a frame; Brawl's hand
  creeps about 0.03 a frame with the stick let go, which counts as stopped); X turns the costume
  until it matches (3 s at most; port 1's own colour on the same character can't be had, so any
  colour does then, and RANDOM, whose colour X never turns, takes any since 0.3.19); B takes a token down on another character back up; then it is locked too. 10 s
  at most, after which the player picks by hand. About 1.5 s in Brawl, 2 s in Project+. Then the ready timer, 30 s, once
  both are plugged in (a ranked later game's: once the character order is done, `CharOrder.h`,
  whose own Start is then not pressed): Start readies a player with a character down, B un-readies;
  both ready, Orca presses Start for port 1 every other frame until the screen goes (to the stage
  select: casual's two-turn pick, ranked's strikes). Casual time up: the players who weren't ready are
  named (`orca queue timeout`) and every controller is masked until the pages leave. Ranked time up:
  on with the picks (without a character on both, Phase 1's no-show rule decides). Overlay: each
  player's name over their panel, with the rating in ranked ("ada · 1532"; since `ux=12` a LOCKED
  IN badge on the panel once ready, before it a ✓), "Press Start to lock in · 0:24" / "bo locked in
  · Press Start to lock in · 0:24", in casual "Hold Z to find someone else" and a bar while Z is
  held (never in ranked), "Time's up".
- **After the room** (a skip, a timeout, the opponent gone, a set over, a leave): the session puts
  back this player's own game as it was when `host` or `join` came (the **queue image**: a machine
  image and the NAND, taken at that boundary while the player was on the queue's own character
  select; armed once the room was really entered, its welcome as host or a session in it as
  joiner). Once the room is over for this player and nothing of drop-in is under way, the image is
  loaded as the state of the next frame (as a keyframe is: a new ring, the NAND replaced), the pad
  goes to port 1 and a room of its own opens (a former joiner had none: no more `no-room`). The
  player is ready again, so Orca prints `orca queue ready` again, unless their own ready timer ran
  out (`timeout me`) or a ranked set was played in that room (since 0.3.17: "No search by itself
  after a ranked set" under "Set end on screen"). A `host` or `join` that comes before the restore
  waits for it at the same boundary. Backing out to the menus drops the image.
- **Later games:** casual comes back to the room's character select with the same opponent,
  characters free (no locks), ready again, but with **no timer** (Luis, 2026-10-04: "I kind of like
  that players can message each other and have a little bit more time to be social"): the
  character select waits until both lock in, never times out (no `orca queue timeout`), and a
  long Z still finds someone else. `Queue::Timed` decides, from the match block alone (ranked, or
  no fight since the room's header: `FOUGHT`), so it is the same on both machines and every
  re-run.
- **A picks, B takes it back, Start locks in** (`ux=12`; Luis, 2026-10-04: "press A to select
  character and then press start after to lock in and ready up ... The UI needs to make it clear
  when you're ready/locked in"). Locked in is what ready has always been here, with these buttons:
  - casual's later games, both at once: A on a character puts the token there, B with the token
    down takes it back up (the game's B, which the online rules pass only for that, "B only takes
    the token back up"), Start with the token down locks in. A locked-in player's A, B, X and Y
    never reach the game (no token taken up, no costume turned); their B unlocks. Both locked in:
    Orca's Start at once, to the stage select (no countdown).
  - ranked's later games: the character order (`CharOrder.h`, "Character order") with the same
    buttons, the winner's lock-in final, then the loser's; the moment the loser locks in the queue
    goes on (`Queue::OrderLockedIn`: the order's Done step with both lock-ins, both tokens down),
    with no ready step and no 30 s after it. Before `ux=12` the order's A lock-in was followed by
    the 30 s ready step again.
  - game 1, both games and modes: unchanged (the picks put in and locked, Start locks in, B
    unlocks, the 30 s timer).
- **What the screen says.** Each player's name over their panel (with the rating in ranked) and,
  once they are locked in, a **LOCKED IN** badge on their panel: a pill in the port's colour (red,
  blue) with a white rim and white capitals across the panel's picture (the overlay's
  `SetQueueLines` `locked`, `PortColourOf`; display only, from memory on first runs). The top line
  says who is locked in and what the other does next: "Pick with A, then press Start to lock in",
  "ada locked in · Your pick: Start locks it in", "You're locked in · bo: pick and press Start"
  (with " · 0:24" while timed, "B to change" under it for a locked-in player of a later game), "Both
  locked in", and "Casual · Both locked in · Here we go" as the screen goes. The character order's
  turns keep their own line under the set's score ("Character order").

### Random and ranked's no skip (Orca 0.3.19)

Luis, 2026-10-05: "you should not be able to click Z to escape a ranked match. You are forced to
play a ranked match. Also, for both casual and ranked, I should be able to select random if I want
to."

- **Ranked has no skip** (`Queue::MaySkip`): holding Z to find someone else is casual's only. In a
  ranked room no game of the set has it: no `orca queue skip`, no bar, no "Hold Z to find someone
  else" (the overlay's lines come from `Queue::TextFor`; the rules bar's native line never named Z).
  Z stays kept from the game on both modes' character selects, so nothing emulated changed with
  this. The set over's "Hold Z to leave" (after the verdict) stays, and leaving a set any other way
  is the forfeit it always was.
- **Random is a pick like any character's.** Both games' character select calls it 0x29
  (`Queue::RANDOM_CHARACTER`, Brawl's `Selch_Random`; Project+ reads the same under the harness),
  never "no character" (0x28): the queue identity carries it, `orca queue ready <q> 41 <colour>`
  prints it, the joiner's Random is steered onto its panel, game 1 locks it, the character order
  and casual's later games lock it in with Start, the LOCKED IN badge shows on it. Brawl's RANDOM is
  the right end of the grid's bottom row (x 22.8..28.7, y -4.07..0.08), Project+'s the middle of
  its bottom row (about x -3..4, y -1.5..2.5); with the token in the hand over either, the hand's
  target is the grid (7), as over any character. The game resolves Random itself as the character
  select ends, from its own random number generator, which is emulated state: both machines of a
  session resolve the same character (the synctests and the two-Orca run below), and each game of a
  set draws again (Project+ `pplus-css-random.txt`: game 1 resolved to 50 and 51, game 2 to 0 and
  43). The opponent sees RANDOM on the panel until the fight, as the game shows it.
- **What was in the way** (found under the harness with 0.3.18's code, `q7-runs`, probe watches on
  both areas' +0x1B8, +0x1BC, +0x1F8 and the hands):
  - **Brawl: the A gate's bottom edge.** It was -3.0 in both games, and Brawl's bottom row reaches
    -4.07, so A was swallowed over the bottom quarter of RANDOM (and of every tile in that row),
    which is where a hand coming up from the panels meets it first: A at (25.64, -4.03) over RANDOM
    did nothing, two frames higher (-2.37) the token went down. Brawl's edge is -4.2 now ("The
    character select's buttons (A only in the grid)"), Project+'s stays -3.0.
  - **The joiner's colour.** X never turns RANDOM's colour (+0x1BC stays, both games), and RANDOM's
    colour number isn't always 0: after a fight the tokens come back on RANDOM with the resolved
    character's (8 and 10 in the Project+ run). The steering pressed X until the colour matched,
    which it never did, so the joiner's Random counted as in, and the joiner could ready, only after
    the 3 s costume limit. Random now takes any colour, as port 1's own colour does.
  - **Nothing else**: 0x29 was already a character to every check (`HasPick`, `placed`,
    `CssToken::Down`, `CharOrder::CssPort::Down`). With 0.3.18's code Project+'s RANDOM went down
    with A, Start printed `orca queue ready ranked 41 0`, the joiner's Random was steered in in 65
    frames and both readied (Brawl: 85 frames, from where A still worked).

### Determinism

The region at `MatchBlock::QUEUE` (`+0x1C0..+0x1EF`; the sprint assigned `+0x100..+0x13F`, which is
Project+'s saved stage switch data): +0x00 the ready flags (bit per port), +0x01 go / timed out,
+0x02 who wasn't ready, +0x03 game 1's locked picks, +0x04 the ready timer's start (hook frame + 1),
+0x08 ports 1-2's raw buttons in the frame that just ran and +0x0C the frame before's, +0x10 the
timer, +0x12 the steering's frames, +0x14 port 2's pick (character, costume, x, y), +0x20 the
steering's start, +0x24 the costume's start (3 s to turn it, then the character is in whatever the
colour), +0x28 port 2's hand as the frame that just ran began (the latch's, for "all but stopped").
The +0x01 flags: go, timed out, and (the queue's own character select) the stopping B still held,
a Start held since the way back from a room (`FLAG_SWALLOW_START`), and A on BACK that just stopped
the search (`FLAG_BACK_A`, 0x10: one boundary, after which the gate presses that A for the game). `Queue::Frame` (after the character order) advances it at every boundary, first
runs and re-runs, as a pure and idempotent function of memory, the hook's frame number and the
synced ports (`Queue::Advance`); the gate's masks come from memory alone (`Queue::Gate`). Any new
header (another mode, room or flags) zeroes the region.

**The raw latch** (`InputGate::SetLatch`): the presses the queue reads are ones the gate keeps from
the game (Start, B, Z), so the gate, right after deciding a frame's masks, hands the latch the raw
pads of the frame about to run (`RawInputOverride`: the session's or the harness's), and the latch
writes ports 1-2's buttons into the region (the old ones one slot down). It runs after the
boundary's snapshot or keyframe was saved or loaded, so no snapshot holds its write, and every pass
over the boundary (a first run, a rollback's or sync test's re-run, a joiner's first frame after
its keyframe) writes it again from the state it starts from and the same pads before the frame
runs: the next boundary's frame hook reads the same bytes on every machine. Only while the header is
the queue's; friends games are never written.

**Local only** (first runs, never memory): the stdout lines (the timeout's at a final frame, through
`ConfirmResults`), the Z hold (this player's own raw buttons, never a guess), the overlay.

**Found under the harness** (`ORCA_UX_PROBE` watches, both games alike): the player area's +0x1BC
is the costume (X one up, Y one down); the hand (OnlineRules.h) starts on its panel's player-type
button (Brawl x -22.5 + 15 a port, y -19.8; Project+ -23.4, -19.3), up is +y; Brawl's hand moves
0.83 a frame at full tilt, Project+'s 1.0; the token is down once +0x1F8 and +0x1F9 are both 0.

**Test knobs** (test overrides, both processes alike): `ORCA_TEST_QUEUE=casual|ranked[:<coin>][:q2]`
(the room's header with `FLAG_QUEUE2`) or `...:solo` (the queue's own character select);
`ORCA_TEST_QUEUE_PICK=<frame>:<char>:<costume>:<x>:<y>[:<rating>]` (harness runs: port 2 plugs in at
that frame with that queue identity, as a matched joiner's does). With `...:solo` and
`ORCA_TEST_QUEUE_PICK` both, port 2's plug-in is the match the queue found: from that boundary the
header is the room's (`FLAG_QUEUE2`, no longer solo), as a host's is once the page hosts, so one run
goes from the player's own character select to the room's. The knob's header is a pure function of
the knob and the synced ports, so it is written on every run of a boundary, re-runs too (a real
host's header changes only at an alone boundary, which nothing re-runs). `Tools/orca/queue-e2e.mjs
--q2 [--scenario skip|timeout|noskip] [--pick random]` plays the page's half against yougame.co
(`noskip`, ranked: both hold Z 3 s on every game's character select and nobody skips; `--pick
random`: every pick RANDOM). Inputs:
`Tools/orca/inputs/bf-queue2.txt`, `pplus-queue2.txt`; the whole flow from the boot,
`bf-queue2-flow.txt` and `pplus-queue2-flow.txt` (casual, two games), `bf-queue2-ranked.txt` and
`pplus-queue2-ranked.txt` (ranked game 1: ready, strikes, pick); the character select's buttons
(`ux=12`), `bf-css-lockin.txt` and `pplus-css-lockin.txt` (ranked through game 2's character order
and stage select) and `bf-css-casual.txt` and `pplus-css-casual.txt` (casual through game 2's
lock-ins and stage pick); Random (`ux=15`), `bf-css-random.txt` and `pplus-css-random.txt` (ranked
with every pick RANDOM: the own character select, the joiner's steered in, game 2's character order,
the two cursors' stage selects).

**Checked 2026-10-04** (branch `q2-queue-css` on `rel-0.3.13`, M4 Pro, headless, `-v Null`):
- Unit tests: `OrcaQueue*` (ready, unready, the stopping B, the timer, casual and ranked timeouts,
  game 1's locks, the steering and its limits, the costume, the skip's hold, the latch, the
  identity, the region) and the rest: 313 `Orca*` tests pass.
- Brawl, `YG_SYNCTEST=7 YG_SYNCTEST_FROM=1700` with `ORCA_TEST_QUEUE=casual:q2
  ORCA_UX_TEST_NAMES=ada,bo ORCA_TEST_QUEUE_PICK=1817:3:2:-7.5:14.23` over `bf-queue2.txt` to frame
  4400 (port 2's pick steered in, Link in costume 2, 94 frames with the 20-frame settle; both ready;
  Orca's Start; the fight; results; game 2's character select, both ready, Orca's Start): 18,851
  re-run frames, **0 RAM mismatches**.
- Project+ under the harness, `pplus-queue2.txt`: port 2's pick in 58 frames, both ready, two games.
- The queue's own character select under the harness (`casual:solo`): Start readies, B un-readies
  with the token still down, Start again, B, then held B goes back to the menus.
- `queue-e2e.mjs --q2` against yougame.co with dev tickets, Brawl casual PASS on the final build
  (the joiner's pick in 1.5 s, verdicts the same, both back on their own character select with
  `orca queue ready` again), and on the builds just before the last steering change (the settle and
  "all but stopped"; the pick then took 3 s): casual 2 games PASS (back in 53 and 123 ms), `--scenario
  skip` PASS, `--scenario timeout` PASS (both `timeout me`, back, not ready), ranked with 1-stock
  games PASS (2-0 set, game 2's character order then the ready timer; back after the set in 40 ms),
  Project+ casual PASS (pick in 2 s, back in 30 ms).
- The queue image: its capture takes 20-30 ms (a machine image and the session NAND, 37.8 MB in
  Brawl), the way back 30-140 ms.

**Checked 2026-10-04, Orca 0.3.14** (`rel-0.3.14`: `q2-boot-menu`, `q2-css-title`, `q2-stages` and
`q2-queue-css` merged on `rel-0.3.13`, `ux=10`; M4 Pro, headless, `-v Null`):
- Unit tests: 334 `Orca*` pass (16 skipped: the live and network ones).
- Brawl, the whole casual flow from the boot (`bf-queue2-flow.txt`, `ORCA_TEST_QUEUE=casual:solo
  ORCA_UX_TEST_NAMES=ada,bo ORCA_TEST_QUEUE_PICK=2057:3:2:-7.5:14.23 YG_RESULTS=1`), `YG_SYNCTEST=7
  YG_SYNCTEST_FROM=1440` to frame 4700: the main menu at 1430, With Anyone > Casual (exit code 30)
  to the character select at 1757 (title CASUAL), port 1 ready at 1988 (`orca queue ready casual 0
  0`), the room's header at 2057, Link's pick in at 2151 (94 frames), both ready at 2378, the casual
  stage pick (port 1 Battlefield, port 2 another; the coin took Battlefield), the fight (port 1 won,
  3-0), results, game 2's character select at 4420 (CASUAL), both ready at 4561: 22,771 re-run
  frames, **0 RAM mismatches**.
- Brawl, ranked game 1 (`bf-queue2-ranked.txt`, `ranked:solo`, the pick at 2106 with rating 1532),
  `YG_SYNCTEST=7 YG_SYNCTEST_FROM=1440` to 2900: Ranked (exit code 31, title RANKED), ready, the
  room's header, the pick in, both ready, strikes by X (port 1 one, port 2 two), port 1's pick with
  A, the fight from 2616: 10,171 re-run frames, **0 RAM mismatches**.
- Project+, the whole casual flow (`pplus-queue2-flow.txt`, the pick at 824:43:2:-7.4:7.7),
  `YG_SYNCTEST=7 YG_SYNCTEST_FROM=200` to 3200: the main menu at 197, Casual's character select at 524
  (title CASUAL), ready at 725, the room's header at 824, the pick in at 902 (78 frames), both ready
  at 1145, the casual stage pick (both named, the coin took port 1's), the fight (4-0), results, game
  2's character select, both ready at 3118: 20,951 re-run frames, **0 RAM mismatches**.
- Project+, ranked game 1 (`pplus-queue2-ranked.txt`, the pick at 874), `YG_SYNCTEST=7
  YG_SYNCTEST_FROM=200` to 4300: ready, the pick in, port 1's strike by X, port 2's two by its timer,
  port 1's pick by its timer, the fight from 4031: 28,651 re-run frames, **0 RAM mismatches**.
- Plain harness runs of the Project+ inputs after the boot change: `pplus-css-title.txt` (FRIENDS,
  CASUAL, RANKED, then VERSUS untouched), `pplus-casual-pick.txt` (prefs 0/6, the coin took 0) and
  `pplus-queue2.txt` (the pick in, both ready, both turns run out, the coin's stage).
- `queue-e2e.mjs --q2` against yougame.co with dev tickets, Brawl: casual PASS (2 games, the pick in,
  verdicts the same, both back on their own character select in 33 and 51 ms, `orca queue ready`
  again); ranked with `ORCA_TEST_SET_RULES=1,8` PASS (3 verdicts the same, back in 35 and 46 ms).
- After the last merges (`orca-chat-toast`, `kLaterGameOrder` WinnerFirst, casual's later games
  with no ready timer, the toasts and chat under Dolphin's OSD rows): 355 `Orca*` tests, 339 pass
  (16 skipped); the Brawl and Project+ whole casual flows above re-run with the same env, the same
  steps (game 2 both ready at 4561 and 3118, no timeout), 22,771 and 20,951 re-run frames, **0 RAM
  mismatches**. An `ORCA_TEST_PRESENT=1280x720 YG_THROTTLE=1` screenshot with `chat` lines and
  "Screenshot saved" rows showing: the chat starts under the OSD rows (the frame meter, top left,
  still shares the first OSD row's line, as before).

**Checked 2026-10-04, Orca 0.3.15** (`rel-0.3.15`: `q3-steer-int`, `q3-header-flags` and
`q3-imgui-font` merged `--no-ff` on `efa260448b`, no conflicts; M4 Pro, headless, `-v Null`, the
release checklist above):
- Build: `dolphin-emu-nogui` and `tests`, no compiler warnings (only the usual Homebrew dylib
  deployment-target notes from `ld`).
- Unit tests: 372 `Orca*`, 355 pass, 17 skipped (the live, network and disc ones; one more than
  0.3.14's 16, the new `OrcaLive.AHostsMismatchReachesItsJoiner`), 0 fail. With `ORCA_LIVE_TEST=1`
  against yougame.co, 12 `OrcaLive.*` pass (`BridgeMirrorsTheRoomAndTakesLeave` skips without its
  bridge).
- Hashlogs against live 0.3.14's: Brawl `bf-mario-link-results.txt` to 10,800, 10,815 frames
  compared, **0 differ** (`notes/r314/mac-brawl.hash`); Project+ `pplus-1v1.txt` to 9,000, 9,011
  frames compared, **0 differ** (`mac-pplus.hash`).
- Synctests, the 0.3.14 env: Brawl's whole casual flow (`YG_SYNCTEST_FROM=1440` to 4700) 22,771
  re-run frames, **0 RAM mismatches**; Project+'s (`YG_SYNCTEST_FROM=200` to 3200) 20,951, **0**. The
  steps are 0.3.14's frame for frame: the room's header at 2057 (104 bytes) and 824 (93 bytes), the
  pick in at 2151 (94 frames) and 902 (78), both ready 2378 and 1145, game 2 both ready 4561 and
  3118, and every "steering port 2" line the same as 0.3.14's.
- Two-Orca checks, all PASS, every session's checksums matched, no desync:

  | | Project+ | Brawl |
  |---|---|---|
  | `--q2` casual, 2 games | verdicts 4-0, 4-0 the same on both; pick in 77 frames; back in 23 ms; **99 / 99** checksums (0 / 0 rollbacks) | verdicts 3-0, 3-0; pick in 85 frames; back in 31 ms; **103 / 103** (1 / 0) |
  | `--q2 --scenario rematch` | bo skipped (Z 90 frames) the room after the pick: **4 / 4**; bo then hosted the new room from port 1 (pick in 73 frames), 1 game the same on both: **52 / 52** | skip: **4 / 4**; bo hosted the rematch (85 frames), 1 game: **50 / 50** |
  | `online-menu-dropin.py` | keyframe of frame 1003, both to With Friends' character select at 1095; **13 / 12** | keyframe of 2287, both at 2383; **12 / 12** |

  (host / joiner; the counts are each side's compared pairs, so they can differ by one.) The
  Mac <-> PC run (`xplat.py`, checklist item 6) is the release lead's, with the PC.

**Checked 2026-10-04, the character select's buttons** (branch `q4-css-lockin` on `rel-0.3.15` at
`20032d9f0b`, `ux=12`; M4 Pro, headless, `-v Null` unless said):
- **Luis's "stuck", reproduced** with the build before (`rel-0.3.15` at `ef24886f39`, the same
  character order as 0.3.14) over `pplus-css-lockin.txt` (`ORCA_TEST_QUEUE=ranked:solo
  ORCA_UX_TEST_NAMES=ada,bo YG_RESULTS=1 ORCA_TEST_QUEUE_PICK=874:43:2:-7.4:7.7:1532`, `ORCA_UX_PROBE`
  watches on both areas' +0x1B8 and +0x1F8): game 2's order begins at 5905 with port 1's token still
  on last game's character (0x08); port 1 moves onto another character and presses A, and that A
  is its lock-in at 5994, 88 frames into its turn, of the old 0x08; its B, A, held B and Start after
  it change nothing (its character and token never move again). Port 2's B is masked; its A over its
  own token takes the token up (6404), after which nothing it does is a lock-in (its Start isn't the
  game's, B is masked) until its 45 s run out at 8694 and Orca puts the token down where the hand
  is (8715); then the 30 s ready step on top (the run ends at 9000, inside it). Brawl
  (`bf-css-lockin.txt`, `ORCA_TEST_SET_RULES=1,8`, the pick at 2106): port 1's A on another character locks Mario (0x00) in at 3929, 70 frames in; port
  2's A with its token down locks its old character in at 4349; the stage select only at 6153, after
  the ready step's 30 s ran out with port 1 never ready.
- The same inputs with this build: Project+ game 2's order at 5905, port 1's A on the new
  character does nothing, its B takes the token up (6014), A puts it on Mewtwo (0x2E, down at 6053),
  B held 60 frames takes it up once and never backs out, A, then Start: locked in at 6163 (step
  second); its B and A after it do nothing; port 2's B takes its token up (6244) and stays held 140
  frames with it in the hand (the character select stays), A, Start: the order is done at 6423 and
  the queue goes on at the same frame, the stage select at 6427 (4 frames, no ready step). Brawl:
  port 1 up at 3949, Link (0x02) down at 3988, held B 60 frames, locked in at 4098; port 2 up at
  4188, held 140 frames, down on 0x17, locked in at 4368 and on at once, the stage select at 4372.
  Casual's game 2 (`pplus-css-casual.txt`, `bf-css-casual.txt`, the 0.3.14 checks' env): port 1's B,
  A on another character, Start (locked in: Project+ 3098, Brawl 4551), A on its own token does
  nothing, B unlocks (3138, 4591) with the token still down, Start again (3158, 4611); port 2's B,
  A, Start: both locked in and the stage select at once (3238 to 3242, 4691 to 4695).
- `YG_SYNCTEST=7`, **0 RAM mismatches** in all four: Project+ ranked (`pplus-css-lockin.txt`,
  `YG_SYNCTEST_FROM=200` to 8600) 58,751 re-run frames; Brawl ranked (`bf-css-lockin.txt`, 1440 to
  5300) 26,971; Project+ casual (`pplus-css-casual.txt`, 200 to 4200) 27,951; Brawl casual
  (`bf-css-casual.txt`, 1440 to 5600) 29,071; the steps at the plain runs' frames.
- Screenshots (`-v Metal`, `ORCA_TEST_PRESENT=1280x720`, `YG_SHOT_EVERY`; looked at, then deleted):
  Project+ ranked game 1 with ada ready: a red LOCKED IN pill across ada's panel, "You're locked in ·
  bo: press Start · 0:25"; game 2's winner's turn: "Your pick · Start locks it in · B to change ·
  0:45" under the set's score; the loser's turn: LOCKED IN on ada's panel, "You're locked in · bo
  picks · 0:42"; Brawl the same two turns; Project+ casual game 2 with both locked in: a red and a
  blue pill, "Casual · Both locked in · Here we go". The players' names and the badges now sit on
  the panels (before, the names were at 1/8 and 3/8 of the width, left of both games' widescreen
  panels).
- Unit tests: 380 `Orca*`, 362 pass, 18 skipped, 0 fail (new:
  `OrcaCharOrder.APicksBTakesTheTokenUpAndNeitherLocksIn`,
  `.BReachesTheGameOnlyAsAPressWithTheTokenDown`, `.LockedPortsFollowTheTurns`,
  `.WinnerLocksInWithStartThenTheLoser`; `OrcaOnlineRules.BReachesTheCharacterSelectOnlyToTakeTheTokenUp`;
  `OrcaQueue.RankedLaterGamesGoOnTheMomentTheOrderHasBothLockIns`,
  `.ALockedInPlayerKeepsTheirPickAndBUnlocks`).
- Not run here: the two-Orca and Mac <-> PC checks (the release's), and the hashlogs (no header
  in their inputs, so nothing here acts on them).
- Match block: no new bytes. The order keeps its 16 bytes at +0x200 (its lock-ins are its steps),
  the queue its region at +0x1C0 (ready is the lock-in); +0x1A8..+0x1B3, offered for this track, is
  still free. What changed is who writes the raw latch (+0x1C8..+0x1CF, +0x1E8..+0x1EF): every
  header that locks, not only queue2's.

**Checked 2026-10-05, Orca 0.3.19** (`q7-ranked-random` on `rel-0.3.18` at `17007aa28e`, `ux=15`;
M4 Pro, headless, `-v Null` unless said; logs in `q7-runs` beside the checkouts):
- Build: `dolphin-emu-nogui` and `tests`, no compiler warnings (only the usual Homebrew dylib
  deployment-target notes from `ld`).
- Unit tests: 439 `Orca*`, 419 pass, 20 skipped (the live, network, disc and design-dump ones), 0
  fail; new `OrcaQueue.RankedHasNoSkip` (casual's hint, bar and skip; ranked's none in game 1 and
  later games, either port, ready or not, Z still kept from the game), `.RandomIsAPickLikeAnyCharacter`
  (the identity, the own character select's ready, the joiner's Random steered in and locked in any
  colour with no X, both locked in, casual's later games, ranked's order), `OrcaCharOrder
  .RandomLocksInLikeAnyCharacter`, and `OrcaOnlineRules.TheCharacterSelectPassesAOnlyInTheGrid`'s
  Brawl (-4.2) and Project+ (-3.0) bottom edges.
- Under the harness with 0.3.18's code (`ORCA_UX_PROBE` watches, `ORCA_TEST_QUEUE=ranked:solo`, the
  joiner's pick `ORCA_TEST_QUEUE_PICK=<frame>:41:0:<x>:<y>:1532`): Project+'s RANDOM down with A at
  (0.60, 0.70), `orca queue ready ranked 41 0`, port 2's Random in 65 frames, both ready, the
  stage select, the fight (`pp-probe`); Brawl the same from (25.64, -0.71), port 2's in 85 frames
  (`bf-probe`); Brawl's A at (25.64, -4.03) over RANDOM changed nothing and two frames higher
  (-2.37) put the token down (`bf-low`); X on RANDOM never moved +0x1BC in either game.
- `YG_SYNCTEST=7`, **0 RAM mismatches**, every pick RANDOM:
  - Project+ `pplus-css-random.txt` (`ORCA_TEST_QUEUE=ranked:solo ORCA_UX_TEST_NAMES=ada,bo
    YG_RESULTS=1 ORCA_TEST_QUEUE_PICK=874:41:3:0.5:0.5:1532`, `YG_SYNCTEST_FROM=200` to 4900):
    32,851 re-run frames. Port 1's RANDOM down at 745, `orca queue ready ranked 41 0` at 775; port
    2's Random (colour 3 in its identity) in at 939, 65 frames, no X; both ready at 1195; the two
    cursors agreed on Battlefield; game 1 fought as 50 and 51 (port 1 won 4-0); game 2's order:
    port 1's B, RANDOM again with A, Start at 3266, port 2's Start on the RANDOM left from game 1
    at 3316 and the stage select at once; game 2 fought as 0 and 43.
  - Brawl `bf-css-random.txt` (the same with `ORCA_TEST_SET_RULES=1,8` and the pick
    `2106:41:3:25.7:-1.5:1532`, `YG_SYNCTEST_FROM=1440` to 5000): 24,871 re-run frames. Port 1's A
    at (25.64, -3.20) over RANDOM, under 0.3.18's edge, put the token down at 2032; `orca queue
    ready ranked 41 0`; port 2's Random in 89 frames; both ready at 2427; game 1 fought as 37 and
    26 on Pokemon Stadium; game 2's order on RANDOM (4022, 4072), fought as 25 and 1 on
    Battlefield.
  The plain runs of both inputs (to 9000) took the same steps at the same frames.
- Two-Orca, Project+ ranked over yougame.co with dev tickets (`queue-e2e.mjs --pplus <dol>
  --queue ranked --q2 --scenario noskip --pick random`): **PASS**. Both picked RANDOM on their own
  character select (`orca queue ready ranked 41 0` on both), bo's Random went into ada's game in 65
  frames (1.3 s); on each game's character select both held Z for 3 s and neither printed `orca
  queue skip`; 2-0 for ada, the 3 verdicts the same on both sides, the characters too (game 1 38 and
  35, game 2 3 and 38: each machine resolved the same Randoms); VICTORY and DEFEAT, the set over,
  both back on their own character select not ready; **235 / 235** checksums matched, 0
  rollbacks, no desync.
- Screenshots (`-v Metal ORCA_TEST_PRESENT=1280x720`, Project+ `pplus-css-random.txt`), kept in
  `ui-shots/0.3.19` beside the checkouts with a README: the queue's own character select searching
  with RANDOM down; the ranked room's character select, game 1, with no "Hold Z" hint or bar; ada
  LOCKED IN on RANDOM; game 2's character order with ada's Random locked in.
- Not run here: the Brawl two-Orca run, the casual two-Orca runs, the hashlogs (nothing here acts
  without a queue header) and Mac <-> PC (the release lead's).

## Embedding

The YouGame desktop app shows Orca inside its own window, at the page's player box, the way it runs
libretro cores (`desktop/native/libretro/host.cpp` in YouGame's repository). The protocol is the
libretro host's, line for line where both make sense. Code: `Source/Core/DolphinNoGUI/Embed.*`,
`Platform.cpp` (commands), `PlatformWin32.cpp` and `PlatformMacos.mm` (the window). Test host for
the Mac: `Tools/orca/embed-test-host.m` (instructions at its top).

### Arguments

```
Orca --embed --parent <handle> --rect X Y W H [-u <user>] -e <disc> [-C ...]
```

- `--rect X,Y,W,H` (one argument) works too. A W or H below 32 (`Embed::MIN_SIDE`) opens the view hidden, at 32, until a big enough `rect`.
- **`--parent` on Windows:** the HWND as a decimal number. That is what the app already passes for
  libretro (`getNativeWindowHandle()` read as a 64-bit integer).
- **`--parent` on macOS:** the window's **CGWindowID** (its window number), as a decimal number.
  `getNativeWindowHandle()` there is an `NSView*` in the app's own address space and means nothing
  to another process. In Electron's main process, the number is the middle field of
  `win.getMediaSourceId()`: `"window:<id>:0"`.
- Pass the disc with `-e`. Orca.app with no disc falls back to its disc picker, which brings it to
  the front as a regular app for that dialog.
- Orca never asks with dialogs while embedded: alerts go to stderr.
- stdin must stay open. A closed stdin means the app is gone, and Orca quits.
- Orca.app runs a session unless `ORCA_SESSION=0` is in its environment. In a session `reset` is
  refused, and `pause` works only while the player's game is alone (below).
- When the app's window goes away without a `quit`, Orca stops too. On Windows that happens when its
  child window is destroyed; on macOS, 2 s after the window leaves the window list. A minimized
  window doesn't count as gone.

### Coordinates

The numbers are the ones `EmbeddedEmulator.measure()` already sends: the box's CSS pixels times
`devicePixelRatio`, so physical pixels.

| OS | `rect` is relative to | Units | Who follows window moves |
|---|---|---|---|
| Windows | the parent's **client area** (top left) | physical pixels. In embed mode Orca is per-monitor-v2 DPI aware, like Electron. | Windows: Orca's window is a `WS_CHILD` of the HWND, so it moves and clips with it. |
| macOS | the parent window's **frame** (top left, title bar included) | backing pixels of the screen the window is on (points × `backingScaleFactor`) | Orca: it polls the window's bounds at 120 Hz. |

On macOS the app adds the title bar to Y before forwarding a page's `rect`:
`y + (win.getContentBounds().y - win.getBounds().y) * screen.getDisplayMatching(win.getBounds()).scaleFactor`.
That is 0 in full screen. On both systems, the app sends `rect` only when the box moves inside the
window (layout, scroll, resize, full screen). Moving, minimizing or hiding the window needs nothing.

### stdin: one command per line

Two boxes: `rect` is Orca's window (where it shows anything at all), `view` the box the game's
picture is laid out in (the window itself unless the app says otherwise).

| Command | Effect |
|---|---|
| `rect X Y W H` | Moves and resizes Orca's window (units above). A rect while hidden doesn't show it. A rect with W or H under 32 hides the window instead, at its last size, until a rect at least 32 by 32 arrives (then it shows again unless the app said `hide`): Orca 0.3.13 and earlier shrank the window to it instead, and under about 17 pixels tall the overlay's text rounded to 0 px and ImGui aborted Orca (`font_size > 0.0f`). The overlay also draws nothing in a picture under 100 pixels tall (`Orca::UX::OverlayUnit`), and every size it hands ImGui is at least 1 px (`TextPx`). An ImGui assert no longer ends a release build at all (`Externals/imgui/imgui_user_config.h`): it is written once per place to stderr (the game's log) and ImGui carries on; a debug build still stops on one. |
| `view X Y W H` / `view off` | Where the game's picture is laid out, in `rect`'s units and origin (`X,Y,W,H` works too); `rect` stays the window. The YouGame page opens its play menu's panel over the left of the player box, and nothing in the page can draw over Orca's window, so it cuts the window (`rect`) to start right of the panel and keeps `view` the whole box: the picture is fitted and centred in the view as it would be in a window that size, so the game neither shrinks nor moves when the menu opens, and the panel covers its left part (the window shows the rest; Dolphin crops what runs past it). The internal resolution follows the view too (Auto's integral scale), so opening the menu never re-creates the EFB. `view off` (and until the first `view`): the picture is laid out in the window, as before. Orca keeps the last `view` across later `rect`s, so the page must send `view` before every `rect` once it has sent one (the toast's cut from above included), and the app must treat `view` as it treats `rect`: let it through its filter, add the title bar to its Y on macOS, and resend it before the `rect` when it refits the window (full screen). The presenter keeps the layout it last drew with until the window system has resized the surface to the new `rect` (`VideoCommon/PresentLayout.h`, `ChooseLayout`), so the picture doesn't jump for a frame. Orca's own overlay elements (the corner's circle, ping and frame meter, the toasts and the chat, the top centre's banners, "Waiting for", the set's end, the control guide) stay in the part of the picture inside the window (`Orca::UX::VisiblePicture`), at the same size; what marks the game's own art (name tabs, portrait frames, the stage select's cursors, the set's dots, the timers in the game's lettering) keeps the whole picture. A view with W or H under 32, or a malformed line, answers `unsupported view`. A window that shows none of the picture (cut to the view's pillarbox bar) shows black: no picture is drawn (`PictureInBackbuffer`). `Tools/orca/view-test.py` takes headless screenshots of each step (the panel's cut, the dim, shut, `view off`, a toast's cut from above, `focus`); `Tools/orca/embed-test-host.m`'s `panel N` sends the page's `view` and cut `rect` in a real window, and `occluded-test.py --overlay` opens the panel that way, with `dim 50`, beside a friend. Host-only drawing: screenshots and frame dumps are the whole picture as before, and the compatibility key is unchanged. **No cap:** YouGame forces the latest Orca and desktop app (Luis, 2026-10-04), and an older Orca logs the line as unknown and ignores it. |
| `dim N` | N from 0 to 100: N percent black over Orca's whole frame, drawn last (over the picture, Dolphin's messages and the overlay), on every present including a stall's redraws, easing to the new level (half black in 0.2 s, `Orca::UX::DIM_EASE_S`). The page sends `dim 50` while its menu is open over the game (as it dims a web game behind its panel, `rgba(0,0,0,.5)`), and `dim 0` when it closes, on `ready` and when it covers the game. `focus` clears it too (the game has the input again only with the menu shut), so a missed `dim 0` can't leave the game dark. Anything else answers `unsupported dim`. Host-only drawing: never in screenshots or frame dumps of the game (except `ORCA_TEST_PRESENT`'s test shots, which take the presented image, overlay and all), no compatibility key change. **No cap**, as `view`. |
| `hide` / `show` | Takes Orca's window off screen and puts it back. Hidden also means no input. |
| `focus` / `blur` | The game gets the input, or gives it back to the page. See "Focus" below. `blur` is new: the libretro host has no `blur`. `focus` also clears `dim`. |
| `pause` / `resume` | Answers `state paused` or `state running`. A `pause` before the game runs is held until it does, so `ready` waits for `resume`. **In a session** (Orca with `orca caps ... pause`) `pause` pauses only while the player's game is alone in it: no friend plugged in or on the way, not joining or catching up, no keyframe wanted or being made, and no `prepare-join`, `join` or `leave` waiting for a frame (`Rollback::OnlineMatch::SoloPauseAllowed`). Otherwise, and before the game runs, it answers `unsupported pause` then `state running`: the game keeps running (the friend's does), and nothing waits. A solo pause ends by itself, with `state running`, as soon as drop-in needs the game: a friend arriving in the room, `prepare-join`, `join` or `leave`. While paused, the room's connection stays up (its thread pings the server every second whatever the game does), but a room that drops meanwhile is reopened only after the resume (that happens at frame boundaries), so a pause longer than the server's 90 s hold on a dropped player's seat comes back to a new code. The pause stops the emulated machine between two instructions and changes nothing it computes; the `orca stats` line leaves the paused time out of its `fps`. An Orca without `pause` in its caps answers `unsupported pause` to every `pause` in a session. |
| `volume V` | V from 0 to 1, linear, as the libretro gain. For this run only: Dolphin.ini is untouched. It is allowed in a session and doesn't touch the compatibility key. |
| `perf off` / `perf fps` / `perf detailed` | With `orca caps ... perf`: the overlay's frame meter at the top left, solo or with friends (under the ping when a friend is in). `fps` (the default until the app says) shows `60 fps · 17 ms`: the frames shown in the last second and the longest of them, host wall time between the boundaries of shown frames (re-runs are not shown frames: their time counts in the next shown frame's; a join's catch-up starts the meter over). It turns yellow under 59 fps or past 20 ms, red under 50 fps or past two frame periods (a hitch), and keeps the worst colour of the last 2 s. `detailed` adds a graph of the last 2 s of frame times and, with a friend, the delay and rollbacks per second. A pause freezes it. Host-only drawing: no compatibility key change. Anything else answers `unsupported perf`. |
| `save N` / `load N` | Answers `unsupported save N` / `unsupported load N`. A session refuses save states, and Orca keeps the game's own saves in its NAND. |
| `reset` | The console's reset button. Answers `unsupported reset` in a session or before the game runs. |
| `prepare-join` | The host invited a friend to drop in. It calls `Orca::Online::PrepareJoin()`: the next frame boundary captures a keyframe ("Drop-in"). |
| `join <code>` / `leave` | With `caps join` / `caps leave`: into a friend's game, or out of one ("Drop-in"). Both are also taken while a launch join waits for the keyframe. |
| `host <code>` / `queue-cancel` | With `caps host` (Orca 0.4.0 on): host the room the queue matched this game into, or the search ended ("Matchmaking"). `unsupported host` when refused. With `caps queue2`, `queue-cancel` also un-readies the player on the queue's own character select (`orca queue unready` follows). |
| `queue rating <n\|->` | With `caps queue2`: this player's rating in the current queue (ranked; 0-65534) or none (`-`), sent before `host` or `join`. It rides the player's queue identity to the other Orca, which shows it by the player's panel ("The queue's character select"). Sent again after a ranked set, once the site rated it: the VICTORY / DEFEAT panel counts to the new number ("Set end on screen"). Anything else answers `unsupported queue`. |
| `direct on` / `direct off` | With `caps direct` (Orca 0.3.9 on): the player's switch for direct links to the other players ("Direct links"). On until the app says; the choice stays for the run and applies while the `caps` line has `direct`. `off` drops every link at the room's next tick and sends every input through the relay (as YouGame's ticket switch does); `on` builds them anew. The `orca stats` line's `tx` shows the path in use. Anything else answers `unsupported direct`. |
| `delay auto` / `delay N` | With `caps delay` (Orca 0.3.5 on): this player's input delay. `N` from 1 to 6 frames is exactly that, never adapted; `auto` is the adaptive delay ("Input delay" below). The choice stays for the run, so it may come before a session and holds for every session after; until the app's `caps` line turns `delay` on, it waits. Mid-match it applies from this player's next frame and the friend needs nothing: each player's delay is its own, and every input travels explicitly. Anything else answers `unsupported delay`. The `orca stats` line's `delay` is the delay in use. |
| `chat <name> <text>` | With `caps chat`: a line someone else wrote in the YouGame page's chat, which the page sends only while Orca fills the screen (its own chat can't be seen then). Each field is `encodeURIComponent` of UTF-8, so only `A-Z a-z 0-9 - _ . ! ~ * ' ( )` and `%XX`: the name at most 24 code points (288 encoded), the text at most 140 (1680), the page cutting longer ones with "…". Orca shows `<name>: <text>` at the top left, under the toasts and clear of the top-centre lines and of Dolphin's own messages (OSD rows such as "Screenshot saved" push the toasts and the chat down, `OSD::MessagesBottom`), the name in the circle's blue, wrapped to 45% of the width, for 8 s, the newest three (`UX/Chat.h`, `UX/Overlay.h`). Host-only drawing: never game memory, no compatibility key change, and the text is never printed to stdout or stderr or logged. A malformed line (a bad escape, bad UTF-8, a control character or line break, a field too long or empty, a third field) answers `unsupported chat` and nothing more. Glyphs the OSD font lacks (emoji) show as `?`. |
| `orb <x> <y> <size> <lit> <away> <badge>`, `orb blink`, `orb off` | With `caps yougame`: the YouGame button, drawn where the page's own stands under Orca's window (the page's overlay button at the game's top left, YouGame's docs/PLAY_OVERLAY.md). A red disc with YouGame's controller mark `size` pixels across (8 to 1024), its top left at (`x`, `y`) in the view's pixels (0 to 32767); `lit` 1 at full strength, 0 at half (the page's button at rest); `away` 1 with an amber dot at the bottom right (a friend's held seat); `badge` 0 to 99 unread, a white badge at the top right ("9+" past 9). `orb blink`: a white ring grows out of it three times, 0.7 s each (something from outside the lobby). `orb off`: not drawn. While it is on, the top left is the YouGame layer's, as on the page (Luis, 2026-10-05: "stay up top nice and neat"): the page's chat lines, Orca's own toasts and the `notice` lines show in the button's pill, a slab that unrolls from under it along its row (the page's Pill: 40 px high at least, its words 54 px in, rows of a 26 px avatar and the words, a chat line's name over up to two lines of its words, cut with "…"; at most 480 px wide and three rows, in the order they came, the newest action's key and verb in grey at its end), in YouGame's slate and Roboto; and Orca's corner (the connection circle, the ping, the frame meter) hugs the picture's top right instead, clear of the match clock, with Project+'s queue bar ending short of it. With the button off, all as before. On macOS a click on it falls through Orca's window to the page's own button under it; on Windows a left press inside the disc prints `orca orb` instead of `orca click`, and the page opens its overlay. Shift+Tab while Orca's window has the keyboard (Windows: the page sees no keys then) prints `orca orb` too, as Esc and F11 print their `key` lines. **On a controller** (a GameCube pad has no Home button), in every scene: hold the D-pad's Up and press Start, and Orca prints `orca orb` too (`UX/OrbCombo.h`). From that press of Start until Start and the D-pad are both let go, the game gets neither: no pause, no second taunt; the Up before Start reaches it (a taunt in a fight). It reads the local player's pad as the session will send it (Rollback.cpp's local read: the app's controller, Dolphin's own pad or a test script), so the keyboard and every controller work the same; the masked pad is what is recorded and sent, so every machine plays it and re-runs replay it. Unfocused (the overlay open), the pad is neutral and the shortcut keeps its state, so Start still held on return does not reopen it. Opening the overlay never pauses Orca, online or not: the page sends `orb off` and `blur` (never `pause`), so this player's pad goes neutral in the session's own read and that is what is sent, while emulation, sound and the picture go on; `Tools/orca/occluded-test.py <host> <bin> <disc> --friend --overlay` opens and shuts it mid-fight beside a friend and fails on any wait or extra hitch on either side. Without an online match the harness applies it to port 1's script, kept per frame for re-runs (`Tools/orca/inputs/bf-orb-combo.txt`, whose hashlog equals `bf-orb-combo-plain.txt`'s). A malformed line answers `unsupported orb` (`UX/YgOrb.h`). Host-only drawing. |
| `notice <kind> <name> <text> [<key> <verb>]` | With `caps yougame`: one of the YouGame overlay's lines, a row of the button's pill (`UX/YgOrbDraw.h` DrawPill, in YouGame's look): the person's initial on their site colour with a mark for `kind` (`join` a green plus, `leave` greyed, `away` an amber ring, `back` a green dot, `voice` and `call` a green ring, `invite` none; a `dm` is the person's own words, drawn as a chat line: the name over up to two lines of them), "`<name>` `<text>`" (the name brighter), and, when `key` and `verb` are there, the action's key in a keycap and its verb in grey ("Shift+Tab Join"). Fields encoded as `chat`'s; `key` and `verb` at most 24 code points each. It stays as long as the page's own line would (voice and call 8 s, join 3.5 s, away 5 s, leave and back 2.5 s, dm and invite 4 s plus 60 ms a character up to 9 s), fading over its last 0.4 s; the newest three rows of the pill, with the chat and Orca's toasts, in the order they came. Never printed or logged. A malformed line answers `unsupported notice`. Host-only drawing. |
| `quit` | Stops at once (no power-button shutdown, also outside a session), then exits. |

Any other line goes to stderr. The app's `EMBED_COMMAND` filter must add `blur` and `prepare-join`,
and its Orca filter (`ORCA_COMMAND`) `view` and `dim`.

### stdout: one event per line

| Line | Meaning |
|---|---|
| `ready W H FPS` | The first frame is on screen. W and H are the emulated frame buffer (Brawl: `640 480`); FPS is the VI rate (`59.9401`). The Mac takes about 1-2 s. A session can show `orca state lobby` **after** `ready` and wait there for its friend: after `ready`, no startup timeout applies. |
| `state running` / `state paused` | The answer to `pause` and `resume`; also `state running` when a session's solo pause ends for drop-in. |
| `unsupported <command> [N]` | The command was refused. The game carries on, so this is not an error. |
| `key escape` / `key fullscreen` | Windows only, and only if Orca's window somehow has the keyboard (Esc, F11). Normally the page sees those keys itself, as on macOS. |
| `orca click` | Windows only: a mouse button (left, right or middle) went down on Orca's window, once per press. The page never sees that click, so the app hands it over: it focuses its window and page, and the page treats it as a press on the box. That means taking DOM focus off whatever had it (a chat field would otherwise keep the keys, since `webContents.focus()` gives it back) and sending `focus`. Do it on every `orca click`, even when the page thinks the game already has the input: Chromium may count a click on another process's child window as its own window losing activation. Orca changes nothing itself on a click. In and out of a session, with no cap: every app forwards the `orca` lines it doesn't know to the page, which ignores them. On macOS the click reaches the page itself ("Focus"). |
| `error <sentence>` | Something failed: bad arguments, the view couldn't open, the game didn't boot. Each `orca error` also gets an `error` twin. |
| `orca state <state>` / `orca error <code> <sentence>` | Session status (`Core/Orca/Status.h`). New states, such as drop-in's `joining`, `friend-joined` and `no-room`, come through unchanged: forward every line that starts with `orca `. |
| `orca menu <event>` | The player picked something in the game's own Online menu: `online casual`, `online ranked` (with `caps host`; `... local` without), `online friends`, or backed out of searching: `cancel` ("Online menu", "Matchmaking"). An event, printed once per pick, only while the player's game is alone. |
| `orca result <json>` | With `caps results`: a matchmade room's verdict on a game or a ranked set ("Matchmaking"). |
| `orca queue ready <casual\|ranked> <char> <costume>` / `orca queue unready` / `orca queue skip` / `orca queue timeout me\|them` | With `caps queue2`: the queue's character select ("The queue's character select"). |
| `exit <code>` | Always the last line, on every exit path: 0 after a normal stop, 1 after a reported error, 2 for bad arguments. |

Embedded, Orca keeps fd 1 for the protocol and points the process's stdout at stderr. So the Dolphin
log (which Windows writes to stdout) and anything else printed go to stderr and never into the
protocol.

### Focus

Dolphin reads every input (keyboard and pads alike) only while `Host_RendererHasFocus()` holds.
Embedded, Orca turns off "background input" for the run so that rule always applies. Embedded, that
means:

- **Windows:** Orca never takes the keyboard. The app's window keeps it, so the page sees every key
  (it sends the keyboard to Orca as a pad in the controller stream).
  - The child window doesn't take the focus on a click, `focus` or `show`; if Windows hands it the
    focus anyway, it gives it straight back to the app's window.
  - A click on the picture lands in the child window, not in the page, so Orca prints `orca click`
    for it (`WM_LBUTTONDOWN`, `WM_RBUTTONDOWN`, `WM_MBUTTONDOWN`). The app and page then do what a
    click on the box does on macOS (stdout, `orca click`).
  - Input counts while the last command was `focus` (the default), the view is shown, and the
    app's window is the foreground window, as on macOS.
  - The `key` lines only come if the child window somehow has the keyboard; normally the page sees
    Esc and F11 itself.
  - Caveat, as with libretro: a child window in another process shares the app's input queue, so a
    stall on Orca's main thread stalls the app's UI. `quit` and a closed stdin stop Orca at once.
- **macOS:** Orca never takes the keyboard or the mouse.
  - Its window can't become key and lets clicks through to the page beneath (`ignoresMouseEvents`),
    so the app stays the active app with its key window. Orca never sees a mouse event, so it prints
    no `orca click`: the page gets the click on the box as its own.
  - Orca reads the keyboard from the HID state and the pads through SDL. Input counts while the last
    command was `focus` (the default), the view is shown, and the app's process is the frontmost app.
  - So the page sends `blur` when it wants keys for itself (a text field, chat, a sheet) and `focus`
    when the box is clicked or focused again.
  - While the game has focus, the page calls `preventDefault` on keys it would otherwise act on
    (Space, arrows).

### macOS window behaviour

Orca's view is a borderless window kept over the app's window. It is ordered just above it with
`orderWindow:NSWindowAbove relativeTo:<CGWindowID>`, so the app's native menus and popups and other
apps' windows still cover it.

- **Dock and app switcher:** the process is `NSApplicationActivationPolicyAccessory`, set before it
  finishes launching, so it has no Dock icon and is not in the switcher.
- **Full screen:** the window joins every Space as a full-screen auxiliary window, so it follows the
  app into full screen.
- **Hidden with the app:** it hides while the app's window is minimized, hidden or on another Space.
  The game keeps running while hidden: Orca's CPU use was the same shown, hidden and minimized.
- **Out of sight, nothing is presented** (also outside embed mode): while Orca's window is ordered
  out, minimized or entirely covered by other windows (`NSWindow.occlusionState`), the Metal backend
  takes no drawable (`Presenter::SetSurfaceVisible`). A covered layer gets its drawables back late,
  and `nextDrawable` waited up to its one second timeout on the CPU thread, which a session shares
  with the GPU: the game fell to 14 fps (2.9 in its worst second) while another app's window covered
  the app's, and the friend's game waited with it. After any `nextDrawable` wait over 50 ms (a window
  covered before Orca hears of it), drawables come from a background acquire, one frame ahead, until
  the window is next out of sight. `Tools/orca/occluded-test.py` measures it.
- **Back on top:** when a click or the switcher brings the app's windows forward, Orca's window goes
  back over the app's window within one poll (8 ms). During a live drag it trails by one poll at most.
- **Covering page content:** nothing in the page can draw over Orca's window, so a page sheet or
  menu over the box has to `hide` it, as the page already does for libretro. The play menu's panel
  beside the game instead cuts `rect` to start right of it, keeps `view` the whole box and sends
  `dim 50`: the game keeps its size and place, its part beside the panel dimmed, the rest under the
  panel.
- **No clipping:** Orca's window isn't clipped to the app's window. The page keeps `rect` inside its
  viewport, as `measure()` does.

### Option (b): IOSurfaces in the app's own view

A spike was measured (`Tools/orca/iosurface-spike.c`). The app checks in a bootstrap name, as
Chromium's rendezvous does, and Orca looks it up and sends three 1920×1080 IOSurfaces as mach ports.
It works on macOS 26:

- Setup takes about 13 ms.
- Per frame, a "frame k is in surface i" message arrives in a median 12 µs, with p99 0.4 ms.

The parts not built:

- **Orca's side:** the Metal backend presents into IOSurface-backed textures (a ring of 3) instead of
  a `CAMetalLayer` drawable. Orca waits on the command buffer before it signals, and resizes recreate
  the ring.
- **The app's side:** a small N-API module in Electron's main process. It adds a `CALayer` to the
  window's content view at the box, sets `contents` to the IOSurface of each frame, keeps the surface
  in use until the next one, and handles moves, scale and clipping itself.

## Brawl Rev 1

2026-10-06, after the launch-night playtest: 347 of about 900 in-app players had a disc refused, and
327 of them brought a genuine Brawl (USA) (Rev 1) (Redump sha1
`0e95949ac585f357e79fcd34b20670b5dca97ac2`, game partition content hash
`ab93ef280b834572523f64c55c6a3205ead66d04`). Orca now plays that exact dump as Rev 2
(`Orca/Disc.cpp` `AliasRevision`).

**How the dumps differ** (both extracted with `dolphin-tool extract -g`, then `diff -rq` and `cmp -l`):

- 5,314 files under `files/` identical (all 126 modules among them), FST identical, apploader, bi2
  and partition layout identical, IOS `0000000100000024` and TMD title version 0 on both.
- `disc/header.bin` and `sys/boot.bin`: byte 7, the revision (1 / 2).
- `sys/main.dol`: one byte, file offset `0x11A1F` (address `0x8001BC9F`): `li r7,1` / `li r7,2` at
  `0x8001BC9C`, the version in the disc ID the game builds at boot. main.dol is at `0x3DD00` in the
  partition.
- `tmd.bin`: the signature and the content hash (the H3 table's SHA-1, which differs because the
  data blocks holding those bytes differ). `ticket.bin`: the signature and the ticket ID
  (`0x1D0`-`0x1D7`). `h3.bin` differs accordingly.

**Choice.** (a) Rev 1 plays only Rev 1 (its own compatibility key) would split a small player base
in two and needs nothing proven; (b) Rev 1 runs as Rev 2 needs the differences to be data the game
reads from the disc, which they are. Taken: (b), done as a read filter, not a RAM write: the
`VolumeWii` of a session's disc returns Rev 2's three bytes (`VolumeDisc::SetReadPatches`), so the
apploader loads Rev 2's main.dol and the header reads revision 2 everywhere (Dolphin's metadata,
`0x80000007`, the profile check, the game INIs). Nothing touches emulated memory, so no frame-hook
logic and no `UX::kCompatVersion` change. Gates: the game ID and revision, then the TMD's content hash
(the same identity the app's `partition_sha1` uses), then each byte holds Rev 1's value before it is
patched. Applied in `BootParameters::GenerateFromFile` and `SetDefaultDisc` (session only),
`PrepareLauncherBoot` and `CheckDisc`. Not applied to a disc inserted later (`InsertDiscCallback`) or
an IPL boot; sessions do neither.

The Rev 1 TMD is what `DIVerify` writes to the session NAND (`title.tmd`), and the Rev 1 ticket is in
ES's title context. Neither reaches emulated memory in any run below, and a drop-in keyframe carries
the host's `title.tmd` whole (only `tmp/` files travel as hashes), so mixed pairs join.

**Proof** (2026-10-06, Mac, headless, `rev1-alias` build; Rev 2 as .wbfs, Rev 1 as .rvz):

- Hashlogs (MEM1 + MEM2 every frame): `bf-mario-link-results.txt` to 10800, Rev 1 == Rev 2 == the
  0.3.27 code on Rev 2, frames 0-10800. `pplus-1v1.txt` to 9000, the same for Project+ (launcher with
  the Rev 1 disc as DefaultISO), frames 0-9000.
- The 0.3.27 code on Rev 1: `orca error disc_revision This disc is RSBE01 revision 1; Orca's
  profile is for revision 2`, exit 1.
- Unit tests: `OrcaDisc.BrawlRev1ReadsAsRev2` (`ORCA_TEST_DISC_REV1`) on .rvz and full .iso Rev 1
  against .wbfs and .rvz Rev 2: the disc header and the partition from 0 to the end of the FST read
  the same; `OrcaDisc.ReadPatchesLandInTheirRangeOnly`. 490 `Orca*` passed.
- Two-Orca, `online-menu-dropin.py <Rev 2> --disc2 <Rev 1>`: a Rev 1 friend dropped into a Rev 2
  host's main menu (keyframe of frame 2161, 38.5 MB of NAND named, not sent) and both went to With
  Friends' character select: PASS, 12 checksums matched on each side, no desync.
- The reverse, `online-menu-dropin.py <Rev 1> --disc2 <Rev 2>` (a Rev 2 friend into a Rev 1 host):
  PASS, 12 checksums matched on each side.
- Project+, `online-menu-dropin.py <Rev 2> --pplus <launcher> --disc2 <Rev 1>`: PASS, 12 checksums
  matched on each side. (A first try timed out waiting for the host's `orca caps` while the external
  disc drive was busy and load was near 25; the retry passed in 44 s.)
- The real queue (dev tickets): `queue-e2e.mjs <Rev 1> --queue ranked --q2 --disc-b <Rev 2>`, a full
  ranked set: PASS, 3 verdicts the same on both sides, 229 checksums matched each, 0 rollbacks.
  `queue-e2e.mjs <Rev 2> --queue casual --q2 --disc-b <Rev 1>`: PASS, 2 games, 101 checksums each (a
  first try failed at a network `fetch failed` on the ticket request, before any match).
- Rev 1 against Rev 1 was not run separately: each Rev 1 run equals a Rev 2 run frame by frame.
- Sync tests (`YG_SYNCTEST=7`) on Rev 1: Brawl to 10800, 73,451 re-run frames, 0 RAM mismatches;
  Project+ to 9000, 60,851 re-run frames, 0 RAM mismatches.

`ORCA_DISC_SHA1` is in the compatibility key but the app doesn't set it yet; when it does, it must
pass Rev 2's hash for this dump, or Rev 1 and Rev 2 players get different keys.

**Review fixes** (2026-10-06, rebased onto main with 0.3.28's Code Menu lock and play again):

- A loader's disc (Project+) was aliased on its own volume in `SetDefaultDisc` after
  `PrepareLauncherBoot` checked another volume, and the result was ignored: a failed read there would
  have booted Rev 1 unaliased against Rev 2 players. Now a session refuses a Brawl (USA) disc that
  doesn't read as Rev 2 there (`Orca::SessionDiscReady`): `orca error disc_revision`, no boot.
- `OrcaDisc.ReadPatchesWithAWrongOriginalPatchNothing` (Rev 2 image): a list whose second byte
  doesn't hold its original is refused and every read stays the same. 503 `Orca*` passed with both
  test discs, `BrawlRev1ReadsAsRev2` ran.
- Merged candidate, two Orcas, Project+ casual from the real queue (dev tickets), host Rev 2 .wbfs,
  joiner Rev 1 .rvz (`queue-e2e.mjs <Rev 2> --pplus <launcher> --queue casual --q2 --disc-b
  <Rev 1>`): PASS, 2 games, the verdicts the same on both sides, 100 checksums matched each, 0
  rollbacks. A first try failed (`peer_left` after game 1, 44 / 45 checksums matched, no desync):
  the external drive's APFS image logged one 81 s transaction stall (`disk11 ... took 81193606 us to
  close`) over the same minute both Orcas dropped to about 2 fps.

## Launch-night fixes (w2-fixes, 2026-10-06)

**Blank stage select (ranked and casual Project+, and With Friends).** The X screenshot (strike
screen, "X strikes a stage", no tiles, only RANDOM) is Project+'s My Music step 1 (`0x80002810`,
`Source/Community/MusicSelect.asm`): `HOOK @ $806b10d0` skips `dispPage` while the step is 1, so the
stage select builds no tiles and no page button, and the transition hook turns the next pick into a
trip to My Music. Y on a stage sets it; the main menu's cleanup (`HOOK @ $806dce4c`) skips step 1
on purpose, and only a page change clears it, so a step set without the trip happening (Y on a tile
the pick then refuses, a scene change mid-way) stays for every later stage select. In the flows Y
and the page button are masked, so the screen never recovers: the timer's A went to My Music.
Repro: `ORCA_UX_PROBE` writing `80002810 = 1` on the character select, `pplus-ranked.txt`: blank
strike screen (shot identical to the report), then `scSelStage -> muMenuMain` (My Music) at the
auto-pick. Fix: `RankedPPlus::ClearStaleMusicSelect`, every Project+ session, on the character
select (My Music's real round trip never passes there). After: the set completes 2-0, and the plain
Versus stage select (no flow) that was blank the same way shows its tiles. Synctest: a probe write
on the character select never lands under `YG_SYNCTEST` (each rewind re-enters the scene, so the
`:1` visit's frame never comes), so write it on the main menu before `YG_SYNCTEST_FROM`
(`@muMenuMain:1 60 write 80002810 00000001`, `YG_SYNCTEST_FROM=280`): every re-run of the
character select's first frame clears it again, 81,991 re-run frames, 0 RAM mismatches, set 2-0.
The triage's other lead, `CURRENT_PAGE` (`0x80496000`)
left at 1, does not reproduce: Project+ resets it to 0 as the stage select opens (probe watch), and
the strike screen shows its tiles.

**Room ticket.** `HttpRequest(10 s)` gave up at about 10-15 s while the page took 12-60 s during the
outage; 30 s and one retry when no reply came. `fake_bridge.py FAKE_TICKET_DROP_FIRST=1` proves the
retry (`OrcaLive.BridgeMirrorsTheRoomAndTakesLeave` gets its room after the dropped request; the
test's last check, the joiner leaving after the host, fails the same with or without the drop: the
rooms Worker now keeps the joiner).

**Keyframe refusals.** A final 4xx from the store (`LastRefusal`) used to make the host pack and
upload a 22-27 MB keyframe every ~3.5 s while a friend waited (1,250 refused uploads on launch
night). Now the waiting friend is dropped with `signed_out` or `refused` at once. 403 `signed_out`
first retries with a fresh ticket. `Tools/orca/dropin-refused.py` (dev tickets, which the store
refuses with 403 `dev_ticket`): PASS, the friend hears `orca error network` 2 s after joining (it
used to wait out its 90 s join limit), and the host made one keyframe. Only refusals with YouGame's
`code` count: a bare 4xx (an edge page, a 408) fails that keyframe and the friend gets another, as
before.

**Logs.** `orca queue unready` now logs why (B, the token picked up, A on Back, cleared after the
room); the compatibility key logs its inputs when they change, so a player alone in a queue shows
which input split them (FMA, files, settings).

**Paused room.** `Platform::WatchRoomWhilePaused` runs `KeepRoomOpen` on the CPU thread every 2 s
during the player's pause (`Core::RunOnCPUThread`), so an edge close while paused reopens the room
instead of leaving it dead until the player unpauses. `Tools/orca/pause-room.py`: PASS, paused,
`test-drop-room`, `orca error network` 1 s later, the room open again 10 s later, never running.

## Code comments from 0.3.27, as written

Merging rel-0.3.27 (`34897a118b`) into the cleaned main shortened its new code comments to main's
style. The originals that carried detail beyond ORCA.md are kept here verbatim; every other one is
in that commit (`git show 34897a118b:<path>`).

### Source/Core/Core/Orca/Status.h: drop-in states, `orca menu`, capabilities, PICK_CAPS

```
// Machine-readable status for the YouGame desktop app, one line each on stdout in a session:
//   orca state booting|lobby|playing|ended, and drop-in's (ORCA.md "Drop-in"): joining <percent>,
//   friend-joining, friend-joined, friend-left <why>, host-left, left, left lobby (With Anyone >
//   Casual or Ranked was picked in a host's friends lobby, by any player in it, and the host left
//   its friends for the queue; its `orca menu online ...` follows; each friend gets host-left and
//   the same `orca menu online ...` once home), no-room (a former joiner in a fight, until it is
//   back on the menus and opens a room of its own), and a held join's
//   (ORCA.md "Boot and friends from the menus"): friend-holding (the host: a friend waits until its
//   game is back in the menus), friend-waiting (the joiner: the host plays a single-player mode)
//   orca error <code> <sentence for the player>
//   orca menu <event>, a pick in the game's own Online menu: online casual, online ranked (with the
//   app's "host"; "... local" without it; "... kept" from a host that kept its friends because its
//   page can't search that queue), online friends, cancel (ORCA.md "Matchmaking")
//   orca result <json>, a verdict of the room in a matchmade room (with the app's "results")
//   orca queue ready <casual|ranked> <char> <costume> | unready | skip | timeout me|them, the
//   queue's character select (with the app's "queue2", UX/Queue.h; through Line())
//   orca shaders <compiled> <total>, while the boot compiles shaders before the first frame
// Codes: boot, cancelled (Orca.app's disc picker closed without a disc), disc_missing and
...
// "orca menu <event>": the player picked something in the game's own Online menu (ORCA.md "Online
// menu"): "online casual", "online ranked" (with the app's "host"; "online casual local" and
// "online ranked local" without it; "online casual kept" and "online ranked kept" from a host that
// kept its friends because its page can't search that queue: PICK_CAPS), "online friends", and
// "cancel" (the player backed out of a search). An event, never a state: each pick prints once (a
// kept one again, armed, once the friends are gone).
...
// Capabilities (ORCA.md, "Embedding"): what this build implements, printed once as
// "orca caps <list>" before "ready", and what the app said it understands ("caps <list>" on
// stdin; a later "caps" line replaces the whole answer, and every feature is read where it acts,
// so the page may answer again mid-run: "pick-casual", "pick-ranked" below). A feature is on only
// when both list it. "pause" needs no answer: it says this Orca pauses a session while the
// player's game is alone in it (older ones answer "unsupported pause").
...
// "pick-casual", "pick-ranked": the page can search that queue right now (YouGame: the queue's
// switches on, and for Ranked a signed-in player; its lib/orca.ts orcaCapsAnswer), and says so
// again on a later "caps" line when that changes (the player signed in, a switch flipped). A host
// with friends in its game leaves them for a With Anyone pick only with that pick's cap; without
// it the pick is announced in place, unarmed ("orca menu online <queue> kept"), so the page says
// why and cancels, and armed once the friends are gone (Rollback/OnlineMatch.h DecideLobbyPick,
// DecideKeptPick). The page's switches and sign-in are its server props: a change reaches a running
// page on a refresh (signing in in place does one), and a reload starts a new run anyway. Offered
// with "locks" (a ruleset: the profiles with the Online menu). With them a ruleset profile offers 15
// caps; the app passes 16 at most (raising that takes a desktop release).
inline constexpr const char* PICK_CAPS = "pick-casual pick-ranked";
// What this build offers for the running profile: CAPS, "results" where its reader is verified,
// and "locks", "queue2" and the pick caps where it has a ruleset.
```

### Source/Core/Core/Orca/UX/UX.h: kCompatVersion, every `ux` step

```
// Bumped whenever anything here that writes emulated memory changes (the name tags; 2: the game
// patches, GamePatches.h, whose file is also hashed into the key; 3: a launcher profile's patches
// are applied too, PPLUS32.patches; 4: the main menu's text about going online, MenuText.h; 5: the
// menu's description box's copy of that text too; 6: a matchmade host's rules preset, Results.h;
// 7: each player's own controls in their name tag, and the port label for a player with controls
// and no name, NameTags.h; 8: the online rules for queue matches (OnlineRules.h, which replace 6)
// and the ranked set's stage, character and result flows (RankedPPlus.h, BrawlStages.h,
// CharOrder.h, RankedSet.h), with their input masks; 9: A on the character select only in the
// grid while the online rules lock, OnlineRules.h; 10: queue2's character select, ready timer and
// steered pick (Queue.h, the header's queue flags), Project+ booting to its main menu and a friend's
// drop-in from the menus (PPLUS32.patches, OnlineMenu.h FriendsMove), the character select's title
// (CssTitle.h), the casual stage pick and the later-game character order (RankedSteps.h,
// BrawlStages.h, RankedPPlus.h, CharOrder.h) with the gate's a_as and steer; 11, Orca 0.3.15: no
// Z on the results screen in any session, its replay save's dead end, OnlineRules.h SessionMasks;
// 12, Orca 0.3.16: the character select's A to pick, B to take the token back up and Start to
// lock in (OnlineRules.h CssBUnpicks, CharOrder.h, Queue.h; the raw latch under every header that
// locks), and the stage select's two cursors, proposals and agreement, and casual's simultaneous
// 15 s pick (StageCursors.h, RankedSteps.h, BrawlStages.h, RankedPPlus.h); 13, Orca 0.3.17: a
// ranked set's room stays open after its verdict and both games stay in their session on the
// results screen until each player leaves it (OnlineMatch's SetOverLeave; an older build would end
// its session at the verdict while the other plays on); 14, Orca 0.3.18: Orca's words in the
// game's own text boxes, the players' names and ratings on their character select plates and the
// queue's line in the rules bar (NativeText.h); 15, Orca 0.3.19: Random a pick like any
// character's, the joiner's Random steered in without turning its colour (Queue.h
// RANDOM_CHARACTER), and no skip in ranked; 16, design F: the plates without the rating and
// SEARCHING on the queue's own select, the rules bar without the timer the band shows, the
// relabels' marks (Relabel.h); 17: a friend's drop-in from the menus waits for the main menu to
// have built its pages (OnlineMenu.h FriendsMove MENU_BUILT), so the move comes at a later frame;
// 18: A on the ONLINE page's With Friends goes straight to the character select and B from there
// back to the ONLINE page on With Friends (no With Friends page: RSBE01.patches, PPLUS32.patches
// muProcWifi and the back out's helper), and A on BACK on the queue's own character select
// (OnlineRules.h CssBackTakesA, Queue.h FLAG_BACK_A, InputGate a_centres_stick); 19: no CPUs
// online and only the seats in use (OnlineSeats.h: the character select's record and panels
// cleared of CPUs, A kept off the player-type and name buttons on
// With Friends and a friend's drop-in and pressed for a player whose panel a CPU left, empty
// panels nobody plugged into hidden):
// the session hashes it into the compatibility key as `ux=<n>`, so two builds that would write
// different bytes (or mask different inputs, or part differently) never meet in a room.
constexpr int kCompatVersion = 19;
```

### Source/Core/Core/Orca/UX/OnlineMenu.h: the reader, and leaving the friends lobby

```
// The reader follows the menu at every frame boundary that runs a frame for the first time (never a
// re-run: what a rollback undoes was never decided) and announces a pick itself only when it
// happens while this game plays alone (Events' `alone`). A pick made while the game isn't alone
// (friends play, or one is about to land: an arrival or a prepare-join not taken yet, an invite's
// stored keyframe that a friend who arrives would replay from; or the app's own join or leave
// waits) is never printed later by the reader, now or once the game is alone again. Casual or
// Ranked made so goes to the session once, at the boundary that saw it (TakeLobbyPick below, Luis
// 2026-10-05: they drop the friends lobby): a host whose page can search that queue now (the app's
// "pick-casual" / "pick-ranked": PickSearchable) leaves its friends and announces it
// (AnnounceOnlinePick), one whose page can't keeps them and announces it unarmed (`... kept`: the
// page says why; armed once the friends are gone), a joiner lets it go, and its host's leaving
// brings it home on the character select that pick took both games to, where it announces the pick
// that select holds (CssPick).
// Acting on a first run's pick while the session's inputs may still be guessed is safe there: the
// session ends at that boundary, so what the host's solo game does next follows its own view of the
// game. With Friends while friends play stays theirs. Whenever the game stops following on from
// what the reader saw (Events NoteResync: back to solo after a session, a friend's keyframe loaded)
// it starts over from the next boundary without announcing anything, so nothing stale is ever
// printed.
...
// ---- Leaving the friends lobby for the queue, and coming home (ORCA.md "Drop-in", "Online
// menu"; Luis, 2026-10-05: Casual and Ranked under With Anyone drop the friends lobby for a
// 2-player room the matchmaker fills, and a guest whose host left goes back to their own lobby) ----

// Prints "orca menu online ..." for a pick (with the app's "host": "online casual" / "online
// ranked", else "... local") and starts what it means: the queue's own character select (with the
// app's "queue2"), the search, or neither (Friends). The reader's own announcement, and the
// session's for a pick it took out of the friends lobby (TakeLobbyPick). `arm` false prints the
// line alone, marked "kept" ("online ranked kept"): a host that keeps its friends because its page
// can't search that queue now (PickSearchable; Rollback/OnlineMatch.h DecideLobbyPick), so the page
// says why and cancels (never a search: the friends are in) and nothing of the queue comes near the
// friends' game (its own character select's header, which a friend on the way would refuse; the
// search's lines).
void AnnounceOnlinePick(OnlinePick pick, bool arm = true);

// Whether the app said its page can search this pick's queue right now (Casual "pick-casual",
// Ranked "pick-ranked": Orca::Status PICK_CAPS); never Friends. Any thread.
bool PickSearchable(OnlinePick pick);

// A With Anyone pick (Casual or Ranked) the frame hook saw at this boundary while the game was not
// alone (friends in it or about to land), which nothing printed: the session takes it at the same
// boundary, after the hook (Rollback/OnlineMatch.cpp). A host leaves its friends lobby for the queue
// and then announces it (with the pick's cap; without, it stays, announces it unarmed and arms it
// once they are gone); a joiner's game is its host's, so it lets it go. Once. CPU thread.
std::optional<OnlinePick> TakeLobbyPick();

// Whether a scene is "the menus" for a former joiner to move to port 1 there (Rollback/
// OnlineMatch.cpp ComeHome): the main menu (every page of it) or a character select, never a fight,
// a stage select, a results screen or the moment between two scenes, and never inside a
// single-player mode (SequenceHoldsDropIn). Pure.
bool MenusScene(std::string_view scene, std::string_view sequence);
// MenusScene as the frame hook read it at this boundary (first runs; false before the first, and
// for any game but Brawl rev 2 and Project+). CPU thread.
bool OnTheMenus();

// The With Anyone pick (Casual or Ranked) the character select the frame hook read at this boundary
// was opened for (the main menu's exit code Orca keeps in the running sqVsMelee,
// CssTitle::ReadPick: 30, 31), nullopt anywhere else (another scene, With Friends' character
// select, a Versus one opened otherwise). Where a former joiner that comes home there picks up that
// pick (Rollback/ OnlineMatch.cpp ComeHome). First runs, as OnTheMenus. CPU thread.
std::optional<OnlinePick> CssPick();
// Whether a Casual or Ranked pick the session couldn't take at the boundary that saw it (Rollback/
// OnlineMatch.cpp TakeLobbyPickNow) still stands where the frame hook last read the game: on the
// way out of the main menu with that pick's exit code (between two scenes), or on the character
// select that pick opened (CssPick). Anywhere else (back on the main menu, a stage select, a fight)
// it is stale. CPU thread.
bool PickStands(OnlinePick pick);
// The pick a main menu exit code (the one the menu leaves with, or the one a character select keeps)
// stands for when it is Casual or Ranked (30, 31). Pure.
std::optional<OnlinePick> QueuePickForCssCode(u32 code);
```

### Source/Core/Core/Rollback/OnlineMatch.cpp: TakeLobbyPickNow, ComeHome, ArmKeptPick, TellNoRoom

```
// ---- The friends lobby and the queue (Luis, 2026-10-05) ----
// A With Anyone pick (Casual or Ranked) the frame hook saw at this boundary while this game wasn't
// alone (UX/OnlineMenu.h TakeLobbyPick), after the app's commands: Casual and Ranked drop the
// friends lobby for a 2-player room the matchmaker fills (DecideLobbyPick). A host whose page can
// search that queue now (the app's pick cap) leaves its room as the app's Leave does (its friends
// play on without it, hear `host-left`, and go home on the character select the pick took every
// game to, where they announce it too: ComeHome), prints `orca state left lobby`, and then the pick
// as if made alone (`orca menu online casual|ranked`: the queue's own character select once this
// game is alone on port 1, from the next boundary). Without the cap it keeps its friends and prints
// the pick's line alone (unarmed: `orca menu online <queue> kept`), so its page says why it can't
// search and cancels; ArmKeptPick arms it once they are gone. Only at
// a boundary the app's commands may take (never mid re-run), which the hook's first run is. Local:
// the session ends here, so nothing of this reaches the friends' games but the room's leave.
void TakeLobbyPickNow(Match& match)
  // A fresh pick is the one that counts (one that waited would have been stale by then anyway).
    // One that waited is stale once the game has gone on from it (back on the main menu, on to a
    // stage select or a fight): leaving the friends for it then would leave them for nothing.
    // The game already left the menu for the character select: never dropped, or Start there
    // would do nothing.
  // A newer pick replaces one kept earlier, whatever it comes to.
    // Either player's press makes the pick in a shared menu: said as what happened, not who did it.
// A former joiner on another port than 1 with no room (MayComeHome), at its first alone boundary
// on the menus: port 1 from the next frame and a room of its own a moment later, as after the
// queue's room (MaybeRestoreQueueImage). Solo with no session, so nothing re-runs this boundary and
// only this game changes (its pads: port 1 plugged in, the old port unplugged; the frame hook's
// ports from the next boundary); the next friend's keyframe carries it. No `no-room`: invites reach
// the new room, and the queue's own character select gets its header (WantedRulesMode).
void ComeHome(Match& match)
  // Every boundary: the common case (port 1, or not solo) asks the room nothing.
  // On port 1 now: a room of its own a moment later (ReopenRoom decides that for every solo game).
  // Home on the character select a With Anyone pick opened (its host left the friends lobby for the
  // queue at that pick, whoever pressed it): this player takes the same way, so Start readies it on
  // the queue's own character select (its header from the next boundary) rather than doing nothing.
  // Whatever its page can search: alone, the page answers it as any alone pick.
// A pick this host announced unarmed to keep its friends (TakeLobbyPickNow), at each boundary after
// it: once the friends are gone and the game is alone on the character select that pick opened, it
// is armed as an alone pick is (`orca menu online <queue>`: the queue's own character select from the
// next boundary, so Start readies it; the page answers it as any pick). Dropped once the game has gone
// on from it (back on the main menu, a stage select, a fight) or a queue or search is under way.
// First runs only (a session's re-run never gets here alone).
void ArmKeptPick(Match& match)
// A player on another port than 1 that didn't come home at this boundary (in a fight, say) has no
// room of its own: the app hears so once, after the room it left has told the page it is empty
// (its emptied mirror is the page's last word about it, and the page must not see that room again
// after "no-room"). It still comes home once it is back on the menus.
void TellNoRoom(Match& match)
```

### Tools/orca/dropin-rooms.py: steps 5-10

```
# 5. Both under the app (a Tools/orca/fake_bridge.py each), the friend joining from the app's invite:
#    the host's page Leave is the app's "leave". The host leaves ("left"), its page is told the room
#    is empty, then shown the host's new room. The friend ("host-left") has told its page the old
#    room is empty and never shows it again: in a fight it says "no-room" (port 2) after that; on
#    the menus it goes home (port 1, a room of its own, which its page is shown).
# Where a step's friend is left on another port than 1 (3, 4 and 5), it either goes home at once (it
# was on the menus: "Drop-in: back to a game of its own") or says "no-room" (in a fight): both pass.
# 6. A friend whose host stops goes home and can queue (YouGame 2026-10-05, "Start did nothing"):
#    both under the app with the queue's character select (caps host queue2) and live pads, no input
#    script. The friend joins the host on the main menu, both go to With Friends' character select,
#    the host's app stops it ("leave", "quit"). The friend hears "host-left", backs out (held B), and
#    picks With Anyone > Ranked from the ONLINE page (through the With Friends page before ux=18), a character, Start: "orca queue ready ranked"
#    within 10 s (through Orca 0.3.22 nothing: port 2, no room, no header). Then: it came home on
#    the character select (port 1), never said "no-room", and its page was shown a room of its own.
# 7. With Anyone with a friend in the game (Luis, 2026-10-05: Casual and Ranked drop the friends
#    lobby): the same pair on With Friends' character select; the host backs out and picks With
#    Anyone > Ranked with the friend still plugged in: "orca state left lobby", then "orca menu online
#    ranked"; the friend hears "host-left" and goes home on the character select the pick took both
#    games to, where it takes the pick too ("orca menu online ranked" after its "host-left"). Neither
#    backs out: each picks a character and presses Start on the select it landed on, and both say
#    "orca queue ready ranked", each on the queue's own character select (its header).
# 8. The same, but the FRIEND presses With Anyone > Ranked in the shared menu (either player's press
#    makes the pick there): the host, who pressed nothing, leaves for the queue all the same, and the
#    friend comes home on its RANKED select, armed: both ready without backing out.
# 9. A friend drops into the room of a guest that came home: the pair on With Friends' character
#    select, the host's app stops it, the friend goes home (port 1, a room of its own); a new Orca
#    joins that room from the app's invite (a keyframe of a former joiner's game: the old host's port
#    names cleared), both play 15 s, the new one's app leaves, and the session the former guest
#    hosted ends having matched checksums ("friend-joined", then "friend-left", no error).
# 10. The same pair, but the host's page can't search Ranked now (signed out, or its switch off: the
#    caps answer has no pick-ranked): the host's Ranked pick keeps the friend. The host prints `orca
#    menu online ranked kept` (unarmed: no queue header, no queue select) and no `left lobby`, the
#    friend hears nothing, and the page's `queue-cancel` changes nothing; 8 s on both still play
#    together. Then the friend's app leaves: the session ends with checksums matched, the host,
#    alone on that RANKED select, arms the kept pick (`orca menu online ranked`, then `orca queue
#    ready ranked` after Start), and the friend goes home there and takes the pick too.
#   --only rooms|page|home|lobby|guest|rejoin|stay[,...]: those steps alone (rooms: 1-4, page: 5,
#   home: 6, lobby: 7, guest: 8, rejoin: 9, stay: 10).
# Each step waits for the lines it expects and fails on a timeout. Two Orca processes at most.
```

### Source/Core/Core/Orca/UX/OnlineSeats.h: file header (No CPUs online)

```cpp
// No CPUs online, and only the seats in use on the screen (ORCA.md "No CPUs online"; `ux=19`).
//
// Luis, 2026-10-05: "If you go into any other mode and create a CPU, and then you exit and go into
// ranked, that CPU actually remains, and it creates a glitched illegal condition. Whenever we're in
// an online mode, we should be clearing out any bots and continuing to disable it. ... If I go into
// casual or ranked, I only see one character possible, which is me. When someone else joins, they
// take the player 2 slot."
//
// How the CPU got there (found under the harness, q12): the Versus character select builds its
// panels from a record the game keeps between visits, gmSelCharData ([0x805A00E0]+0x10; each
// port's +0xB8 + port x 0x5C: +0 the character, +1 the player's state, 0 a human, 1 a CPU, 3
// nobody), and saves its panels back into it only as it goes on to the stage select. Vanilla Brawl
// resets that record whenever Versus starts from the main menu; Project+ doesn't, so a CPU from a
// local game that reached the stage select came back on With Anyone's Casual and Ranked selects and
// on With Friends', and went into the queue match: a third fighter in game 1 (a CPU on panel 3), or
// a room whose joiner could never take panel 2 (a CPU there) and never left its character select.
// Even in Brawl, A on an empty panel's player-type button made a CPU on With Friends' select.
//
// What is online here (Online()): the match block's header locks (the queue's own character select
// or a casual or ranked queue room, OnlineRules.h), or the main menu's pick Orca keeps in the
// running sqVsMelee (+0x18; CssTitle::ReadPick) is With Friends (24-27) or With Anyone's Casual (30)
// or Ranked (31), or two ports or more are plugged in (a friend who dropped in, wherever the host
// was). Group > Brawl alone, and everything else offline, keep CPUs as the game has them.
//
// While a select is online, at every frame boundary, first runs and re-runs, writing only bytes that
// differ (a pure, idempotent function of emulated memory and the synced ports):
// - **The record** (ClearRecord), between two scenes (scMemoryChange) only: a CPU's state becomes
//   nobody's, and so does a human's on a port nobody plugged in (a second local player's, kept by
//   Project+). The first such boundary after the menu's pick comes before the character select
//   builds its panels (5 boundaries before it in Project+, 33 in Brawl), so the select starts with
//   only the players who are there. Never on the stage select or in a fight: the stage select's end
//   copies the record into the fight's players, and by the next boundary that copy is made, so no
//   fight ever loses a player to this.
// - **The panels** (ClearPanels), on the character select: a CPU's panel (its area's kind +0x1B4, 2)
//   becomes an empty one (0). The select saves that kind into the record as it leaves, so the CPU
//   plays no fight. The panel's own picture isn't the game's empty one then (its CPU plate stays),
//   which is why the seats below hide it, or the join below makes it a player's. A human's panel
//   whose port nobody plugged in is the game's: it empties one itself 17 frames after its
//   controller goes (both games, the token back in the hand and off the grid), and builds none for
//   a port without a controller; a write here would come first and leave the token's highlight on
//   the grid (found under the harness, q12).
// - **The buttons** (GateMasks): where no header locks (With Friends, a friend's drop-in, Casual or
//   Ranked before the queue's header lands), A never reaches a player-type button (0x1D: a CPU of an
//   empty panel, or of a human's own) or a name button (0x1C), nor anywhere within one frame's
//   travel of them (TYPE_BUTTONS_A_FLOOR). Under a header the online rules already pass A only in
//   the grid. Joining a fresh panel needs no A: a hand that moves into the grid with its token
//   joins it. A panel a CPU had (its token left on the grid, gone with the CPU) can't be joined
//   that way: Orca presses A for its port while that port's hand rests on the panel's own
//   player-type button (PressesJoin), which the game turns into the player's panel, as a local
//   player rejoins after removing a CPU. So a friend who plugs into a live select whose CPU was on
//   their panel takes it at once.
// - **The seats** (ApplySeats), on the character select: an empty panel (kind 0) whose port nobody
//   plugged in is hidden, its 39 game models (MuObjects at area +0xB0..+0x14C; each one's nw4r
//   ScnMdl at +0xC) drawn neither opaque nor translucent (ScnObj flags +0xCC, 0x60 over the game's
//   0xA0000000). So the queue's own select shows the player alone, with the opponent's seat
//   (SEARCHING, the kit's ring) once it searches; a queue room shows both players; With Friends
//   shows the players who are there, a friend's panel appearing as they plug in. Hiding is drawing
//   only: the panel's logic and its buttons stay (the A masks above keep them dead). Any panel
//   that should show and carries Orca's 0x60 gets it cleared, on any character select, so a select
//   that stops being online (the friend left Group > Brawl) shows its panels again.
```

### Source/Core/Core/Orca/UX/OnlineSeats.h: TYPE_BUTTONS_A_FLOOR

```cpp
// ---- The buttons (both games, the 2026-10-03 rasters: css-gate-runs raster.log, pplus-map.log) ----
// The player-type and name buttons start at y -18.3 or lower (between -17.4 and -18.3) in both
// games; the hand moves at most 1.0 a frame (Brawl 0.83). From -16.4 or above, no single frame's
// stick takes it onto one with A down. Brawl's panel picture (a costume, 0x1B) ends at -17.4,
// Project+'s at -16.6, so A below -16.4 loses only the picture's bottom edge (X and Y still turn the
// costume).
```

### Source/Core/Core/Orca/UX/OnlineSeats.h: PressesJoin

```cpp
// Whether Orca presses A for port `port` (0-3, plugged in) on an online select without a header:
// its own panel is empty (`kind` 0) with its token neither in the hand nor flying (a CPU's panel
// once it is empty, whether the game or ClearPanels emptied it; a fresh empty panel holds its token
// in the hand), and the port's hand rests on that panel's own player-type button (where a
// player's hand starts). A there makes the panel the player's (HMN), the game's own way back from a
// CPU's; nothing else joins it (the hand moving into the grid doesn't, with no token in it).
```

### Source/Core/Core/Rollback/OnlineMatch.h: HomeInputs

```cpp
// A former joiner's way home (ORCA.md "Drop-in"; Luis, 2026-10-05: a guest whose host left goes back
// to their own lobby). A player that joined a friend's game and then went solo (its host left, it
// left, its session ended) plays on the port it was given, with no room: nobody can drop in, and the
// queue's own character select never gets its header there (it wants port 1), so Start would do
// nothing. At its first alone boundary on the menus it moves to port 1 and a room of its own opens.
```

### Source/Core/Core/Rollback/OnlineMatch.h: LobbyPickStep

```cpp
// What the session does with a With Anyone pick (Casual or Ranked) the frame hook saw while the
// game wasn't alone (UX/OnlineMenu.h TakeLobbyPick), at the same boundary. Luis, 2026-10-05: Casual
// and Ranked drop the friends lobby for a 2-player room the matchmaker fills, but only for a queue
// the page can search now (the app's "pick-casual" / "pick-ranked"; else the friends would be
// dropped for nothing, and the page would say "needs an account" or "coming soon" and cancel). In a
// shared menu either player's press makes the pick, and every game sees it at the same frame.
enum class LobbyPickStep
{
  // Nothing: a joiner's game is its host's (the host leaves, and this game hears host-left, then
  // comes home on that pick's character select and announces it there: AnnounceHomePick); a queue
  // room's menus are locked; without the app's "host" nothing would search ("... local"), so the
  // friends' game stays theirs, as before.
  Ignore,
  // Nobody to leave by now (the app's leave or join went first, or the last friend has gone and the
  // session only idles out its last frames): just announce the pick. Or a host with friends whose
  // page can't search the picked queue now (no pick cap): it keeps them and announces the pick in
  // place, unarmed (LobbyPickPlan::arm: `orca menu online <queue> kept`), so its page says why and
  // cancels; once they are gone the pick is armed (DecideKeptPick).
  Announce,
  // A host with friends in its game or on their way, whose page can search the picked queue: it
  // leaves the room (its friends play on without it: `orca state host-left`), prints `orca state
  // left lobby`, and announces the pick.
  Leave,
```

### Source/Core/Core/Rollback/OnlineMatch.h: LobbyPickPlan

```cpp
struct LobbyPickPlan
{
  LobbyPickStep step = LobbyPickStep::Ignore;
  // An announced pick drops what drop-in kept for an invite (made under the header before the
  // pick): a friend who comes later gets a keyframe of the queue's own character select. Never
  // with a session still running, and never with friends kept.
  bool drop_keyframe = false;
  // The announcement starts what the pick means (UX/OnlineMenu.h AnnounceOnlinePick: the queue's
  // own character select, or the search). False for a host that keeps its friends: the line alone,
  // since the queue's header would reach a friend on the way (who would refuse it: `orca error
  // mismatch`) and the page cancels anyway.
  bool arm = true;
};
```

### Source/Core/Core/Rollback/OnlineMatch.h: AnnounceHomePick

```cpp
// A former joiner coming home (ComeHome) on a character select a With Anyone pick opened (UX/
// OnlineMenu.h CssPick: its host left the friends lobby at that pick, whoever pressed it): whether
// it announces that pick there (AnnounceOnlinePick: the queue's own character select, so its Start
// readies it as it does its host's). Only with the app's "host" (without it nothing would search),
// and never over a queue or search already under way. The pick caps don't matter here: it is alone,
// and its page answers the pick as any alone pick (its reason, or the search).
```

### Source/Core/Core/Rollback/OnlineMatch.h: KeptPickStep

```cpp
// A pick a host announced unarmed to keep its friends (LobbyPickPlan::arm false), at each later
// boundary (ArmKeptPick): armed once the game is alone on the character select that pick opened
// (the friends gone), as an alone pick is, so Start there readies it rather than doing nothing;
// dropped once the game has gone on from that select (UX/OnlineMenu.h PickStands) or a queue or a
// search is under way anyway.
```

### Source/Core/Core/Rollback/Harness.cpp: ReseatLocalPort

```cpp
// ORCA_UX_TEST_RESEAT_AT=<frame> (harness runs, not online): the player plays port 2 until that frame
// and port 1 from it, the other of the two reporting no controller, as a former joiner whose host
// left plays solo on port 2 and moves to port 1 at its first boundary on the menus
// (Rollback/OnlineMatch.cpp ComeHome). The script's port-1 rules drive whichever port the player is
// on, and the frame hook's ports follow (the test names' first, alone). The player's port (0 or 1)
// for `frame`, or -1 without the knob.
```

### Source/Core/Core/Rollback/Harness.cpp: TestPortUnplugged

```cpp
// Whether test port `port` (0-based) reports no controller for `frame`: one that left
// (ORCA_UX_TEST_UNPLUG_AT), and with ORCA_UX_TEST_PADS_FOLLOW_PLUGS=1 any port outside the frame
// hook's ports for that frame (beyond ORCA_UX_TEST_NAMES, or a friend's before
// ORCA_UX_TEST_PLUG_AT), as a session's port without a player does: a friend's controller then
// appears to the game as they plug in. Port 1 always has its pad. Not for ORCA_TEST_QUEUE_PICK.
```

### Source/UnitTests/Core/Orca/OnlineSeatsTest.cpp: OrcaOnlineSeats.AFriendWhoLeftIsTheGamesToEmpty

```
  // With Friends' select, three players in, then port 3's friend leaves (unplugged). The game
  // empties their panel itself 17 frames after the controller goes, its token back in the hand
  // (both games, under the harness); a write here would come first and leave the token's highlight
  // on the grid. So the panels' clear leaves every human's panel alone, plugged in or not, and the
  // panel is hidden once the game has emptied it.
```

### Source/UnitTests/Core/Orca/OnlineMenuTest.cpp: OrcaOnlineMenuLobby.CasualOrRankedWithFriendsLeavesTheFriendsLobby

```
  // A host (port 1) in a session with a friend in it, the page's queue on and able to search the
  // picked queue now (the app's pick cap).
```
```
  // A friend's arrival the room has and the host hasn't taken yet: a friend too (left in, it
  // would be taken into the queue's game and refuse it), and so in an idling session.
```
```
  // A joiner: the pick is its host's game's (the host leaves, this game hears host-left and comes
  // home on that pick's character select: AnnounceHomePick).
```
```
  // No page queue ("... local": nothing would search): the friends' game stays theirs, whatever
  // the pick caps say.
```

### Source/UnitTests/Core/Orca/OnlineMenuTest.cpp: OrcaOnlineMenuLobby.WithFriendsAQueueThePageCantSearchKeepsThem

```
  // A host with a friend in its game whose page can't search the picked queue now (Ranked signed
  // out, or the queue's switches off: no pick cap). Leaving would drop the friends for nothing:
  // it stays, and announces the pick in place, unarmed, so the page says why and cancels.
```
```
  // Friends on their way too (waiting for a keyframe, plugging in, or an arrival not taken yet):
  // kept, and what drop-in kept for them stays (no keyframe dropped, nothing of the queue armed,
  // whose header they would refuse).
```
```
  // Without the app's "host" nothing searches at all: ignored as before, cap or none.
```
```
  // Nobody to leave by now: announced armed as an alone pick (the reader's own announcement is
  // armed whatever the page can search; the page answers it the same way), cap or none.
```

### Source/UnitTests/Core/Orca/OnlineMenuTest.cpp: OrcaOnlineMenuLobby.EveryCapFitsTheAppsCapsLine

```
  // The app's stdin filter and its `orca caps` parser take 16 words, and so does the page's
  // (YouGame desktop/src/orca-policy.ts ORCA_COMMAND, parseOrcaLine; web lib/orca.ts
  // parseOrcaCaps): more is a desktop release first. The most any profile offers is all of them.
```

### Source/UnitTests/Core/Orca/OnlineMenuTest.cpp: OrcaOnlineMenuLobby.APickWithNobodyToLeaveIsAnnounced

```
  // The last friend has gone and the session only idles out its last frames: nobody is left
  // behind, so no "left lobby" and no new room; the session ends a few frames later as ever.
```
```
  // A keyframe kept for an invite nobody took yet (or a prepare-join's) is no player: the pick is
  // announced and that keyframe (made under the header before the pick) goes.
```

### Source/UnitTests/Core/Orca/OnlineMenuTest.cpp: OrcaOnlineMenuLobby.AFriendHomeOnThePicksCharacterSelectTakesThePickToo

```
  // Home on the character select With Anyone > Casual or Ranked opened (its host left the friends
  // lobby at that pick, whoever pressed it): announced, so Start readies it. Whatever its page can
  // search now (no pick cap here): it is alone, and the page answers it as any alone pick (its
  // reason, or the search), never a dead end where Start does nothing.
```
```
  // Any other character select (With Friends', a Versus one opened otherwise) or the main menu.
```
```
  // A queue or search already under way (the reader announced it, alone by then): never twice.
```

### Source/UnitTests/Core/Orca/OnlineMenuTest.cpp: OrcaOnlineMenuReader.APickMadeWithFriendsGoesToTheSessionOnce

```
  // Casual or Ranked picked while friends play: nothing announced, and the session hears of it
  // once, at the boundary that saw it (a host leaves its friends lobby for the queue, then
  // announces it: Rollback/OnlineMatch.cpp).
```
```
  // Never on a re-run; With Friends goes over too (the frame hook keeps it to itself).
```

### Tools/orca/versus-cpu-dropin.py: header

```
# versus-cpu-dropin.py: a friend drops into a host's local Versus character select (Group > Brawl)
# that has CPUs (ORCA.md "No CPUs online": the panels and the join), over a real YouGame dev room,
# headless and muted. Two Orcas at most.
#
#   Tools/orca/versus-cpu-dropin.py <dolphin-emu-nogui> <disc> [--pplus <launcher.dol>]
#
# 1. Host boots to Group > Brawl's character select, makes CPUs of panels 2 and 3 (A on their
#    player-type buttons: nobody's controller is in either), picks, and waits there.
# 2. Friend sends "join <host's room>": the host's keyframe is taken on that select, the friend
#    loads it, catches up and plugs into port 2, whose panel is a CPU's.
# 3. At the plug-in both games empty both CPU panels at the same frame ("Online seats: ... 2
#    panel(s) cleared"), and the friend's panel becomes theirs (kind 1, "a2-kind = 00000001" in
#    both logs at the same frame) with no input from the friend: Orca presses A for port 2 on its
#    own player-type button, where its hand starts (OnlineSeats::PressesJoin).
# 4. The friend leaves; the checksum gate as in online-menu-dropin.py: each side's session end
#    matched checksums with the other (N > 0), and neither logged a desync.
# Fails on a desync, a timeout or an "orca error".
```

### Tools/orca/inputs/bf-reseat.txt: header

```
# A former joiner going home (ORCA.md "Drop-in": Rollback/OnlineMatch.cpp ComeHome), Brawl, for a
# sync test of the move: run with ORCA_UX_TEST_RESEAT_AT=<frame> (the player plays port 2 until that
# frame, port 1 from it; the other reports no controller; the frame hook's ports follow) and
# ORCA_UX_TEST_NAMES=bo. Port 1's rules here drive whichever port the player is on. Set the frame
# about 250 frames into the first character select (from a run with YG_SCENES=1), e.g.
#   ORCA_UX_TEST_RESEAT_AT=1907 YG_SYNCTEST=7 YG_SYNCTEST_FROM=1400 YG_EXIT_AFTER=<menu 3 + 100>
# The ux=18 walk (With Friends goes straight to its character select, and B out of it lands on the
# ONLINE page with With Friends selected): boot -> the main menu -> PLAY ONLINE > With Friends -> the
# character select on port 2: it joins its panel and puts its token on a character -> the move:
# port 2 unplugs, port 1 plugs in -> port 1 joins its panel, puts its token down, holds B (token up,
# then out to the ONLINE page) -> With Anyone > Ranked -> the character select again: join, a
# character, Start, then held B back to the With Anyone page.
```

### Tools/orca/inputs/bf-cpu-friends.txt: header

```
# No CPUs on With Friends' character select (ORCA.md "No CPUs online"), Brawl, no test knob: Group >
# Brawl, a CPU on panel 3 (A on its player-type button), a pick, Start, B on the stage select, B out
# to the top page, PLAY ONLINE > With Friends: its character select (the third). There port 1 tries
# A on panel 4's player-type button (before ux=19, a CPU), then goes up and picks. Brawl's own menu
# already resets the record, so the third select starts with port 1 alone either way; since ux=19
# A on that button does nothing and panels 2-4 are hidden. YG_EXIT_AFTER=2950.
```

### Tools/orca/inputs/pplus-cpu-online.txt: header

```
# Luis's CPU bug (ORCA.md "No CPUs online"), Project+ (ORCA_PROFILE=PPLUS32), no test knob: Group >
# Brawl, port 1's hand onto panel 3's player-type button, A (a CPU), up into the grid, A on a
# character, Start: the stage select (the character select saves its panels into its record there).
# B on the stage select: the character select again, the CPU rebuilt on panel 3 (local Versus keeps
# its CPUs). B held out to Group, B to the top page, PLAY ONLINE > With Anyone > Ranked: its
# character select (the third). There port 1 tries A on panel 4's player-type button, then goes up
# and picks. Before ux=19 that select had the CPU on panel 3 and A made another on panel 4; since,
# its panels are port 1's alone (the record cleared between the scenes, panels 2-4 hidden) and A
# there does nothing. Watch with pplus-cpu-record-probe.txt's watches; YG_EXIT_AFTER=1700.
```

### Tools/orca/inputs/pplus-cpu-record-probe.txt: header

```
# ORCA_UX_PROBE file (ORCA.md "No CPUs online"): Project+'s record of the character select's players
# (gmSelCharData, [0x805A00E0]+0x10; each port +0xB8 + port x 0x5C: the character, then the state,
# 0 a human, 1 a CPU, 3 nobody) as a local Versus game leaves it with CPUs on ports 2 and 3, written
# on the first main menu before anything goes online. Project+ never resets that record, so before
# ux=19 the next With Anyone or With Friends select built those CPUs into its panels. For harness
# runs (with ORCA_TEST_QUEUE=ranked:solo and pplus-queue2-ranked.txt) and two-Orca runs
# (queue-e2e.mjs --probe, dev tickets). The watches log what the select and the fight make of it.
```


# Moved from ORCA_ARCHITECTURE.md (2026-10-06): internal messaging guidance, not public

## Play again after a ranked set (q14, 2026-10-06)

Luis, 2026-10-06 (03:23 UTC, room `6qe4adpj`): "The flow where we finish a ranked match and I click
play again, and that person accepts my invite, seems to fail ... make sure that this flow works
smoothly". ORCA.md "Play again after a ranked set" (under "Drop-in") is the short version; this is
the original write-up and the verify runs.

**Play again after a ranked set** (branch `q14-play-again`; `OnlineMatch.cpp` `EndQueueForFriend`,
`EndQueueForJoin`, `RearmFriendsPick`). Luis, 2026-10-06 (03:23 UTC, room `6qe4adpj`): "The flow
where we finish a ranked match and I click play again, and that person accepts my invite, seems to
fail ... make sure that this flow works smoothly". After a set both games are back on their own
Ranked character select, not ready (the queue image), each hosting a friends room of its own. The
page's Play again (YouGame's Play for fun) invites the opponent into the inviter's room and sends
`prepare-join`; the opponent's page accepts and sends `join <code>`. Through 0.3.26 the join always
failed: the inviter's game still carried the queue's own character select's header
(`FLAG_SOLO | FLAG_QUEUE2`, which `WantedRulesMode` asks for while the queue is active, alone on port
1), its keyframe carried it, and the friend refused it right after loading (`Online rules: ... this
room wants no header (the host's header is its own character select's): mismatch`, `orca error
mismatch`), whoever had left the set's room and whoever had hosted it
(`Tools/orca/queue-e2e.mjs --scenario playagain --variant a|b|c|d`, `--scenario cssjoin --where
ranked`: JOIN-FAILED on 0.3.25 and on `q11-lobby`). Inviting a friend from With Anyone's Casual or
Ranked character select at any other time failed the same way. Now the newest intent wins, as With
Friends does and as a lobby pick does the other way round:
- **The host** (port 1, not joining, its room's welcome said `private`): a friend arriving, or the
  app's `prepare-join` (an invite on its way), while its player is on the queue (its own character
  select, or the app's search) ends the queue and the search at that boundary, before the header is
  called for again, and prints `orca menu cancel` (the page drops its search and the player's
  ready). The header goes back to none at the next boundary (`HeaderFreeAt` allows it with a friend
  waiting; the player's own rules come back), a keyframe made under the old one is dropped
  (`DropStaleKeyframe`), and the friend's keyframe carries none. At `prepare-join` that costs
  nothing; at an arrival with no invite first, one more boundary before the keyframe. The select
  stays the one the pick opened (titled RANKED until the friend plugs in, then FRIENDS: "The
  character select's title"). Never in a room the queue made (its opponent is the match) or before
  a room's welcome (a matched room's `host` just taken).
- **Once the friends are gone** (one came in, all left, and the host's game is alone again on the
  character select the queue's pick opened, a fight in between or not), the pick is armed again
  if its page can search that queue now (its pick cap; `orca menu online ranked`): its queue's own character select, where Start searches, as for the
  friend who went home on that same select (`ComeHome`, `AnnounceHomePick`). Never for an invite
  nobody took yet: the player waits there for the friend, and Start would take them into a search.
- **The friend** whose keyframe loads into a friends room ends its own queue the same way (`orca
  menu cancel` if one was under way) and drops the game it kept for after a queue room (its own
  character select, or the set's room's). Kept, that image was never put back (only a queue room's
  end does that) and stopped the player going home when its host left (`MayComeHome`: `no-room`,
  so its own invites failed), and the queue still active would have put the queue's own character
  select's header on the friend's game once home on port 1. A join that fails before the keyframe
  loads keeps the player's own game, its queue select and all.
- The friend's way in needed nothing new: `join` from anywhere (its own select, or still in the
  set's room as its host or its joiner, on the results screen or the select) leaves that room at a
  clean boundary (`JoinInPlay`; a set already decided is no forfeit) and joins.

Local: what changes is which header this host writes at its own solo boundaries before a keyframe,
and its own process state; nothing a session runs on two machines (no compatibility change).

**Verify (2026-10-06, two Orcas against prod, `Tools/orca/play-again-matrix.sh`, built from
`q14-play-again` merged into rel-0.3.27):** released 0.3.27 fails `css-ranked` and `pa-d`
(JOIN-FAILED: `orca error mismatch ... this room wants no header`). With q14: `css-ranked`, `pa-d`,
`pa-dh`, `pa-a`, `pa-b`, `pa-c`, `pa-d-fl`, `pa-e` all pass: a game played together (129 s of
fight), 156-170 checksums matched on both sides, no mismatched rollbacks, and the leaver hosts its own
room ~1.9 s later (HOME); `pa-d-fl` shows the host re-armed (`orca menu online ranked`). Runs under
`~/ClaudeWorktrees/orca-q14-0327-verify/out/`. The code was then re-applied on main's cleaned style
(branch `q14-play-again-clean`); its own runs are logged below when done.

Workaround for 0.3.27 players: the inviter presses B on the Ranked select (back to the menu) before
pressing Play again; an invite already sent then gets a clean keyframe.

## macOS 15: guest RAM views on top of each other (mac15-sigtrap, 2026-10-07)

**Symptom.** Orca 0.3.26-0.3.28 closed with SIGTRAP on Project+ about 2.2 s after the `frame
boundary hook` line, before its first frame: `Invalid write to 0x000002c8, PC = 0x801e17bc` (then
`0x2dc`, `0x2e4`, `0x2e0`), then `Unknown instruction 00000000 at PC = 00000500, LR = 801e0bcc`, and
the panic became an exit. 13 automatic crash reports from desktop 0.12.36 (an M2 MacBook Air on
macOS 15.7.9, an M1 MacBook Air on 15.6.1) and the first reporter's 9 runs (an M2 Max on 15.6.1).
Every crash came from macOS 15; every Mac known to have played was on macOS 26.

**Cause.** Upstream #14838 (`27804b4e67`, in 2609, so in every Orca release) passes
`MAP_MEM_VM_SHARE` to `mach_make_memory_entry_64` in `Common/MemArenaDarwin.cpp`. On macOS 15 a
`vm_map` of that entry ignores its offset: every view (MEM1 at 0, the locked L1 cache at 32 MB, MEM2
at 32.25 MB, the fastmem views and the rollback aliases) maps arena offset 0, so MEM1, the L1 cache
and MEM2 are the same pages. Project+ loses MEM1 `0x0-0x3FFF`, so the current-thread pointer at
`0x800000E4` is 0 (`OSSleepThread`'s four invalid writes) and the external-interrupt vector at `0x500`
reads as zeros. The likely writer is Brawl's `__LCEnable`, which clears exactly 16 KB of locked cache
with `dcbz_l` just before the first frame (inferred from the size and timing, not traced).
Vanilla Brawl dies sooner, in `ClearMEM2Arena` (`Unknown instruction` at `801d5998`): its MEM2 clear
zeroes its own MEM1 code. macOS 26 maps the offsets correctly, which is why no Mac here showed it.
Upstream hit it too (an Intel Mac on macOS 15; other games on ARM Macs and later macOS) and replaced
the memory entry with `vm_remap` of the arena itself in #14891 (`da76f651ec`, after 2609).

**Fix.** `da76f651ec` cherry-picked unchanged: `CreateView` and `MapInMemoryRegion` `vm_remap` the
arena, and `MapInMemoryRegion` then sets the view's protection. Host-side only; the game's memory is
the same wherever the old mapping worked, so `kCompatVersion` stays. The changed calls run when
views are created or the DBATs change (boot, `__LCEnable`), not on a snapshot load, whose
`DBATUpdated` is skipped while the BATs are unchanged.

**Checked** on the M4 Pro (macOS 26.3) and in the macOS 15.6.1 (24G90) guest of
`~/Documents/brawl-dolphin/mac15-vm` (tart on the same M4; results in its `out/mac15fix/results/`):
- macOS 15.6.1, release builds of the base (`0f423ddba5`) and the fix: the base's Project+ crashes
  exactly as in the players' reports, headless after 6 s and embedded (the app's way: parent window,
  Metal) after 16 s; the base's vanilla Brawl crashes at `801d5998`. The fix reaches the first frame
  (`Frame hook`, `JIT cleared once`) and runs until stopped (45-60 s) every way: Project+ headless,
  embedded, both with `ORCA_TEST_NO_AFP=1`, on the cached interpreter (`ORCA_TEST_CPUCORE=5`), and
  vanilla Brawl. No crash report from the fix.
- A C replay of the arena (the Wii layout and every view): the `MAP_MEM_VM_SHARE` entry fails on
  macOS 15.6.1 (6,177 mismatches; MEM2 writes land in MEM1) and `vm_remap` passes; both pass on 26.3.
- Hashlogs of the default inputs (Brawl to 10800, Project+ to 9000): the fix equals the base on
  macOS 26.3, and the fix on macOS 15.6.1 equals the fix on 26.3, on every frame.
- `YG_SYNCTEST=7`, 0 RAM mismatches: macOS 26.3 Brawl to 10800 (73,451 re-run frames) and Project+
  to 9000 (60,851); macOS 15.6.1 Project+ to 3000 (18,851) and Brawl 1600-4000 (16,751).
- Unit tests `Orca*`: 514 pass, 17 skipped (the live and latency switches), none fail.

Not covered: M1-M3 silicon (the guest runs on the M4; nothing in the fix depends on the chip) and
macOS 15.7 (the guest is 15.6.1; the M2 reporter runs 15.7.9).

## The game's Sound slider in rooms (0.3.32, 2026-10-07)

**Report.** "Music would sometimes not play at all" (tweet, triage `orca-music-sometimes-silent`).
Investigation (lead session, 2026-10-07): Options > Sound's balance at the end labelled MUSIC mutes
all music; in a room it is the host's memory, so the host's slider silenced the joiner (two Orcas
0.3.29, P+: friend RMS 1 against 3500). Dashboard decision `orca-sound-slider-in-rooms` (lock at 50
or clamp 10-90) still proposed when this was built; the lock is the recommended default.

**The page** (Brawl's `sora_menu_main`, text at `0x8116384C`; Project+'s own build at +0x1480, the
Sound page's code and relocations identical): init `0x8117AC28` copies record +0x18 into the page's
+0x66C (u16) and sets the middle pause +0x66E to 20. The input handler `0x8117B2CC` (row 0 the
slider, row 1 SOUND TEST): left held, `cmplwi r0, 100; bge` then at 50 a 20-frame pause, else +1;
right held, `cmpwi r0, 0; beq`, the same pause, -1. Each step calls `0x80076F30` on the sound
manager `[0x805A01D0]` with ((100 - v) / 100, v / 100), which normalises, doubles and clamps at 1.0:
music at +0x714, effects at +0x718, then the stream player and 32 sound players. So the sound
changes live; A or B on the page only stores +0x66C into the record (`0x8117B1AC`, `0x8117B200`) and
sets the record's dirty bit (`0x800557A8`). Other callers of `0x80076F30`: `GameGlobal::init`
(`0x8004C7F0`, 50), the defaults reset (`0x8004E294`, 50), a save's load (`0x8004ECB8`); none in any
other module on the disc, none in Project+'s codeset sources on the SD card.

**Why every session, solo included.** The lead's question offered "solo keeps it". A patch in the
page can't: every Orca is the host a friend can drop into, and the slider's state (record and sound
manager) is RAM, so a balance set while alone reaches the friend with the keyframe; the two-Orca
proof below sets it before the friend joins, the common case (the queue's matched host played alone
first). Keeping it solo would need a synced reset at plug-in: `0x80076F30` with an equal pair (what
`GameGlobal::init` passes: 0.5, 0.5) from game code (a toggled cave on a per-frame path), or Orca
writing nw4r sound state; neither is menu-only. The Music switch covers music off for one player.

**Patch.** Both limits 50 (`28000064 -> 28000032`, `2C000000 -> 2C000032`) and right's `beq -> ble`
(`418200B0 -> 408100B0`), so a value under the limit only comes up to it (vanilla's beq at 0 would
pass a lower start into 65535; no session has one). Two 3-word guarded groups per file. Clamp: left
`2800005A`, right `2C00000A`. No kCompatVersion change of its own (the files are in the lobby key;
0.3.32's one bump is 22). Correction (2026-10-07, evening): 0.3.32 as released is ux=23; 22 was
the v-sync move's, in the unshipped 0.3.31.

**Checked** (dev build of `release-0.3.32`, Mac, all through `run-lock.py check`; scripts and logs
in the lead's scratchpad `rel0332/slider/`):
- RAM on the Sound page before the patch, Brawl Rev 2 and Project+: the six words as above; with it,
  both groups land on entering the main menu (Brawl frame 1441, Project+ 199), the module differs
  from the unpatched dump only in the three words, `0x9017BE68` stays 0x32 while left is held, and
  the menu music plays on (Brawl RMS 3000-3800 a second after the slider against 13 unpatched;
  Project+ 2000-2700 on its menu against 1-400). Brawl Rev 1 (rev1 alias): the same landings and
  sound.
- Two Orcas over yougame.co rooms (`two-orca-balance.py`, now taking Brawl too), the host's slider
  pushed toward MUSIC solo, a friend joining 45 s after boot: shipped 0.3.30, friend silent (Brawl
  RMS 12-20, Project+ 1); 0.3.32, friend 2700-6000 (Brawl), 2900-6100 (Project+). Every run 28-30
  checksums matched both sides, no desync.
- Hashlogs of the default inputs: the build with the files minus the two groups equals 0.3.30
  (Brawl 0-10800, Project+ 0-9000); with the lock it differs from the landing frame on, and
  MEM1+MEM2 at the character select, in the match, on the results screen and the second character
  select differ only in the three words (the freed module memory keeps them in the match).
- `YG_SYNCTEST=7`: the Sound page with the slider held and tapped both ways, then B out (Brawl to
  3400: 21651 re-run frames; Project+ to 2400: 14651), and the default inputs (Brawl to 10800:
  73451; Project+ to 9000: 60851): 0 RAM mismatches.
- `OrcaOnlineMenuSound.TheBalanceSliderStaysInTheMiddleInBothGames` (fails without the groups).

Not run here: the two-Orca perf gate (menu code only, nothing per frame), Windows, the queue path
(`queue-e2e.mjs`; the same drop-in as a friends room).

## Orca 0.3.32 (2026-10-07)

Branch `release-0.3.32s` (the earlier `release-0.3.32`, which holds 0.3.33's game-start work
without the v-sync move, is left alone). On 0.3.31 (`ecb39fb754`, never shipped alone):

- The name-tag commits and the Sound slider lock, cherry-picked from `release-0.3.32`
  (`4f6834dfbe`..`c2ef593d8f`; `4f412350fe`, ux=22, was already in the base).
- `codex/input-replay-protocol` `2d97b2ac07`: a join rebuilds the host's game from the joiner's own
  boot origin and the host's input history (ORCA.md "Drop-in"). `codex/peer-state-containment`
  `ffbb0ae3fa` is superseded and not in it (stacked, no join could succeed).
- kCompatVersion 23 (`orca2:` keys either way).
- Mac and Linux builds: `Keyframe.cpp` and `OnlineMatch.cpp` compile with `-fexceptions` (their
  try/catch is MSVC-only otherwise). The savestate-read hardening in `2d97b2ac07` (ChunkFile,
  State, Fifo, GPFifo, GameFileCache, ChunkFileTest) threw from a header every file includes, so
  those files are back to 0.3.31's on every platform; only snapshots this process captured reach
  the state loader.
- From the two reviews: the rebuild clears the JIT at the profile's `JitClearFrame` (Project+'s
  frame 2) and after returning to the player's own game; the boot origin's capture stops the NAND
  journal; one friend at a time (a second arrival is dropped with `room_full`); a rebuild ended by
  the room or its host reports the room's code or `peer_left`.

Not in it, from the reviews (the lead's call): a host-side cap on how much history it offers (a
long-running host can't be joined within the 3-minute deadline, and the page retries `network`);
a capture failure at the origin stops the boot instead of playing on without drop-in; a retry when
Windows can't delete the NAND folder on the way back; the low items (name cleaning in replays,
128 MB download caps, a wall-clock rebuild watchdog, a `static_assert` on the decode budget).
Server side (YouGame): replay ids and `orca2:` only, before online reopens.

## Orca 0.3.33 (2026-10-08)

Branch `release-0.3.33`, from `release-0.3.32s` (`2a58d1d59e`, 0.3.32 as shipped). Cherry-picked
with `-x`, resolved by hunk:

- From `release-0.3.32` (the old feature branch, built on the unshipped 0.3.31; its name-tag and
  slider commits are already in 0.3.32): the ranked game-start (`d0c8b0e254`, `3c0bc9e5bc`,
  `be0b827c16`, `16bbb50686`: game-start at the first frame with the fighters in; a set left before
  game 1 is void), and the small items `a15d5a140e` (Windows alerts never throw; each C++ exception
  names itself), `0f423ddba5` + `c872898d24` (run-lock: wait out macOS's new-program check; only
  Luis's installed Orca counts as Luis playing; `0f423ddba5` came along so the repo's run-lock is
  the live one), `75aa74ee5c` (release checklist step 0), `43fa9cd712` (boot logs off at the first
  frame, embedded), `d523d584bc` (`--verify` NAND cleanup). On 0.3.32's input-replay joins the
  game-start needed one change: `ResultsTracker::Rebase` (a rebuilt join) takes the rebuilt frame's
  fight, fighters in, as already begun, as it takes the set's games (test added). The only conflict
  was `Results.cpp` (0.3.32's `Rebase` beside the new `Confirm` signature).
- `pc/ranked-css-keep-triggers` `3e6a38a86f`: L and R held at lock-in reach the game on games 2+ of a
  ranked set (Zero Suit Samus, Sheik, Nana-led Ice Climbers), plus the ORCA.md line.
- SDL never opens the GameCube adapter (`SDL_HINT_JOYSTICK_HIDAPI_GAMECUBE` and
  `SDL_HINT_HIDAPI_LIBUSB_GAMECUBE` "0", after the config's hints): on Windows Orca's SDL held the
  Wii U adapter through WinUSB and the app's helper got "Permission ... denied"
  (`gc-adapter-admin-windows`).
- Input delay: no page's `delay N` (or `delay auto`) now means a fixed 3, never the adaptive delay
  (`OnlineMatch.cpp` `DEFAULT_DELAY`). The adaptive code and `Config::input_delay` 2 stay for the
  tests and the loopback harness.
- `orca-publish-public.sh`: Luis's name and handle may appear (Luis, 2026-10-07); everything else in
  the denylist stays. `orca-package-macos.sh`: `-ffile-prefix-map`/`-fdebug-prefix-map` map the
  checkout to `.`, and the packager fails if the executable still names the checkout, build or home
  path (0.3.32's named the checkout 421 times). The Windows release scripts (PC, outside the repo)
  need the same: MSVC has no `-ffile-prefix-map`; `/d1trimfile:<root>\` (for `__FILE__`) and
  `/PDBALTPATH:%_PDB%` (the PDB path in the exe) are the candidates, untested here.
- kCompatVersion 24 (Project+ ranked sets now set `FIGHT_SEEN`; the character order keeps L and R at
  `+0x20E`).

Not in it: a long-session join fix (the after-release measurement had not written its
recommendation when this branch was cut). Tests (Mac, `build`, Release): dolphin-nogui + tests
build with no new warnings (one `-Wsign-compare` in `YouGameRoom.cpp`'s replay notice, from 0.3.32);
Orca* with the discs: 594 run, 577 passed, 17 skipped (OrcaLive 14, OrcaUXLatency 2, OrcaKitDraw 1),
0 failed; `run-lock-test.py` 35/35; headless ranked smoke: Brawl game 1's fighters in at frame 6866
(fight from 6584), Project+ 3486 (from 3346). Harness runs that send no `delay N` (queue-e2e,
xplat.py) now play at a fixed 3: a perf gate against 0.3.32 must give both builds the same delay.

## Orca 0.3.34: fresh starts (2026-10-08, in progress)

Branch `freshboot-0.3.34`, from `release-0.3.33` (`69b97f49d3`). Design and measurements behind it:
the lead's scratchpad `freshboot/design.md` and `rel0332s-runs/longjoin/` (0.3.32's input-replay
join rebuilt about 600 frames a second on the M4 Pro: 25 min of host play took 162-178 s to plug in,
about 40 min passed the 3-minute rebuild limit, slower machines failed at 10-12 min). ORCA.md "Fresh
starts" says what it does (published: shorter joins only, never why peer state can't be loaded).

- **The origin moves to the built main menu** (`DecideOrigin`): the first boundary whose main menu
  has been built (`muMenuMain`, step running, exit code 0, `+0xAC8` set) for 30 frames, captured
  before that boundary's hook. Before it every Orca plays `CanonicalBootPads` (port 1 neutral, 2-4
  unplugged, its own pad read and dropped), `PortsAt` is port 1 with no values, the hook's `alone` and
  the header's `free` are false, and the app's commands wait. A launch joiner runs that boot
  unthrottled and unseen, pumping its join meanwhile, then blocks in `JoinHost` at its origin as it
  did at frame 0. A game whose menu isn't read (`FreshMove::Supported`) keeps frame 0. No origin by
  frame 6,000: no joins either way (`origin_failed`, friends dropped with `refused`).
- **Replay** (`Replay.h`, `Keyframe.cpp`): `first_frame` after the frame count; frames `[first, K)`
  only; a `menu_exit` word per frame (0, 25, 30, 31), only on the first. The decoder refuses
  `first > K`, other exits, an exit anywhere else. Ids stay `replay-<K>-<hash8>`: frames restart at
  the origin's after each fresh start. kCompatVersion 25.
- **Fresh start** (`FreshStart`): `RestoreLocal` (shared with the join's load, the cancelled join's
  way back and the queue image) puts back the origin's image and NAND on a new ring; JIT cleared; the
  replay restarts at M with `menu_exit` on its first frame; the port values restart with this
  player's own from M; unthrottled, unseen and muted (`SetCatchingUp`) through the tail, whose hook
  runs as a re-run playing the replay's events (`FreshMove::Frame` applies the exit). The tail ends
  30 frames after the character select's task is readable (cap 600).
- **Triggers** (`DecideFreshStart`): `host` arms a queue room's, run at the first boundary after the
  welcome names the queue (Hold until then: no keyframe), once per room. A friend at a solo host
  with more than 600 frames of history since its last start (`ORCA_TEST_FRESH_AFTER`), after the
  single-player hold. A `prepare-join` no longer pre-makes a keyframe.
- **Port 1's pick in a queue room**: not the input gate (no new match-block bytes, nothing the joiner
  computes): after the tail the host holds neutral pads until the room's header is in, then
  `Queue::StepOwnPick` steers its own hand from its queue identity with the same integer steering,
  as its own inputs, recorded like any other; done when down on the pick in its colour, at 10 s, or
  when a session starts (the player's pad then). Without `queue2` or a pick it just hands back.
- **NAND restore** writes only differing files and removes the rest (`ReplaceNandTree`).
- **Knobs**: `ORCA_TEST_FRESH=off`, `ORCA_TEST_FRESH_AFTER=<frames>`, `ORCA_TEST_FAST_UNTIL=<frame>`.

Answers taken for the design's open questions (defaults, revisit with Luis): a friend arriving
during a Versus fight against CPUs fresh-starts at once; the friends rules carry is a follow-up; the
in-place threshold is 10 s. `Profile::boot_nand_frame` (`BootNandFrame`) is unused since 0.3.32.

Checks on this branch (Mac, `build`, Release; two-Orca runs on the local rooms stack, dev tickets):

- dolphin-nogui + tests build. `Orca*`: 614 run, 594 passed, 20 skipped (OrcaLive 14, OrcaUXLatency
  2, OrcaKitDraw 1, OrcaDisc 2: this tree's test binary finds no `Sys/Orca` beside its bundle;
  `OrcaKeyframe.HttpStoreAgainstAFakeServer` 1, which passes against `fake_keyframes.py`), 0 failed.
- Origins: Brawl Rev 2 frame 1524 (RAM `f0c74d6dd64230b5`, NAND 37,807 KB, 17-31 ms), Project+ frame
  230 (`0c80f10df0a8be22`, 2,416 KB), the same throttled, unthrottled (launch joiner) and with
  `ORCA_TEST_FAST_UNTIL`. A fresh start restores in 22-126 ms; its tail runs 1524-1588 (Brawl) or
  230-266 (Project+) in about 0.2-0.35 s.
- `long-join.py` (`ORCA_TEST_FAST_UNTIL` hosts): a friend plugs in 1.2 s after `join` at a Brawl
  host at frame 29,573 and 1.7 s at a Project+ host at 37,828; earlier, 1.2 s at 54,287 (15 min)
  and 1.7 s at 50,677. 0.3.32 needed 162-178 s at 25 min.
- A launch join (`ORCA_JOIN`, its boot run unseen to its origin): plugged in 2.3 s.
- `queue-e2e.mjs --q2 --queue ranked`: Brawl 228 checksums each, Project+ 233, each host's pick put
  back in 70 frames, both back on their own select after the set. `--queue casual` fails after
  game 1 ("the character select never went on") on this stack with 0.3.33's build too.
- `online-menu-dropin.py` (in place, `--invite` both cases), `versus-cpu-dropin.py` (18 checksums),
  `dropin-commands.py`: pass. A join held by `ORCA_TEST_HOLD` fresh-starts once the hold ends.
- Harness-only runs (hash logs) take none of this: no room, so no origin, no replay scope and no
  steering; their RAM is unchanged by construction. Not run: Mac<->PC (`xplat.py`), the Windows
  restore time (the perf comparison and the hash logs ran after the reviews, below).

After the three reviews (2bc06a00f6, fcd67d6b57; same Mac, same local stack):

- Fixes: a queue host makes no keyframe while its fresh start runs (tail, hold, its own pick being
  put back), so a join sent at once no longer starts the session mid-steer; the tail's boundaries
  don't count toward the header's limit; `CaptureOrigin` clears the JIT like every other way into
  the origin; a failed fresh start lets whoever waits join in place instead of retrying; a replay
  frame missing below the target fails the join. Test harness (`Harness::Restart`): a restore that
  starts a new timeline makes its frames first passes. Before it, scene lines, results and the
  first-pass hooks stopped until the frame number passed the old timeline's, so queue-e2e kept
  pressing A and Start on a ranked stage select it couldn't see (the perf gate's rollbacks doubled).
- `Orca*`: 614 run, 594 passed, 0 failed (the same 20 skipped). `YG_SYNCTEST=7`: Brawl casual
  22,771 and ranked 10,171 re-run frames, Project+ casual 20,951 and ranked 28,651, 0 RAM
  mismatches. Release step 3 hash logs equal 0.3.33's over every shared frame (Brawl 0-10806,
  Project+ 0-9005).
- `queue-e2e --q2 --queue ranked --join-delay 0 --costume 1` (the page's timing): Brawl and
  Project+ sets 2-0, the host's pick back in costume 1 two frames before port 2 (1668/1670,
  342/344), matched to plugged in 2.9 s, 229 and 233 checksums each side.
- `long-join.py --fast-age`: Brawl hosts at frames 9,571 / 55,820 / 127,805 (2, 15, 35 minutes)
  plugged in 1.1 / 0.9 / 1.4 s, Project+ at 7,706 / 54,493 / 126,499 in 1.8 / 1.6 / 1.7 s; on
  fcd67d6b57 again, 1.2 s (Brawl, 127,764) and 1.6 s (Project+, 132,882); 13 checksums each side,
  no desync. In place (`ORCA_TEST_FRESH=off`) into a fight at a 1-minute host: Brawl 9.2 s (4,976
  frames rebuilt in 7.7 s), Project+ 13.0 s (6,888 in 11.0 s), no desync. `online-menu-dropin.py
  --pplus`, `versus-cpu-dropin.py --pplus` and `online-menu-dropin.py --disc2 <Rev 1>`: pass.
- Perf (`gate-0334fb`, the gate-0333 method: a Project+ ranked set, delay 3, 40 +- 8 ms, Metal;
  both builds unsigned, the same queue-e2e): 0.3.33 (B1-B4) against this branch (C1-C4). Frame
  numbers restart at the origin, so "at frame 14400" misses the end of this branch's set; compared
  over each session instead. Joiner, whole set: slow 21.0 / 22.5, hitches here 0.8 / 0.0, stalls
  12.8 / 11.0, rollbacks 28.8 / 28.5, re-run frames 81.8 / 80.2, snapshot 0.85 / 0.82 ms. Host,
  last periodic line (about 12,300 frames in): slow 15.8 / 16.8, stalls 0.2 / 0, rollbacks 25.2 /
  26.0. Within run-to-run spread: no regression. (B3 and B4 were marked tainted by external-SSD
  stalls; the gate read the disc from the internal disk.)
- Still not run: Mac<->PC (`xplat.py`), the Windows freeze and the origin hitch on the PC.

## Orca 0.3.35 (2026-10-08)

Branch `release-0.3.35`, from `release-0.3.34` (`9f262a506b`, 0.3.34 as shipped). Two side branches
off that same commit, merged with `--no-ff` (no conflicts: they touch different files):

- `orca-active` (`c3866a3771`, `ffb48d7a56`): the app gets `orca active <interval_s> <active_s>
  <inputs> <mode> <pads>` every 60 s while a game runs, and once more when it stops (ORCA.md
  "Activity lines"). Embedded, the first comes after `ready`. Observation only: on its branch the
  Brawl and Project+ hash logs equal 0.3.34's (`4474f0d28c16`, `d8801b31d2a5`). Today's desktop app
  logs the line and does nothing else with it; counting play from it is a YouGame change. Not yet
  compiled on Windows (MSVC `/WX`). Review 2 asked for the two-Orca perf gate on the release that
  carries it (the frame hook and the local pad path read for it).
- `orca-swapchain-resize-0334` (`781ada5721`): on Windows, when D3D11 or D3D12 `ResizeBuffers`
  can't make the new back buffers, Orca skips the picture and asks for the resize again instead of
  the assert in `CreateSwapChainBuffers` that ended it with exit `0x80000003` (four field crashes on
  0.3.30-0.3.34, three players' machines). stderr names the window size and the HRESULT once per
  change. Checked on the PC: the released 0.3.34 told `rect 0 0 20000 400` exits 2147483651; the
  branch's build keeps playing, the picture comes back, it quits cleanly. ORCA.md "Windows window
  resizes".
- **kCompatVersion stays 25.** ORCA.md: only what changes what the game computes bumps it. Neither
  branch does: the activity reader reads local pads and the wall clock and writes nothing that
  reaches emulated memory, the replay, checksums or the key, and the swap-chain change is the D3D
  renderer's presentation (a skipped picture changes no emulated state; re-runs draw nothing
  anyway). Outside `Source/Core/VideoBackends/D3D*` (not built on macOS) the merged tree's code is
  `orca-active`'s, so its hash logs carry over to the Mac build. The key carries the build's git
  revision anyway, so 0.3.34 and 0.3.35 never meet.
- ORCA.md: the compatibility line said `ux=24`; it has been 25 since 0.3.34.

Checks (Mac, worktree `orca-rel-0335`, `build`, Release, `orca-cmake-flags.txt` + tests):

- dolphin-nogui + tests build from clean (ccache): the same 13 warnings as 0.3.34's full build, none
  new (Orca's only one is `YouGameRoom.cpp:3076`'s `-Wsign-compare`). The binary says
  `Dolphin [release-0.3.35] 2609-623`.
- `Orca*` with the fake keyframe server and both Brawl discs: 640 run, 621 passed, 19 skipped
  (OrcaLive 14, OrcaUXLatency 2, OrcaKitDraw 1, OrcaDisc 2: no `Sys/Orca` beside this tree's test
  binary), 0 failed; `OrcaActivity` 26/26.

Not run here (the release's own steps): hash logs and synctests on this build, the two-Orca perf
gate against 0.3.34, the Windows build (the first MSVC compile of `Activity.cpp`) and its gtests,
the swap-chain repro on the release bits, Mac <-> PC.

## Random gave the same characters every match (orca-random-seed-0335, 2026-10-09)

Report (decepticon, P+, 0.3.32, 2026-10-08): "I almost always get sonic first. And I get
dedede/yoshi/luigi all the time" (dashboard `orca-random-same-characters`).

**Cause, proven under the harness with probe watches and writes:**

- Two `mtRand` generators: `g_mtRandDefault` (`0x805A00B8`, seed `+4`) and a second at `0x805A0420`
  (seed `+4`), same vtable `0x8042AE50`, generate `0x8003FAC4` (`(s * 0x41C64E6D + 12345) &
  0x7FFFFFFF`), randf `0x8003FAF8`, randi `0x8003FC7C` (the default one).
- The character select draws each panel's RANDOM as it starts from the second one (12 draws: CSS
  frame 2 in P+, about 30 in Brawl; `0x806857F0`, `randf(0x805A0420) * 42` into P+'s table at
  `0x80680E80`, area i's character is draw 3i). A on RANDOM copies that draw into gmGlobalModeMelee
  (no draw at the press; picking RANDOM again in the same visit gives the same character).
- Writing `0x805A00BC` on the main menu or the select changed nothing; writing `0x805A0424` before
  the select changed RANDOM (P+ Roy to another, Brawl `0x25` to `0x03`). A longer main menu, A
  7-65 frames after hovering, Start 200-300: the same seeds and character. The menus' generator
  moves only at select entries and about every 5 s of a fight.
- The pinned clock and scripted boot make it `207d5b71` at every origin (app path; `207d5b25`
  offline). With 0.3.34's fresh starts every queue room starts there.

**On 0.3.35** (queue-e2e `--q2 --queue ranked --pick random`, rooms mur7guyk and 8xkmpur7): both
rooms, both Orcas: the menus' generator `22537565` after the select's draws, game 1 host R.O.B.
(`0x26`), joiner King Dedede (`0x23`), game 2 `0x03`/`0x26`. `g_mtRandDefault` at game 1's fight
start was the same too (`01aa7b00`).

**Fix:** a seed per history ([ORCA.md "Random numbers"](ORCA.md#random-numbers)):
`ReplayFrame::seed` on the first frame (ORP2), drawn by the host at every boot origin and fresh
start, written into both generators by `FreshMove::ApplySeed` at that frame's hook. ux 26.

**Checks** (Mac, Release, this branch):

- Build: only the known `YouGameRoom.cpp:3076` warning. `Orca*`: 643 run, 623 passed, 20 skipped
  (OrcaLive, OrcaDisc, the HTTP store, UXLatency, KitDraw), 0 failed; new tests
  `OrcaKeyframe.AHistorysSeedTravelsOnItsFirstFrameOnly`,
  `OrcaOnlineMenuFresh.AHistorysSeedStartsBothRandomNumberGenerators`,
  `OrcaOnlineMenuFresh.TheCharacterSelectsRandomDrawsDependOnTheSeed`.
- queue-e2e P+ ranked `--pick random`, twice: PASS, 227 checksums matched on each side, 0
  rollbacks; characters 41/42, 42/40 and 23/0, 15/40 (0.3.35: 38/35, 3/38 both times). Every
  origin's RAM hash is still `0c80f10df0a8be22`; the joiner's rebuild applied the host's seed.
- dropin-commands.py P+: PASS, and with `ORCA_TEST_FRESH=off` (joins from the host's boot origin,
  its CaptureOrigin seed): PASS; the friend rebuilt with the host's seed both times.
- Offline hash log (pplus-1v1 to 4000): identical to 0.3.35 (no room, no seed).

Not run: Mac<->PC (`xplat.py`), Brawl two-Orca, the perf gate. The change writes two words at one
frame, so no perf effect is expected.

## Name tags typed in the game (branch tags-width-0335, 2026-10-09)

**Reports** (site comments, Oct 7-9, after 0.3.30-0.3.35): "changing controls inside the game
(notedly turning off tap jump) literally just doesn't work"; a step-by-step one: Options > Controls,
make a tag named like their username, tap jump and C-stick changed, Casual or Ranked, "controls
return to default settings", and the Controls list then shows that name twice.

**Cause, proven with the game's own menus** (shipped 0.3.35 bytes, Brawl rev 2, headless,
`YG_INPUT` through Options > Controls > New entry, the keypad, the GameCube page and Save):
- The name list types **full-width** characters: "ADGJ" is `FF21 FF24 FF27 FF2A` in the tag; the
  same in Project+. `BrawlTag` and `MakeTag` write ASCII (`0041 ...`), and `FindTag` compared code
  points, so a player's own tag never matched their username.
- In a dev game's room named `sandbox-ada`: the player's `ＳＡＮＤＢ` (slot 0, tap jump off) stays
  as saved; on the character select Orca makes `SANDB` in slot 119 at the defaults and port 1 wears
  it (tap jump on). No `orca controls` line and no kept file: `ReadOwnControls` found neither the
  worn tag nor, off the select, a tag named `SANDB`, so nothing reached the page or the account,
  and online rooms carried nothing. The Names list then shows SANDB twice.
- Save on the GameCube page writes the flag byte as 0x01 with tap jump off and 0x81 with it on: 0x80
  is tap jump, 0x01 a "saved" mark, not tap jump. In a match (Mario, up held after GO) a worn tag
  with 0x80 or 0x81 jumps (status 0xA, 0xB) and one with 0x01 or 0x00 does not, so the tag
  mechanism and Orca's clamped profiles do apply tap jump; only the tag choice was wrong.

**Change.** Names match as they read: full-width forms fold to ASCII and letters to one case
(`SameName`). `FindPlayerTag` returns a tag the player typed that reads as the name before the one
Orca made; `ApplyNameTags` uses it for a port's name and its variant, and `ReadOwnControls` uses it
for the own-name fallback and for a last-worn tag that reads as the own name (so a tag typed after
wearing Orca's is the one read). On the character select the read takes the worn slot itself.
`WriteControls` keeps a flag byte's bits a profile doesn't carry (the 0x01 Save mark). Nothing else.

**Checked** (dev build of the branch, Mac, every run through `run-lock.py check`):
- `OrcaUXNameTags` 22/22, two new tests that fail on 0.3.35's `NameTags.cpp`
  (`ATagThePlayerTypedInTheGameIsTheirs`, `ControlsSavedInTheGamesMenusArePublishedThere`);
  `Orca*` 642 run, 623 passed, 19 skipped (OrcaLive, OrcaDisc, OrcaUXLatency, OrcaKitDraw), 0 failed.
- The same menus in a dev room: `orca controls ...00...` (tap jump off) printed and kept right after
  Save, before any character select; port 1 wears slot 0; no slot-119 tag; one SANDB in the list.
- Restart on that user folder, the tag typed again and left at the defaults: "own controls from an
  earlier run", no change published for the new tag, the character select writes the kept controls
  into it and port 1 wears it.
- `YG_SYNCTEST=7` from 2300 to 6000 with `ORCA_UX_TEST_NAMES=sandbox-ada,sandbox-bo` over the
  typed tag, its Save, the character select (port 1 in the typed tag, port 2 SAND2), the stage
  select and a match: 0 RAM mismatches (PASS); up held in the match: no jump.

**Release notes.** Remote ports' tags can differ from 0.3.35's in a session whose memory holds a
typed tag of a player's name (a With Friends host's own), so the release that takes this needs its
kCompatVersion bump. The writes are still a pure function of memory and the synced ports, the same
on every machine whoever is remote. The frame hook gains one folded scan of the 120 tags per joined
port on the character select and one per frame while alone: run the two-Orca perf gate as usual.
Known edges (review): in a With Friends host's memory a remote port whose 5-letter tag reads as a typed tag takes it (a port without controls then plays that layout; a same-prefix joiner whose port joins before the host's can take the host's typed tag, as it could Orca's ASCII one before); off the select a player who picked Orca's ASCII copy over their typed one has the typed one read. Not clang-formatted (no clang-format on this Mac). Not covered: tags kept between runs (still
`orca-keep-own-tags`), Windows, a real two-Orca room with a typed tag.

## Explaining it to people (internal)

**One sentence:**

> "Rollback means rewinding the console a few frames and replaying them. We rewind all of it, at
> one exact moment, so a replayed frame comes out identical to the original."

**Thirty seconds:**

> "When your opponent's input arrives late, the game rewinds a few frames and replays them with the
> right input. That only works if the replay is identical to the original. Earlier approaches for
> Brawl rewound most of the console and patched the gaps with hooks inside the game. We rewind the
> whole console at one exact point, and we test it by rewinding every frame of full matches and
> checking memory byte for byte. Once that passes, the game needs no changes at all."

**An analogy:**

> "Imagine rewinding a chess game. One approach puts the pieces back but leaves the clocks and the
> scoresheet as they were, then adds house rules to cover the mismatches. We put back the pieces,
> the clocks and the scoresheet, so no house rules are needed."

**If someone asks what the new idea was:**

> "There wasn't a new technique. Restoring the whole machine exactly is the textbook way to do
> rollback in an emulator. The difference was testing: we made the emulator prove a rewound frame is
> byte-for-byte identical before writing any netcode. That turned vague desyncs into specific bugs
> we could fix."

**What not to say:** that Brawlback "did it wrong", or that Orca is faster per frame. Brawlback's
research informed the experiments that led to Orca's design, and we haven't measured the speed
claim.

## Orca 0.3.36 (2026-10-09)

Branch `security-0.3.36`, from `release-0.3.35` (`ae0bd8a41f`, 0.3.35 as shipped). A security
release: nothing new for players.

- The four security fixes of upstream Dolphin's `2609a` hotfix, cherry-picked with `-x` (all
  clean): `a5c88ec00e` (NetPlay: oversized input buffer messages), `c129fd7aac` (assembler: malformed
  source), `13817559eb` (NetPlay: oversized Wii save banners), `2eea927e95` (VideoCommon: restored
  FIFO pointers are validated). Orca's nogui build constructs no Dolphin NetPlay client or server and
  has no debugger UI; the FIFO check also runs on rollback's own snapshot loads, so the synctests
  cover it. The rest of `2609a` is Android, RetroAchievements, the version bump, and the
  MemArenaDarwin fix Orca has carried since 0.3.30.
- Drop-in hardening: tighter checks on the host's roster and history for a joining player
  (`Session::ApplyRoster`, `Session::ReceiveHistory`). Its own seat is plugged in once, only after
  it holds the history up to its live frame; its live frame is named once; its unplug frame only
  comes earlier; history stops before its live frame and its plug-in and stays within
  `HISTORY_AHEAD` of the frame it is at. Tests `OrcaDropIn.HistoryPastTheJoinersLiveFrameEndsTheSession`,
  `RosterThatTakesBackTheJoinersSeatEndsTheSession`, `PlugInBeforeTheJoinerHoldsItsHistoryEndsTheSession`,
  `JoinersUnplugOnlyComesEarlier`, `PlugInAsTheFirstRosterTheJoinerHears` (an honest host whose
  plug-in is the first roster the joiner hears), `HistoryAheadOfTheJoinerIsBounded`.
- **kCompatVersion stays 25.** Neither change alters what the game computes: the FIFO check only
  refuses a state whose pointers fall outside the buffer, and the drop-in checks change nothing a
  session computes. The key carries the build's git revision, so 0.3.35 and 0.3.36 never
  meet.

Checks (Mac, worktree `orca-rel-0336`, `build`, Release, `orca-cmake-flags.txt` + tests):

- dolphin-nogui + tests build: the same warnings as 0.3.35's build (Orca's only one is
  `YouGameRoom.cpp:3076`'s `-Wsign-compare`), none new. The binary says
  `Dolphin [security-0.3.36] 2609-629`.
- `Orca*` with the fake keyframe server and both Brawl discs: 646 run, 627 passed, 19 skipped (the
  same 19 as 0.3.35), 0 failed. The new drop-in tests that end a session fail on 0.3.35's
  `Session.cpp`; `PlugInAsTheFirstRosterTheJoinerHears` passes on both.

The release's own steps (hash logs and synctests against 0.3.35, the two-Orca perf gate, the
Windows build and its gtests, Mac <-> PC) are recorded with the release.

## Orca 0.3.37 (2026-10-09)

Branch `release-0.3.37`, from `security-0.3.36` (`3b7cc5517d`, 0.3.36 as shipped). Four side branches,
each off `release-0.3.35`, merged with `--no-ff`:

- `music-loop-0335` (`4d90601c87`): with the Music switch off, a stream voice stays music through its
  song's loop (the game turns music voices into normal voices at each loop, so music played for
  0.5-5 s per loop). Clean merge.
- `orca-rejoin-seat-0335` (`04b32e5fcf`, `ee73c12f8e`): a ranked joiner back in its old seat after a
  dropped connection skips the host's roster entries from the earlier stay until the host takes it in
  (bug reports 6afb0948, 22ba6446, 96e56887). Conflict in `Session::ApplyRoster` with 0.3.36's own-seat
  checks, resolved by intent: the skip runs first, so an earlier stay's entry can neither set
  `m_own_live_from` nor trip the plug-in check (under 0.3.36's checks such an entry would have set
  `m_own_live_from` to the old live frame, and the admitting roster would then end the session). The
  skip's "not taken in yet" test reads `m_own_live_from` instead of the seat plan's `live_from`: the
  same until the host names the live frame (a joiner's plan starts with its own seat empty, and
  `m_own_live_from` is set whenever the plan's is, and never reset), and after that no roster change to
  the own seat is skipped, a later drop by the host included.
- `orca-random-seed-0335` (`e76a92dae2`, `4abe7663f7`): every history starts from a seed of its own
  (ORCA.md "Random numbers"). Clean merge.
- `tags-width-0335` (`ea1935aa68`): a name tag the player typed in the game is theirs (full-width
  names fold to ASCII, one case). Conflict in this file only.
- **kCompatVersion 25 -> 26, one bump for both changes that alter what the game computes:** the seed
  (both generators' seed words written at a history's first frame) and typed name tags (the session's
  writes differ whenever memory holds a typed tag of a player's name). The music and rejoin changes
  alter neither. The key also carries the git revision, so 0.3.36 and 0.3.37 never meet anyway.
- clang-format 19.1.7 over every line changed since 0.3.36: only the seed's lines needed it (wrapping,
  include order); the name-tag lines were already clean. Then 0 lines to change.
- `Tools/orca-publish-public.sh` names `github.com/yougame-co/orca-netplay` (the repos moved to the
  `yougame-co` org on 2026-10-09; cherry-picked from main `95a392c04e`).

Checks (Mac, worktree `orca-rel-0337`, `build`, Release, `orca-cmake-flags.txt` + tests):

- dolphin-nogui + tests from clean (ccache): the same warnings as 0.3.36's build, none new. The binary
  says `Dolphin [release-0.3.37] 2609-646`.
- `Orca*` with the fake keyframe server and both Brawl discs: 656 run, 637 passed, 19 skipped (the same
  19 as 0.3.36: OrcaLive 14, OrcaDisc 2, OrcaUXLatency 2, OrcaKitDraw 1), 0 failed. The ten new tests
  pass: OrcaMusic (4), the seed's OrcaKeyframe (1) and OrcaOnlineMenuFresh (2),
  `OrcaDropIn.FriendBackInTheSameSeatAfterADrop`, OrcaUXNameTags (2).

Not run here (the release's own steps): hash logs and synctests on this build, the two-Orca perf gate
against 0.3.36, the Windows build and its gtests, Mac <-> PC (the seed and the tags both need it), a
real two-Orca room with a typed tag.

## Orca 0.3.38 (2026-10-09)

Branch `release-0.3.38`, from `release-0.3.37` (`8a053f2cee`, 0.3.37 as shipped).

- `match-latency-orca-0337` (`cdd51eccd5`, `1f28cd4cf2`, `dfcea6d4a6`, `4d4f9ac3cf`), merged with
  `--no-ff`, clean: a queue host's own pick is restored unseen and unthrottled (up to 120 frames), a
  replay of at most 32 KiB travels inside the `kf` room message (the keyframe store stays the
  fallback for bigger ones and for `ORCA_TEST_INLINE=off`), and a matched joiner shows "Opponent found
  · joining <host>…" until the replay loads; the match start timing tools (never published). Merged,
  not rebased, so its "(WIP)" subject stays and the shas its gates and the PC build used stay valid.
  Its numbers on prod with the rooms/site change of YouGame PR #667: casual Brawl matched -> both in
  control 3.21 / 3.48 s p50 / p90, against 5.47 / 5.87 s on 0.3.37; the store's `not_seated` retry
  (about 1 in 10 matches since #667) goes away with the inline replay.
- Linux, `cherry-pick -x` of the `linux-x11-embed` commits and the GCC 12 fix only (`38105499b4`,
  `bf0fa8549e`, `df8b0b6c02`, `80cfa63190`), not `linux-alpha-0335`'s merge into 0.3.35 nor the main
  commits `linux-x11-embed` sits on (`683291a33c` Contributing.md, run-lock): all clean, and every
  Linux file is byte-identical to `linux-alpha-0335`'s. X11-only code (`PlatformX11.cpp`,
  `GLX11Window.cpp`, `XInput2.cpp`), the `Embed.h`/CMake hooks for it, `Tools/orca-package-linux.sh`;
  inert on macOS and Windows.
- Upstream Dolphin, `cherry-pick -x`, all clean: `622c711ccf` (a crash in `std::locale{""}` with an
  invalid `LC_ALL`; adds `Common/LocaleUtil`), `910eeffc08` (unsigned counters in
  `VideoCommon/Statistics`, no signed overflow), and upstream PR #14772 whole, the EXI microphone
  series `afee1e8ae4`..`cee5888137` (8 commits, the sanity checks `e02e385b16` among them). Sessions
  force every EXI slot to None, so the microphone code never runs in one. Picked straight onto the
  release branch rather than with `upstream-sync.py start`, whose sync worktree holds another run's
  `upstream-sync-20261009`.
- clang-format 19.1.7 over every line changed since 0.3.37: four comments in `PlatformX11.cpp` were
  wrapped one column late; then 0 lines to change.
- **kCompatVersion stays 26.** A bump is for a change to what the game computes, and none of these
  makes one: match start runs the same inputs at the same frames (its branch's hash logs equal
  0.3.37's, `YG_SYNCTEST=7` the same re-run counts), the inline replay is the same ORP2 bytes the store
  carried, the Linux code is windowing and packaging, the upstream picks touch the locale, the
  statistics counters and the (unused) microphone. The join protocol changes, but 0.3.37 and 0.3.38
  never meet: the compatibility key starts with the build's git commit (`git rev-parse HEAD`), so they
  are never matched, and in a friends room a hello with another key is ignored (the joiner ends with
  `room_mismatch`). So every platform's build (macOS, Windows, Linux) has to come from a git checkout
  of the release commit. Replay format: ORP2, unchanged.

Checks (Mac, worktree `orca-rel-0338`, `build`, Release, `orca-cmake-flags.txt` + tests, at `b8d692f3e3`):

- dolphin-nogui + tests (ccache): 0.3.37's warnings minus one: the `YouGameRoom.cpp` sign-compare is
  gone (the replay offer check moved into `KeyframeOfferFromJson` with a cast); none new. The binary
  says `Dolphin [release-0.3.38] 2609-668`.
- `Orca*` with the fake keyframe server and both Brawl discs: 663 run, 644 passed, 19 skipped (the
  same 19 as 0.3.37), 0 failed. The seven new tests pass: OrcaFreshStart (3), OrcaKeyframe (3, the
  inline replay), `OrcaMatchmakingMenu.AMatchedJoinerSaysTheOpponentWasFound`.

Not run here (the release's own steps): hash logs and synctests on this build, the two-Orca perf gate
against 0.3.37, the Windows build and its gtests, Mac <-> PC, the Linux build in the VM (GCC 12 has
not yet compiled 0.3.36-0.3.38's code), the packaged app's bundle version.
