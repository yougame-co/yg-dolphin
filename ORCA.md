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
| `ORCA_TEST_COMMANDS` | Tests only: `1` takes the embed protocol's stdin commands without embedding, `test-drop-room` (the room's connection drops once, as a network failure would) and `test-direct off\|on\|in\|out\|rebuild` (direct links drop every datagram, only incoming or outgoing ones, none again, or are built anew; "Direct links"). |
| `ORCA_DIRECT` | `0`: no direct links, the relay only (the hello stops offering them, so the friend's Orca uses the relay too). Default: on. |
| `ORCA_DIRECT_FORCE` | Tests only: `turn` offers and accepts relayed candidates only (a link through Cloudflare TURN); `srflx` nothing on a host interface. |
| `ORCA_DIRECT_LOOPBACK` | Tests only: `1` also offers loopback candidates, for two Orcas on one machine whose LAN address refuses a send to itself. |
| `ORCA_DIRECT_LOSS`, `ORCA_DIRECT_BLOCK` | Tests only: drop this percentage of the datagrams sent; `1` starts with every datagram dropped (as `test-direct off`). |
| `ORCA_NAME` | Name shown in the room. |
| `ORCA_TEST_DEV_GAME` | Tests only: an anonymous dev ticket from the site instead of the app (no ratings, no account). |
| `ORCA_SITE` | Tests only: the site for dev tickets (default `https://yougame.co`). |
| `ORCA_TEST_PADS`, `ORCA_TEST_SKIP_RENDER` | Harness overrides; they change the compatibility key. |
| `ORCA_TEST_FULL_SNAPSHOTS` | `1`: rollback snapshots copy all of RAM instead of copy-on-write (`Source/Core/Core/Rollback/Cow.h`), to compare the two. A test override too. |
| `ORCA_THREAD_QOS` | `0`: no interactive scheduling for the CPU, room and controller-stream threads ("Input latency"); `1`: also for a scripted harness run's CPU thread. |
| `ORCA_TEST_PRESENT` | Tests only, headless (`-p headless`): `WxH` presents into an offscreen image of that size, the OSD and Orca's overlay on it, and screenshots (`YG_SHOT_EVERY`) and frame dumps take that image instead of the bare frame. Presenting then waits for its present time like a window does (headless normally presents nothing). Each screenshot's readback is a frame of its own ~13 ms longer, which the frame meter shows. Not with the Software renderer (it draws no UI); the compatibility key is unchanged. |
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
host leaving) and `Tools/orca/dropin-rooms.py <nogui> <disc>` (a room with no host, a friend first
in the host's room, the host's connection dropping mid-match).

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
module unchanged), so a session with no save goes straight to character select at frame 150 and
writes no save; answering Yes cost 13 s of save writing (character select at frame ~925). None of a
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

Not yet supported: a GameCube controller adapter as the local controller (the local pad is the host's
port 1 as mapped in Dolphin's controller settings).

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
     friend stops after its inputs before L are acknowledged.
   - A friend whose connection drops is unplugged by the host after its last input, rolling back
     the frames that guessed otherwise.
   - A friend who stops answering is dropped after 10 s.
   - Why a friend went reaches both sides:
     - The host prints `orca state friend-left <left|desync|stalled|network>` and keeps playing.
     - A friend whose own session saw a desync leaves with `bye desync`.
     - A friend the host unplugs for a desync (or for going quiet) gets a `drop` message and
       ends with `orca error desync` (or `peer_left`).
     - A friend whose host left ends with `peer_left`, or with `desync` when the host's bye
       said so.
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
     host did, or the session failed) has no room of its own until seats map to ports, and prints
     `orca state no-room` once, after the room it left has told the page it is empty (and never
     mirrors that room again). The app can still send `join`.
   - A friend that leaves while it still catches up (never plugged in) just leaves the room at
     once; the host drops the seat (`friend-left left`).

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
    subnet (a macOS Local Network permission granted late).
  - Rebuilds for a failure back off 2, 4, 8 ... 30 s, and the backoff resets after 30 s up. The
    answering side asks for one with `dlr`; the offering side builds it (not twice in a second).
  - Each build is a new generation with fresh keys. TURN credentials older than 10 minutes are
    minted again first (`YouGameRoom::FreshTicket`): a friend may arrive long after the room
    opened.
- **Three or four players:** a link per pair, each falling back on its own.
- **Drop-in:** linking starts at the hello, in the lobby. A joiner's history comes through the
  relay, and its live inputs go direct as soon as the link is up.
- **Logs** never carry addresses, candidates or credentials: libjuice's own lines go to the
  NETPLAY log (never stdout, which the app parses) with addresses taken out.
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
  players, blocks, the test delay).
- `OrcaSessionDirect*` (duplicates and reordering in the session, a drop-in friend and a
  wall-clock match beside the relay).
- `ORCA_LIVE_TEST=1` `OrcaLive.DirectLinkBetweenTwoRooms` (two rooms on yougame.co).
- `Tools/orca/direct-link.py <nogui> <disc>`: two Orcas over a dev room, a match, `test-direct`
  off/in/on and three rebuilds. `--both ORCA_DIRECT_FORCE=turn --expect turn` and similar run the
  variants. `Tools/orca/xplat.py` passes the `ORCA_DIRECT*` switches through and reports the
  transport.

## Input delay

`Orca/Session/Session.cpp` ("Input delay") has the rule; it changed in 0.3.5.

- Two frames by default, like Slippi. Rollback hides the rest of the link, spikes included.
- The adaptive delay rises only for what rollback can't cover:
  - **The typical link.** It is the median of the player's latest 15 round trips to the relay (one a
    second) and the slowest friend's median, which each reports. Through the relay, one way is half
    of each. The delay covers what the 7-frame rollback window leaves of that, keeping a frame for
    the boundary and one for jitter: two frames cover a round trip of about 230 ms between the
    players. A new session takes the room's recent round trips at once, and the delay rises as soon
    as every player's median is in. One side's alone sets nothing.
  - **Sustained stalls on a friend's side.** A friend stalls when this player's inputs outran its
    rollback window, so only this player's delay can help. Three such stalls in the last 10
    one-second windows raise it a frame; one second counts two at most, and the first 10 s of a
    session need four. A friend's stall counts only if its lag on this player's inputs (below)
    reached the window around then: with three or four players, it may have waited on someone
    else. Stalls don't count while a controller isn't plugged in yet (catching up), around a hitch
    on either machine, or while waiting for the first packet.
- Rollbacks never move the delay, however many there are.
- A raised delay steps down a frame at a time, and only if every friend would still have had a
  frame of room. Each packet shows how far the friend ran past this player's newest input it had
  (its frame minus its acknowledgement); in the last 10 s that may not have reached the window
  minus one. A delay the link raised follows the link down after 3 s of wanting less (a first
  median inflated by a keyframe's transfer). One that stalls raised waits for 10 stall-free
  seconds per step. Stalls back within 20 s of a step undo it and double the next wait.
- The app's `delay N` fixes it instead (Embedding, stdin).
- Since the SI relatch ("Input latency"), a pad sampled for frame f is what the game reads in frame
  f, so delay N is N frames, as in Slippi. Before it the game read each frame's pads a frame late,
  and delay 2 acted like 3.
- Why (2026-10-02, a Project+ match): a Mac at 34-35 ms whose Wi-Fi spiked to 113-123 ms every
  ~11 s, against a friend at 22 ms whose game hitched about as often. In menus both sat at 2 with
  no rollbacks. In every match the old rule climbed to 4-5:
  - A second with 30 re-run frames, or two stalls in three seconds, on either side raised both
    players. The stalls came from the spikes and from the friend's hitches.
  - A lower needed ten seconds of at most three re-run frames per second. A match never has those,
    so the delay came down only between matches.
  - At 4-5 the rollbacks barely moved: 20-30 per 15 s, 3-4 frames deep, against 59 at 2.

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

## Online menu

Luis's rules: Play Online works in both games, through the game's own menus and never a new
screen; no Nintendo server is ever asked for anything; nothing about connecting looks like
Nintendo's. What this build does (v1: the menus and the events; the search window, `host <code>`
and the matched room come later):

- **The top page.** Brawl's Wi-Fi button reads PLAY ONLINE (a texture, below) and can be selected
  again; Project+'s already reads PLAY ONLINE. The footer under it says "Play different modes
  online." in both games.
- **A opens the game's own ONLINE page** (With Friends / With Anyone), with no connection window:
  no save check ("There is no save data" in Project+), no NAND access, no network interface task.
  B goes straight back to the top page, with no "Disconnect from Nintendo WFC?" window. Options
  (WiiConnect24) and Spectator never show. Brawl's page title reads ONLINE.
- **With Anyone** shows CASUAL and RANKED (textures, both games, with their descriptions
  rewritten). Either goes to the local Versus character select and Orca prints
  `orca menu online casual local` or `orca menu online ranked local`.
- **With Friends**: each of its buttons goes to the local Versus character select, where friends
  drop in ("Drop-in"), and Orca prints `orca menu online friends`. Their descriptions say so (a
  brawl, or in Project+ a fight, with the friends you invite, 2-4 players); none promises a roster
  or a mode that isn't there.

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
  - The ONLINE page's Options (`muProcOptWifi`, WiiConnect24 jobs) and With Anyone's Spectator are
    never enabled (`isEnableButton` answers no; they also stay hidden without a connection).
  - sqMenuMain's exit table: codes 24-27 (With Friends), 29 (Spectator), 30 (Casual, was Basic
    Brawl / Basic Versus) and 31 (Ranked, was Team Battle) all go to sqVsMelee, the local Versus
    character select. No online scene, network thread or DWC is ever entered. The menu's exit code
    itself is left as the game wrote it, and Orca reads it.
  - The IOS refusals of every session stay as the backstop: NET_KD_REQ, IP/Top and SSL refuse
    in a session and each refusal is logged at INFO (`IOS_WC24`, `IOS_NET`, `IOS_SSL`).
- **Text** (`MenuText.h`): the main menu's message files, found through the menu scene's archive
  (`mu_menumain_en`, MiscData 5 and 10), are rewritten in emulated memory: Brawl's footer ("...
  while connected to Nintendo WFC."), Project+'s "(Discontinued)" and "(Project M not compatible!)",
  With Anyone's descriptions (Casual, Ranked), With Friends' (Friend Roster, Team Multi-Man,
  Home-Run Contest, and the Boss Battles text no button shows) and the records' "Nintendo WFC"
  mentions. A message is rewritten only while its bytes are exactly the game's own, into the same
  slot (never longer, zeros after), at every frame boundary on both machines: a pure function of
  emulated memory, covered by the UX's compatibility version (`ux=4`).
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
  since). A pick made while friends play, or while one is about to land, is theirs too and is never
  announced, not even once the game is alone again. Going solo after a session, or loading a
  friend's keyframe, starts the reader over (`Events::NoteResync`), so nothing stale is ever
  printed. It only reads memory.
- **No network, proven per run**: `Tools/orca/online-menu.py` drives every page in both games and
  fails on any `/dev/net` open, any NET_KD_REQ, NCD, IP/Top or SSL request, or any NAND command on
  the save's, WiiConnect24's or the network settings' folders after the boot.

| stdout | When |
|---|---|
| `orca menu online casual local` | With Anyone > Casual: the game went to the local Versus character select ("local": nothing searches yet). |
| `orca menu online ranked local` | With Anyone > Ranked, the same. |
| `orca menu online friends` | Any With Friends button: the local Versus character select, where friends drop in. |

The lines print once per pick, only while the player is alone, and never carry state: the page
treats them as "the player asked to play online" (Casual and Ranked today: "coming soon", invite a
friend). A later build that searches will print `orca menu online casual` (no `local`) when its
search window opens, behind a `host` capability this build doesn't have.

Tests: `OrcaOnlineMenu*` unit tests (the gates, the textures and their precedence, the text rewrite,
the reader's edges and `AloneAt`); `Tools/orca/online-menu.py <nogui> <disc> [--pplus <dol>]
[--video Metal --shots <dir> [--scale 4]]` (every way through the menu, solo, offline);
`Tools/orca/online-menu-dropin.py` (a friend drops into a host on the ONLINE page; the pick that
follows is both players', and nothing prints; with `--invite`, a pick within 30 s of an invite's
stored keyframe prints nothing and one after it prints).

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

- `--rect X,Y,W,H` (one argument) works too. W and H below 1 count as 1.
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

| Command | Effect |
|---|---|
| `rect X Y W H` | Moves and resizes the view (units above). A rect while hidden doesn't show it. |
| `hide` / `show` | Takes the view off screen and puts it back. Hidden also means no input. |
| `focus` / `blur` | The game gets the input, or gives it back to the page. See "Focus" below. `blur` is new: the libretro host has no `blur`. |
| `pause` / `resume` | Answers `state paused` or `state running`. A `pause` before the game runs is held until it does, so `ready` waits for `resume`. **In a session** (Orca with `orca caps ... pause`) `pause` pauses only while the player's game is alone in it: no friend plugged in or on the way, not joining or catching up, no keyframe wanted or being made, and no `prepare-join`, `join` or `leave` waiting for a frame (`Rollback::OnlineMatch::SoloPauseAllowed`). Otherwise, and before the game runs, it answers `unsupported pause` then `state running`: the game keeps running (the friend's does), and nothing waits. A solo pause ends by itself, with `state running`, as soon as drop-in needs the game: a friend arriving in the room, `prepare-join`, `join` or `leave`. While paused, the room's connection stays up (its thread pings the server every second whatever the game does), but a room that drops meanwhile is reopened only after the resume (that happens at frame boundaries), so a pause longer than the server's 90 s hold on a dropped player's seat comes back to a new code. The pause stops the emulated machine between two instructions and changes nothing it computes; the `orca stats` line leaves the paused time out of its `fps`. An Orca without `pause` in its caps answers `unsupported pause` to every `pause` in a session. |
| `volume V` | V from 0 to 1, linear, as the libretro gain. For this run only: Dolphin.ini is untouched. It is allowed in a session and doesn't touch the compatibility key. |
| `perf off` / `perf fps` / `perf detailed` | With `orca caps ... perf`: the overlay's frame meter at the top left, solo or with friends (under the ping when a friend is in). `fps` (the default until the app says) shows `60 fps · 17 ms`: the frames shown in the last second and the longest of them, host wall time between the boundaries of shown frames (re-runs are not shown frames: their time counts in the next shown frame's; a join's catch-up starts the meter over). It turns yellow under 59 fps or past 20 ms, red under 50 fps or past two frame periods (a hitch), and keeps the worst colour of the last 2 s. `detailed` adds a graph of the last 2 s of frame times and, with a friend, the delay and rollbacks per second. A pause freezes it. Host-only drawing: no compatibility key change. Anything else answers `unsupported perf`. |
| `save N` / `load N` | Answers `unsupported save N` / `unsupported load N`. A session refuses save states, and Orca keeps the game's own saves in its NAND. |
| `reset` | The console's reset button. Answers `unsupported reset` in a session or before the game runs. |
| `prepare-join` | The host invited a friend to drop in. It calls `Orca::Online::PrepareJoin()`: the next frame boundary captures a keyframe ("Drop-in"). |
| `join <code>` / `leave` | With `caps join` / `caps leave`: into a friend's game, or out of one ("Drop-in"). Both are also taken while a launch join waits for the keyframe. |
| `delay auto` / `delay N` | With `caps delay` (Orca 0.3.5 on): this player's input delay. `N` from 1 to 6 frames is exactly that, never adapted; `auto` is the adaptive delay ("Input delay" below). The choice stays for the run, so it may come before a session and holds for every session after; until the app's `caps` line turns `delay` on, it waits. Mid-match it applies from this player's next frame and the friend needs nothing: each player's delay is its own, and every input travels explicitly. Anything else answers `unsupported delay`. The `orca stats` line's `delay` is the delay in use. |
| `quit` | Stops at once (no power-button shutdown, also outside a session), then exits. |

Any other line goes to stderr. The app's `EMBED_COMMAND` filter must add `blur` and `prepare-join`.

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
| `orca menu <event>` | The player picked something in the game's own Online menu: `online casual local`, `online ranked local`, `online friends` ("Online menu"). An event, printed once per pick, only while the player's game is alone. |
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
- **Covering page content:** nothing in the page can draw over the view, so a page sheet or menu over
  the box has to `hide` it, as the page already does for libretro.
- **No clipping:** the view isn't clipped to the window. The page keeps `rect` inside its viewport, as
  `measure()` does.

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
