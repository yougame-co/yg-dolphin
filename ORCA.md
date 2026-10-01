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
Online play goes through a private two-player YouGame room (`Source/Core/Core/Orca/Session/Online.h`):

| Variable | Meaning |
|---|---|
| `YOUGAME_BRIDGE`, `YOUGAME_TOKEN` | Set by the YouGame desktop app: tickets come from its loopback bridge. |
| `ORCA_ROOM` | Room code (6-12 of `a-z0-9`). Default: the app's invite, else a new code. |
| `ORCA_NAME` | Name shown in the room. |
| `ORCA_TEST_DEV_GAME` | Tests only: an anonymous dev ticket from the site instead of the app (no ratings, no account). |
| `ORCA_SITE` | Tests only: the site for dev tickets (default `https://yougame.co`). |
| `ORCA_TEST_PADS`, `ORCA_TEST_SKIP_RENDER` | Harness overrides; they change the compatibility key. |

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
without FMA3 has a different key. The first in is the host; its Orca starts the match once the other says hello.
Seat 0 is controller port 1.

Unit tests: `ninja tests && ./Binaries/Tests/tests --gtest_filter='Orca*'`. With `ORCA_LIVE_TEST=1`,
the `OrcaLive` tests also open real rooms on yougame.co.

**Launcher profiles (prototype: Project+).** A mod that boots through its own loader program runs
from a launcher profile, picked with `ORCA_PROFILE` (`Data/Sys/Orca/PPLUS32.ini` is Project+ v3.2).
The session boots the loader executable (`-e <launcher.dol>`) with the disc as Dolphin's default disc
(`-C Dolphin.Core.DefaultISO=<disc>`), and refuses unless the loader, the disc's ID and revision, and
the SD card image next to the loader match the profile (XXH3-128). The SD image is opened read-only:
every session forces SD writes and folder sync off. The profile also names the save that seeds the
session NAND (`SaveTitle`), and its frame hook goes live once the game's instruction is at the
address. None of a mod's files are in this repository; the player's machine downloads the official
release. Example (Project+'s `Project+ Netplay Launcher.dol` and `sd.raw` side by side in `<dir>`):

```bash
ORCA_SESSION=1 ORCA_PROFILE=PPLUS32 dolphin-emu-nogui -p headless -u <user> -v Null \
  -C "Dolphin.Core.DefaultISO=<RSBE01 rev 2 disc>" -e "<dir>/Project+ Netplay Launcher.dol"
```

A session also refuses whatever would change one player's game alone: pausing, loading savestates,
the reset button, and the Wii power button (Stop ends the match instead).

Stock behaviour Orca changes outside a session: the build uses the bundled libcurl (with mbedTLS
and the system CA file) on macOS and Linux, because the netcode needs libcurl's WebSocket support.
That client doesn't use the macOS Keychain's extra roots, and it doesn't do TLS 1.3. On Windows the
bundled libcurl uses Schannel and the system store, with certificate revocation checked best effort
(a CRL or OCSP server that can't be reached no longer fails the connection).

Not yet supported: a GameCube controller adapter as the local controller (the local pad is the host's
port 1 as mapped in Dolphin's controller settings).
