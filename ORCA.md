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
| `ORCA_TEST_COMMANDS` | Tests only: `1` takes the embed protocol's stdin commands without embedding, and `test-drop-room` (the room's connection drops once, as a network failure would). |
| `ORCA_NAME` | Name shown in the room. |
| `ORCA_TEST_DEV_GAME` | Tests only: an anonymous dev ticket from the site instead of the app (no ratings, no account). |
| `ORCA_SITE` | Tests only: the site for dev tickets (default `https://yougame.co`). |
| `ORCA_TEST_PADS`, `ORCA_TEST_SKIP_RENDER` | Harness overrides; they change the compatibility key. |
| `ORCA_TEST_FULL_SNAPSHOTS` | `1`: rollback snapshots copy all of RAM instead of copy-on-write (`Source/Core/Core/Rollback/Cow.h`), to compare the two. A test override too. |

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
scripted match hashed identically on both, 2026-10-01), so Mac and PC players meet. An x86-64 CPU
without FMA3 has a different key. The host (the Orca that started the game) is seat 0, controller
port 1; a friend's seat is its room slot.

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
address. Its game patches (`Data/Sys/Orca/<profile>.patches`, `Core/Orca/UX/GamePatches.h`) apply
from then on, as guarded code groups only: `PPLUS32.patches` skips Project+'s "Create save file for
Project+?" prompt the way `RSBE01.patches` skips Brawl's (Project+ loads Brawl's boot module
unchanged), so a session with no save goes straight to character select at frame 150 and writes no
save; answering Yes cost 13 s of save writing (character select at frame ~925). None of a mod's
files are in this repository; the player's machine downloads the official release. Example
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
     took the room) is the other run's room: a host opens a fresh one rather than take it back.
   - A player the room re-seats on a new socket (`back`, or `join` for a listed id) is another run of
     Orca: it is unplugged like an `away` one, and greeted (and offered a keyframe) anew.
   - A room the page was shown ends with an emptied mirror (its code, no players or participants,
     `ready: false`), so the page knows Orca left it. Each room mirrors only while it is the
     process's newest: an older room's late update never overwrites a newer one's.
   - The page's lobby Leave is the app's `leave` (with `caps leave`); without it, it ends the room. A
     host that leaves always opens a new room, even with nobody in the old one: the page reads
     `orca state left` as Orca having left its room.

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

- `hello {c: key, r: heard you, h: is host}`: sent until answered. A mismatched key keeps a
  joiner out.
- `kf {f, id, n, x, key}`: host to a joiner's connection, every second until its first packet
  arrives. `key` is the keyframe's AES-256-GCM key (64 hex).
- `drop {r}`: host to a joiner it unplugged: `desync`, `stalled` or `network`.
- `p`: session packets, broadcast once to the room. Fields beyond the 2-player ones:
  - `ro`: roster, `[plug, unplug, live]` per seat, with -1 for never;
  - `ha`: history acknowledgement;
  - `ht`, `hf`, `hp`: history seat, first frame, and `[count, 4 pads hex]` runs;
  - `lv`: leaving.
- `bye {r}`: why a player leaves, `desync` or `stalled` (anything else is ignored). It is broadcast
  before the leave, so the room's leave event carries it.

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
- Orca.app runs a session unless `ORCA_SESSION=0` is in its environment. The app sets that for solo
  play, or `pause` and `reset` are refused.
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
| `pause` / `resume` | Answers `state paused` or `state running`. A `pause` before the game runs is held until it does, so `ready` waits for `resume`. **In a session** `pause` answers `unsupported pause` then `state running`: the game keeps running (the friend's does), and nothing waits. |
| `volume V` | V from 0 to 1, linear, as the libretro gain. For this run only: Dolphin.ini is untouched. It is allowed in a session and doesn't touch the compatibility key. |
| `save N` / `load N` | Answers `unsupported save N` / `unsupported load N`. A session refuses save states, and Orca keeps the game's own saves in its NAND. |
| `reset` | The console's reset button. Answers `unsupported reset` in a session or before the game runs. |
| `prepare-join` | The host invited a friend to drop in. It calls `Orca::Online::PrepareJoin()`: the next frame boundary captures a keyframe ("Drop-in"). |
| `join <code>` / `leave` | With `caps join` / `caps leave`: into a friend's game, or out of one ("Drop-in"). Both are also taken while a launch join waits for the keyframe. |
| `quit` | Stops at once (no power-button shutdown, also outside a session), then exits. |

Any other line goes to stderr. The app's `EMBED_COMMAND` filter must add `blur` and `prepare-join`.

### stdout: one event per line

| Line | Meaning |
|---|---|
| `ready W H FPS` | The first frame is on screen. W and H are the emulated frame buffer (Brawl: `640 480`); FPS is the VI rate (`59.9401`). The Mac takes about 1-2 s. A session can show `orca state lobby` **after** `ready` and wait there for its friend: after `ready`, no startup timeout applies. |
| `state running` / `state paused` | The answer to `pause` and `resume`. |
| `unsupported <command> [N]` | The command was refused. The game carries on, so this is not an error. |
| `key escape` / `key fullscreen` | Windows only, and only if Orca's window somehow has the keyboard (Esc, F11). Normally the page sees those keys itself, as on macOS. |
| `error <sentence>` | Something failed: bad arguments, the view couldn't open, the game didn't boot. Each `orca error` also gets an `error` twin. |
| `orca state <state>` / `orca error <code> <sentence>` | Session status (`Core/Orca/Status.h`). New states, such as drop-in's `joining`, come through unchanged: forward every line that starts with `orca `. |
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
  - Input counts while the last command was `focus` (the default), the view is shown, and the
    app's window is the foreground window, as on macOS.
  - The `key` lines only come if the child window somehow has the keyboard; normally the page sees
    Esc and F11 itself.
  - Caveat, as with libretro: a child window in another process shares the app's input queue, so a
    stall on Orca's main thread stalls the app's UI. `quit` and a closed stdin stop Orca at once.
- **macOS:** Orca never takes the keyboard or the mouse.
  - Its window can't become key and lets clicks through to the page beneath, so the app stays the
    active app with its key window.
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
