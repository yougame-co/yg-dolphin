#!/usr/bin/env python3
"""Offline, native-input-only discovery. Invoke through heavy-slot and run-lock."""
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
proof = root / "notes/doubles-teams-proof"
work = root / "build-doubles-teams/discovery-native"
work.mkdir(parents=True, exist_ok=True)
game = work / "game"
game.mkdir(exist_ok=True)
source = Path("/Volumes/YGWorktrees/pplus-3.2.1b/run")
launcher = "Project+ Netplay Launcher.dol"
for name in (launcher, "sd.raw"):
    if not (game / name).exists():
        subprocess.run(["cp", "-c", str(source / name), str(game / name)], check=True)
inputs = work / "native-input.txt"
inputs.write_text((proof / "host.txt").read_text() + "\n" +
                  (proof / "guest.txt").read_text())
probe = work / "discovery.probe"
commands = []
for scene, frames in (("scSelctCharacter", (1400, 1451, 1460, 1500, 1790)),
                      ("scSelStage", (1, 100,)), ("scMelee", (1,)),
                      ("scVsResult", (1,))):
    for frame in frames:
        prefix = f"{scene}-{frame}"
        for name, address, size in (
                ("global", "[805a00e0]", "30"),
                ("melee", "[[805a00e0]+8]", "400"),
                ("select", "[[805a00e0]+10]", "1000"),
                ("rule", "[[805a00e0]+1c]", "80"),
                ("result", "[[805a00e0]+18]", "1400"),
                ("task", "[[[805a0060]+4]+400]", "700")):
            commands.append(f"@{scene}:1 {frame} dump {address} {size} {prefix}-{name}")
        if scene == "scSelctCharacter":
            task = "[[[805a0060]+4]+400]"
            for port in range(4):
                address = f"[{task}+{0x44 + 4 * port:x}]"
                commands.append(f"@{scene}:1 {frame} dump {address} 240 {prefix}-area{port}")
probe.write_text("\n".join(commands) + "\n")
user = work / "user"
user.mkdir(exist_ok=True)
environment = {
    "PATH": os.environ.get("PATH", ""),
    "TMPDIR": "/Volumes/YGWorktrees/tmp/orca-doubles-teams",
    "ORCA_SESSION": "1", "ORCA_PROFILE": "PPLUS321", "ORCA_TEST_PADS": "4",
    "ORCA_TEST_RNG_SEED": "0xb28296b9",
    "ORCA_UX_TEST_NAMES": "Seat1,Seat2,Seat3,Seat4",
    "YG_INPUT": str(inputs), "ORCA_UX_PROBE": str(probe),
    "YG_SCENES": "1", "YG_RESULTS": "1", "YG_EXIT_AFTER": "6500",
    "ORCA_NO_LOCAL_INPUT": "1",
    "SDL_JOYSTICK_HIDAPI": "0", "SDL_JOYSTICK_HIDAPI_GAMECUBE": "0",
    "SDL_HIDAPI_LIBUSB": "0", "SDL_JOYSTICK_IOKIT": "0", "SDL_JOYSTICK_MFI": "0",
    "SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT": "0x0000/0x0000",
}
command = [str(root / "build-doubles-teams/Binaries/Orca.app/Contents/MacOS/Orca"),
           "-p", "headless", "-v", "Null", "-u", str(user),
           "-C", "Dolphin.DSP.Backend=No Audio Output",
           "-C", "Logger.Logs.ROLLBACK=True", "-C", "Logger.Options.Verbosity=3",
           "-C", "Dolphin.Core.DefaultISO=/Volumes/YGWorktrees/OSCOO/ROMs/Wii/"
           "Super Smash Bros. Brawl (USA, Canada) (Rev 2).wbfs",
           "-e", str(game / launcher)]
with (work / "engine.log").open("w") as log:
    result = subprocess.run(command, env=environment, stdout=log, stderr=subprocess.STDOUT,
                            timeout=240)
raise SystemExit(result.returncode)
