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
| `prepare-join` | The host invited a friend to drop in. It calls `Orca::Online::PrepareJoin()`, which is a stub until drop-in join lands. |
| `quit` | Stops at once (no power-button shutdown, also outside a session), then exits. |

Any other line goes to stderr. The app's `EMBED_COMMAND` filter must add `blur` and `prepare-join`.

### stdout: one event per line

| Line | Meaning |
|---|---|
| `ready W H FPS` | The first frame is on screen. W and H are the emulated frame buffer (Brawl: `640 480`); FPS is the VI rate (`59.9401`). The Mac takes about 1-2 s. A session can show `orca state lobby` **after** `ready` and wait there for its friend: after `ready`, no startup timeout applies. |
| `state running` / `state paused` | The answer to `pause` and `resume`. |
| `unsupported <command> [N]` | The command was refused. The game carries on, so this is not an error. |
| `key escape` / `key fullscreen` | Windows only, while Orca has the keyboard (Esc, F11). On macOS the page sees those keys itself. |
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

- **Windows:** the child window has the Win32 keyboard focus.
  - It gets it with `focus` or a click on the game; `blur`, `hide` or a click elsewhere gives it back to the parent.
  - A `show` puts the focus back if the last command was `focus`.
  - Alt and F10 stay game keys, so they don't open the app's menu. Alt+F4 still reaches the app.
  - The page sees no keys meanwhile, hence the `key` lines.
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
