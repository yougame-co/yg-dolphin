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
| `ORCA_TEST_GATE` | Tests only: masks in the input gate ("Input gate"). A test override too. |
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

## Match start

Luis, 2026-10-03 (on his Windows PC): the first seconds of every match (the countdown and just
after) dropped frames and froze, and the rest of the match was smooth; loading may take longer, but
those drops must go. Two things compiled there, on the one thread that runs the game in a session
(single core): the GPU pipelines the match draws first, and the match's PowerPC code. Both now
compile before the match is on screen.

Status, 2026-10-03: on the Mac, from the second launch after an update on, both matches of a
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
- **A limit on that wait** (`ShaderCache::WaitForBootCompile`, sessions only). The first boot after
  an update compiles every pipeline in the UID cache (below), and the desktop app kills a boot that
  hasn't drawn in 120 s (`desktop/src/orca.ts`), a timer installed apps keep. After 40 s the wait
  ends as soon as the ubershaders are compiled; at 60 s it ends whatever is left. What is left
  compiles in the background on the runtime thread count and is drawn with an ubershader until it
  is ready; an ubershader still compiling at 60 s is compiled again at its first draw (a stall,
  not a failed start). The log says which: "Orca: compiled N pipelines (K ubershader) before the
  first frame in X s" or "Orca: stopped waiting for shaders after X s, ...". While the wait lasts
  more than a second, Orca prints `orca shaders <compiled> <total>` once a second, before
  `ready`, so an app can tell a slow boot from a hung one. `ORCA_TEST_SHADER_WAIT_MS=N` sets both
  limits to N ms. Dolphin's shader workers slept through work already queued when their count
  changed; they no longer do (`AsyncShaderCompiler::WorkerThreadRun`).

Rendering only: nothing drawn reaches RAM (EFB/XFB copies stay on the GPU, EFB access and bbox are
off), so none of this can make two players' games differ.

Why it came back after every update: Dolphin's compiled-shader files (`Cache/Shaders/*.cache`, D3D's
DXBC on Windows) carry the build's git commit in their header and are emptied by any other build;
the pipeline UID cache survives. Without the boot compile, that cache was recompiled on one
background thread (`ShaderCompilerThreads` defaults to 1), and a pipeline still queued at the
countdown compiled again on the emulation thread. macOS adds Apple's own Metal cache, kept per
binary (Metal keeps no `Cache/Shaders` file). Until this change only the desktop app's macOS
command line asked for the first two settings. The app's `-u` is `<userData>/orca/user`
(`%APPDATA%\yougame-desktop\orca\user` on Windows, `~/Library/Application
Support/yougame-desktop/orca/user` on macOS), one folder for every Orca version: the UID caches,
the EFB copy cache and the JIT list survive updates, the compiled shaders don't. The app passes no
`-v`, so Windows draws with Dolphin's default, D3D11 (`D3D`), unless the folder's `Dolphin.ini`
says otherwise; the forced-settings line names it (`GFXBackend=`).

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
glslang, SPIRV-Cross and FXC: its time after an update is unmeasured. New textures cost 3-5 ms in the same window
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
  a while after it: Brawl ~165 frames, Project+ ~28. On the M4 the warm-up takes 37 frames (Brawl,
  30,000 blocks) and 35 (Project+, 28,000), the blocks the stage's first frames run first.
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
- `ORCA_JITWARM=0` turns it off; `ORCA_JITWARM_LOG=1` logs each frame that compiled ahead.

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
- **With Friends**: each of its buttons goes to the local Versus character select, where friends
  drop in ("Drop-in"), and Orca prints `orca menu online friends`. Their descriptions say so (a
  brawl, or in Project+ a fight, with the friends you invite, 2-4 players); none promises a roster
  or a mode that isn't there.
- **B out of that character select** goes back to the page the pick was made on, with the cursor
  on the button picked, the way B after Group > Brawl comes back to Group with Brawl selected (and
  the way the game comes back from its own online modes): With Friends on FIGHT! (or whichever of
  its buttons), With Anyone on Casual or Ranked; one more B is the ONLINE page. It holds after
  matches too: the results come back to the same character select, and B from there goes back to
  the same page. From Group > Brawl (Project+'s VERSUS, and its boot character select) B still
  opens Group.

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
    Brawl / Basic Versus) and 31 (Ranked, was Team Battle) all go to Orca's block in place of the
    online sequences' entries, which starts sqVsMelee, the local Versus character select, as Group >
    Brawl does. No online scene, network thread or DWC is ever entered. The menu's exit code itself
    is left as the game wrote it, and Orca reads it.
  - The back out (sora_scene, both games): sqVsMelee always went back to sqMenuMain with argument 1,
    which the menu opens as Group with Brawl selected. The block keeps the exit code in sqVsMelee's
    object (+0x18, slack in its heap block) and the back out's helper (in the online entries' old
    blocks) turns it into sqMenuMain's argument for that button (24-27 -> 23-26, 29-31 -> 27-29;
    sqMenuMain's own table maps them to menu modes 24-27 and 29-31, and a mode is the exit code of
    the button the menu opens on), once; anything else is 1, as before. sqPrizeCheck restarts
    sqVsMelee after a match without touching the object, so the code outlives matches. Game code and
    memory: rollback re-runs it and a drop-in keyframe carries it.
  - The IOS refusals of every session stay as the backstop: NET_KD_REQ, IP/Top and SSL refuse
    in a session and each refusal is logged at INFO (`IOS_WC24`, `IOS_NET`, `IOS_SSL`).
- **Text** (`MenuText.h`): the main menu's message files, found through the menu scene's archive
  (`mu_menumain_en`, MiscData 5 and 10), are rewritten in emulated memory: Brawl's footer ("...
  while connected to Nintendo WFC."), Project+'s "(Discontinued)" and "(Project M not compatible!)",
  With Anyone's descriptions (Casual, Ranked), With Friends' (Friend Roster, Team Multi-Man,
  Home-Run Contest, and the Boss Battles text no button shows) and the records' "Nintendo WFC"
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
  since). A pick made while friends play, or while one is about to land, is theirs too and is never
  announced, not even once the game is alone again. Going solo after a session, or loading a
  friend's keyframe, starts the reader over (`Events::NoteResync`), so nothing stale is ever
  printed. It only reads memory.
- **No network, proven per run**: `Tools/orca/online-menu.py` drives every page in both games and
  fails on any `/dev/net` open, any NET_KD_REQ, NCD, IP/Top or SSL request, or any NAND command on
  the save's, WiiConnect24's or the network settings' folders after the boot.

| stdout | When |
|---|---|
| `orca menu online casual` / `orca menu online ranked` | With Anyone > Casual or Ranked, with the app's `host`: the game went to the local Versus character select and the page's queue searches ("Matchmaking"). |
| `orca menu online casual local` / `orca menu online ranked local` | The same without `host` (an older app or page): nothing searches. |
| `orca menu online friends` | Any With Friends button: the local Versus character select, where friends drop in. |
| `orca menu cancel` | Searching (or matched as the host, with nobody plugged in yet), the player backed out of the character select to the main menu, alone: the search is over. Once. |

The lines print once per pick, only while the player is alone, and never carry state.

Tests: `OrcaOnlineMenu*` unit tests (the gates, the textures and their precedence, the text rewrite,
the reader's edges and `AloneAt`); `Tools/orca/online-menu.py <nogui> <disc> [--pplus <dol>]
[--video Metal --shots <dir> [--scale 4]]` (every way through the menu, solo, offline);
`Tools/orca/online-menu-dropin.py` (a friend drops into a host on the ONLINE page; the pick that
follows is both players', and nothing prints; with `--invite`, a pick within 30 s of an invite's
stored keyframe prints nothing and one after it prints). online-menu.py's `back-*` scenarios hold B
out of the character select and press A on the page that comes back: the same pick (exit code) again
proves the page and the cursor (`back-friends-N`, `back-casual`, `back-ranked`), `back-chain` walks
B to ONLINE and the top page and picks Casual, `back-versus` checks Group > Brawl still comes back to
Group. They use the harness's per-visit rules (`@muMenuMain:2 ...`: only on the scene's second entry).

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
   queue, the host's next alone boundary writes the match block's header, and the locks follow
   ("Online rules": the ruleset's rules in both games, casual's random legal stage, no way out of
   the character and stage select; `ux=8`, which replaced the beta's Brawl-only rules preset). A
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
   {void}` ("limit"). The room settles each report by agreement (a mismatch, detail included, is
   void) and rates the set once.
7. A ranked room closes with 4004 "match-over" after its result: a clean end. The host prints
   `orca state friend-left match-over` and opens a fresh private room (never the closed code); the
   joiner prints `orca state host-left match-over`, moves to port 1 and opens its own room too.

Leaving: `leave` in a ranked set is a forfeit (the room's rule, 15 s). The survivor of a ranked set
whose opponent went (the room still holds a match without a verdict) stays in the room with no
session until the room's verdict (a forfeit or void, then 4004) or 30 s, then opens a room of its
own. In casual the host leaves the queue's room for a fresh private one as soon as the opponent is
gone. A desync voids the match in progress (`finish {void}`) before the players part.

### Lines

| stdin | |
|---|---|
| `host <code>` | With `caps host`: host the matched room (above). An invalid code, or a game that isn't alone: `unsupported host`. |
| `queue-cancel` | With `caps host`: the page's search ended; the overlay's search lines go. Nothing else changes. |

| stdout | |
|---|---|
| `orca caps ... host results locks` | `results` only where the result reader is verified (Brawl rev 2, Project+); the room hears the results either way. `locks` where the profile has a ruleset (the same two): queue rooms play the online rules ("Online rules"). Information only: the locks don't wait for the app's answer. |
| `orca menu online casual\|ranked` | The pick, with `host` agreed (`... local` without). |
| `orca menu cancel` | The player backed out of searching (above). |
| `unsupported host` | The `host` was refused. |
| `orca result <json>` | With `results` agreed: the room's verdict, when it arrives (not at this Orca's own read). At most 300 bytes. |
| `orca state friend-left match-over` / `orca state host-left match-over` | A ranked room closed after its result. |

`orca result` shapes (the local player first; `n` the game's number in the casual pairing or the
ranked set; `score` agreed wins; `d` the agreed detail):
- casual game: `{"q":"casual","k":"game","n":3,"out":"won"|"lost"|"draw"|"void","why"?:"mismatch"|"nocontest"|"ports"|"unrecorded"|"desync","d"?:{"st":2,"c":[mine,theirs],"s":[mine,theirs],"to":0|1}}`
- ranked game: `{"q":"ranked","k":"game","n":2,"out":...,"why"?:...,"score":[mine,theirs],"d"?:{...}}`
- ranked set: `{"q":"ranked","k":"set","out":"won"|"lost"|"void","by":"games"|"forfeit","why"?:"desync"|"limit"|"unrecorded","score":[mine,theirs],"r"?:[before,after],"rank"?:"<24 chars>"}`

The overlay toasts the same: "You won game 2 · 1–1", "You won the set 2–1", "Opponent left: you
win the set", "This set doesn't count".

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
  `result` (a set's or a casual game's verdict), `command-error` (logged; a refused begin tries
  again), the socket's 4004 close.

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

`Tools/orca/queue-e2e.mjs <nogui> <disc> [--pplus <dol>] [--queue ranked|casual] [--scenario cancel]`:
two headless Orcas (one for `cancel`), each behind a stand-in for the desktop app's bridge that
plays the page's part exactly (caps, Orca's own tickets, the queue ticket and `/match`,
`host`/`join`, the `match_room` rewrite) and serves the app's controller stream, whose pad the
script presses through the menus, the character select and the stage; in the fight port 2 walks off
the stage until it has no stocks left. It checks the stdout sequence and that both Orcas print the
same verdicts, each from its side. Dev tickets by default (nothing recorded); `--slug <listing>
--cookie-a/--cookie-b <file> --id-a/--id-b <id>` uses real tickets as two signed-in players.
Under `flock orca-run.lock`, never while Luis plays.

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
  Friends rooms register none, or one that returns no masks.
- **Not yet.** Synthesized presses (a Start the rules press for a player), and a raw view of a
  masked button for the rules (a press the game didn't see).

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

## Online rules

Phase 1 of the online rules for queue matches (YouGame `docs/ORCA_ONLINE_UX.md`; the design's
phases and rulesets): rules locked in both games, casual's Start going to a random legal stage,
no way back from the character and stage select to the menus, ranked's no-ready timer and its
stall rule. Friends rooms are unchanged: nothing here acts without the match block's header, and
only a queue room's host writes one (`Core/Orca/UX/OnlineRules.*`, `MatchBlock.h`).

### The match block's header

- **Where:** `MatchBlock.h`, at FreeSpace's block (`0x806F2380`, both games). Header: `YGM1`,
  version 1, mode (0 none, 1 casual, 2 ranked), ruleset (1 Brawl Supernova 2025, 2 Project+ 2024).
  Then the set state (`+0x10`, Phases 2-3), the step state (`+0x80`, Phase 2), Phase 1's ready
  timer (`+0xC0`), and the player's own rules saved when the header was written (`+0xD0`, and
  Project+'s stage switch data at `+0x100`).
- **Who writes it, and when:** only at a boundary where the game is its player's alone (Events.h
  `alone`: nobody plugged in or on the way, nothing anyone replays), the header this machine's room
  calls for (OnlineMatch `WantedRulesMode`): a casual or ranked queue room's mode while this game
  hosts it (port 1), or waits in a ranked room for the set's verdict; none otherwise. None clears
  it, gives back the saved rules and zeroes the block; a block that never had a header (the game's
  dead code) is never touched. Everything else in the block is game logic at every boundary.
- **The keyframe waits for it:** a keyframe for an arrival is captured only once the header is as
  the room calls for (at most 120 boundaries, so an arrival never waits forever). The header lands
  at the first alone boundary after the room's welcome, long before the opponent can arrive.
- **The joiner checks it** at the first frame hook after its keyframe loads: the mode its room's
  queue calls for (none for a friends room) and its own profile's ruleset. Anything else: it leaves
  at once, with `orca error mismatch That match couldn't start` and the bye reason `mismatch`.
- **Lifecycle:** casual unlocks when the opponent unplugs (the host leaves the queue room for a
  room of its own: header none). Ranked stays locked while the set is open, whoever is plugged in
  (the survivor waits in the room for the verdict), and clears once the room closes.

### The locks (while the header says casual or ranked; first runs and re-runs, only bytes that differ)

| | Brawl (Supernova 2025) | Project+ (2024) |
|---|---|---|
| gmSetRule | stock, 3 stocks, 8:00, handicap off, ratio 1.0, team attack on, pause off | the same with 4 stocks |
| Items | frequency None, every item off (record menu data `+0x00`, `+0x08`, `+0x0C`) | the same |
| Stage choice | casual Random, ranked Choose | the same |
| Legal stages | Random Stage Switch: Brawl page `0x04005007` (BF, FD, Delfino, YI, Lylat, Smashville), Melee page `0x00020000` (Pokemon Stadium); FD's bit off while a human player's pick is Ice Climbers (`0x10`): the match's init data (gmGlobalModeMelee +0x98, 0x5C per port: the character select writes each pick there as it is made; the panel's own +0x1B8 is only the hovered character), on the character select and on the way to the fight (a Random character resolved) | the 2024 Proposed list as its preset 3 (`Switch03.rss`) holds it: its first `0xA8` bytes of RSS_EXDATA (`0x8042C4E8`: page 0 the 9 legal stages, all in the random set, hazards off), written on the character select; casual also clears the strike table |
| Input gate | character select: B; stage select: B; fight: Start | the same, plus the D-pad on the character select (its Code Menu) and L, R and Z on the stage select (alternates, hazards, L+R+A) |

- Casual's Start: Random stage choice skips the stage select (two frames in scSelStage), and the
  game's own RNG (in memory, the same on both machines) picks from the legal list. Probed: the
  Brawl page's Smashville bit alone gave stage 0x21 (Smashville), the Melee page's 0x00020000 alone
  0x2E (Pokemon Stadium). Project+'s random never repeats the last
  stage (its own rule).
- No way back to the menus: sqVsMelee::setNext's state 5 takes the character select's result and
  sends anything but "go on" to state 16, the menu. A toggled group (`RSBE01.patches`,
  `PPLUS32.patches`, conditions on the header's magic and mode) flips that one store to state 4,
  the character select again, the way the stage select's B comes back to it: the BACK button (and
  held B) reload the character select instead (checked under the harness: A on BACK with
  `ORCA_TEST_QUEUE=casual` brings back a fresh character select; without it, the menu). The gate
  also takes B away on the character select, so held B does nothing at all. Nothing needs B to
  pick. Project+'s own hooks in sqVsMelee (MusicSelect, SSSButtons, css.asm) are elsewhere.
- Pause off: Brawl's rule, and no Start in a fight for Project+'s hold-Start pause.
- The rules screen still opens, but whatever is changed there goes back at the next boundary.

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

`ux=8` (the rules preset of `ux=6` is gone), cap `locks`. Test knob `ORCA_TEST_QUEUE=casual|ranked`
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
character select; held B for 30 frames without it goes to the menu (so the gate's B and the flip
are what keep a queue match in).

Not yet (Phase 1 leftovers): the rules bar's arrows and the Rules, team (BRAWL tab) and name
buttons still respond (the rules snap back at the next boundary; NameTags keeps the names), and A
on a NONE panel can add a CPU (the game is then void "ports"); Giga Bowser and Wario-Man aren't
refused in Project+; Project+'s Code Menu values (Auto L-Cancel, Stock Control, Input Assist,
Debug) aren't forced (their defaults are off and the menu can't open); the ready timer's no-show
and the stall claim were not run live; the room doesn't yet treat a stall claim as more than the
survivor's word (YouGame rooms).

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
clears the block. `ORCA_TEST_MATCH_BLOCK=ranked|casual` (tests only, no real room) asks for a
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
  - On the stage select, Project+'s own Stage Striking (the hook at 0x806B586C, untouched) strikes
    with X and draws the struck art. At each boundary the hook counts the active player's new bits
    in `STAGE_STRIKE_TABLE` (0x8042C822, page 0's positions 0-30 at +2/+4) into the block, then
    writes the table the block calls for: stages not in play (game 1's counterpicks, the loser's
    won stages), the strikes so far, and every stage of pages 1-4. A change it makes is redrawn by
    Project+'s code, which redraws the page's struck art whenever `PAGE_INDEX` (0x8042C821) isn't
    `CURRENT_PAGE` (0x80496000): the hook sets it to 0xFF. Project+'s own check then refuses a
    struck stage (it clears the selection).
  - Turns: the stage select's controller field (task +0x278) is the active port, and the gate
    masks every other port entirely. The active port keeps its stick and the step's button, X to
    strike or A to pick; B (clears strikes, back to the characters), L and R (alternate stages,
    and L+R+A back to the characters, which reads every port), Z (hazards), Start and Y are masked,
    and A and X on Random (it would strike every stage outside the random set, or pick for them).
  - A pick whose timer ran out: every other stage shown struck, the selection (+0x248) set to the
    default, and the gate presses A for the player (`Mask::press`, new) with everything else masked.
  - Results: a fight entered from the flow counts at its results screen (Results.h `Judge`): the
    winner's score, the stage won on, the record. A draw or a void counts nothing (the same game
    again). Two wins decide the set, and the stage select is free again.
  - The overlay's top-centre lines (UI only, published from first runs): "Ranked · Game 2 · ada
    1–0 bo", and on the stage select "bo strikes 2 stages · 0:24" or "Your turn: pick the stage
    with A · 0:09".
- **Compatibility:** `UX::kCompatVersion` 8.

**Test knob** `ORCA_TEST_RANKED=<0|1>` (a test override): a ranked header from the first frame for
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
  struck 1-2-1 with X, the first striker a fair coin from the game's own RNG seed (`0x805A00BC`,
  through murmur3's finalizer: the same on both machines, nobody's port). The last starter is the
  stage, and the flow selects it.
- **Later games:** the previous game's winner may ban one of the seven legal stages (X) or skip
  (Y, or let the timer run out); then the loser picks with A, with full DSR (never a stage the
  loser has won on this set; if that leaves nothing, DSR is dropped).
- **MK clause:** exactly one Meta Knight (gmCharacterKind `0x18`): the other player picks any
  legal stage, over strikes, the ban and DSR. **IC/FD:** with Ice Climbers (`0x10`) on either
  port, Final Destination is never allowed.
- **Turns:** the stage select's own controller field (task `+0x278`) is the turn's port, and the
  input gate leaves every other port nothing; the turn's port loses B (back to the character
  select), Start (it takes a stage), Z, L and R. A works only once the cursor has rested a frame:
  on the page button, and in a pick on an allowed stage.
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
- **Overlay:** "Game 2 · You 1–0 Ada" on the character and stage selects, and under it on the
  stage select whose turn it is with the timer ("Ada strikes 2 (X) · 0:24").

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

## Character order

A ranked set's later games pick characters in order on the game's own character select
(`Core/Orca/UX/CharOrder.*`; the ranked rules' Phase 4, Luis's spec of 2026-10-03):

- **Project+** (2024 Recommended, B.6): the previous game's winner picks first; the loser sees that
  pick and picks second. 45 s each. Then the stage select as the stage tracks run it.
- **Brawl** keeps the interim order until its stage pick can move before characters: both pick at
  once on later games, with the same 45 s. Supernova 2025's exact order (the ban, the loser's
  stage, then the winner may change character, then the loser) needs results -> stage select ->
  character select -> match, and the Meta Knight clause re-checked once both final characters are
  known (`MkFreePicker`, unit-tested, not wired; it takes Meta Knight's id in whichever id space the
  caller's characters use).
- Game 1 has no order (both pick at once, the ranked ready timer applies), and nothing here acts in
  casual or friends games.

**How it plays.** On the winner's turn the loser's controller does nothing, and the winner can't
take the token back (B) or go on (Start); A locks the pick in, on a character or anywhere else to
keep the one the token is on (the tokens stay down from one game's character select to the next).
Then the loser's turn, the winner's controller doing nothing; A locks in, or Start goes on. When
both have picked or the time is up, Orca presses Start for the loser, every other frame until the
screen goes. Brawl: B and Start masked for both, each locks in with A and then can't change, Start
once both have or the time is up. The overlay's top line says whose turn it is and the time left
("Ada picks first · 0:42", "Your pick · 0:42 · A locks it in").

A player who picks the token up (A on their own token) and lets the time run out can only move and
put it down; Orca puts it on the character under the hand if there is one, and presses Start only
once both tokens are down ("Waiting for Ada to put the token down").

**Determinism.** The state is 16 bytes right after the match block (`kArea` = `0x806F2580`, the
same dead code of `sqNetAnyOkiraku::setNext`, short of `kMatchBlockLimit`, so the rules tracks' own
layouts of the block can't collide with it): magic `YGCO`, the game it belongs to, the step, the
port that picks first, the lock-ins, the hook frame the step began at and the frames into it. The
frame hook advances it at every boundary, first runs and re-runs (a re-run at the same boundary
finds its own write and writes nothing new: no press counts on a step's first boundary, and every
change resets the step's frames to 0), and the input gate's masks and presses come from that
state and the character select's memory alone. The gate's masks gained a synthesized press
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

**Test knob** `ORCA_TEST_CHAR_ORDER=<pplus|brawl>:<game>:<winner port 1-2>` (a test override, both
processes alike): that set on every character select with no match block behind it, so the order
runs on the boot's character select once both players have joined.

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
(18 tests) and every `Orca*` unit test pass.

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
| `host <code>` / `queue-cancel` | With `caps host` (Orca 0.4.0 on): host the room the queue matched this game into, or the search ended ("Matchmaking"). `unsupported host` when refused. |
| `direct on` / `direct off` | With `caps direct` (Orca 0.3.9 on): the player's switch for direct links to the other players ("Direct links"). On until the app says; the choice stays for the run and applies while the `caps` line has `direct`. `off` drops every link at the room's next tick and sends every input through the relay (as YouGame's ticket switch does); `on` builds them anew. The `orca stats` line's `tx` shows the path in use. Anything else answers `unsupported direct`. |
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
| `orca menu <event>` | The player picked something in the game's own Online menu: `online casual`, `online ranked` (with `caps host`; `... local` without), `online friends`, or backed out of searching: `cancel` ("Online menu", "Matchmaking"). An event, printed once per pick, only while the player's game is alone. |
| `orca result <json>` | With `caps results`: a matchmade room's verdict on a game or a ranked set ("Matchmaking"). |
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
