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

Two friends meet when both use the same code and the same compatibility key (the Orca build, the
game profile, every forced setting and the seeded save; not the host CPU, so a Mac and a PC can
play each other). The first in is the host; its Orca starts the match once the other says hello.
Seat 0 is controller port 1.

Unit tests: `ninja tests && ./Binaries/Tests/tests --gtest_filter='Orca*'`. With `ORCA_LIVE_TEST=1`,
the `OrcaLive` tests also open real rooms on yougame.co.
