#!/usr/bin/env python3
# online-menu-dropin.py: a friend drops into a host who sits on the game's own ONLINE page
# (ORCA.md "Online menu"), over a real YouGame dev room, headless and muted. Two Orcas at most.
#
#   Tools/orca/online-menu-dropin.py <dolphin-emu-nogui> <disc> [--pplus <launcher.dol>]
#
# 1. Host boots to the main menu and opens PLAY ONLINE: the ONLINE page (With Friends / With
#    Anyone), and stays there.
# 2. Friend sends "join <host's room>": the host's keyframe is taken on the ONLINE page, the friend
#    loads it, catches up and plugs into port 2 ("playing" / "friend-joined").
# 3. With the friend plugged in, the host's script picks With Anyone > Casual: both games leave the
#    menu for the local Versus character select at the same frame, and neither Orca prints any
#    "orca menu" line (a pick while friends play is theirs too, never an event).
# 4. The friend leaves: the host plays on alone ("friend-left left"), still printing nothing (the
#    pick was made while not alone and is never announced later).
# Fails on a desync, a timeout, an "orca error", or any "orca menu" line.
import os
import queue
import random
import shutil
import string
import subprocess
import sys
import tempfile
import threading
import time

if len(sys.argv) < 3:
    sys.exit("usage: online-menu-dropin.py <nogui> <disc> [--pplus <dol>]")
BIN, DISC = sys.argv[1], sys.argv[2]
PPLUS = sys.argv[sys.argv.index("--pplus") + 1] if "--pplus" in sys.argv else None
ROOM = "olm" + "".join(random.choice(string.ascii_lowercase + string.digits) for _ in range(6))
WORK = tempfile.mkdtemp(prefix="orca-online-dropin-")
T0 = time.time()
# The host's pick, in frames of the main menu scene: late enough that the friend has plugged in
# (a friend plugs in some 30-60 s after the host reaches the menu). A join slower than this makes
# the host pick alone: it prints the event and the test fails on it, saying so.
PICK_AT = 7200

if PPLUS:
    HOST_INPUT = ["285 330 1 B", "@muMenuMain 120 124 1 B",
                  "@muMenuMain 200 203 1 DOWN", "@muMenuMain 250 253 1 A"]
else:
    HOST_INPUT = ["@scStrap mash 30 99999 1 40 A", "@scBoot mash 30 99999 1 40 A",
                  "@scTitle mash 30 99999 1 40 A",
                  "@muMenuMain 120 123 1 DOWN", "@muMenuMain 170 173 1 A"]
HOST_INPUT += [f"@muMenuMain {PICK_AT} {PICK_AT + 3} 1 RIGHT",
               f"@muMenuMain {PICK_AT + 50} {PICK_AT + 53} 1 A",
               f"@muMenuMain {PICK_AT + 100} {PICK_AT + 103} 1 A"]


def log(text):
    print(f"[{time.time() - T0:6.1f}] {text}", flush=True)


def write(name, lines):
    path = os.path.join(WORK, name)
    with open(path, "w") as f:
        f.write("\n".join(["# online-menu-dropin.py"] + lines) + "\n")
    return path


class Orca:
    def __init__(self, name, env_extra, input_path):
        self.name = name
        self.lines = queue.Queue()
        self.menu = []
        user = os.path.join(WORK, f"user-{name}")
        os.makedirs(user)
        env = {
            "HOME": os.environ["HOME"],
            "PATH": os.environ["PATH"],
            "ORCA_SESSION": "1",
            "ORCA_TEST_COMMANDS": "1",
            "ORCA_TEST_DEV_GAME": "orca-dropin-test",
            "ORCA_TEST_KEYFRAME_DIR": os.path.join(WORK, "kf"),
            "ORCA_NAME": name,
            "YG_INPUT": input_path,
            "YG_SCENES": "1",
        }
        if PPLUS:
            env["ORCA_PROFILE"] = "PPLUS32"
        env.update(env_extra)
        args = ["nice", "-n", "10", BIN, "-p", "headless", "-u", user, "-v", "Null",
                "-C", "Dolphin.DSP.Backend=No Audio Output",
                "-C", "Logger.Logs.ROLLBACK=True", "-C", "Logger.Options.Verbosity=2"]
        if PPLUS:
            args += ["-C", f"Dolphin.Core.DefaultISO={DISC}", "-e", PPLUS]
        else:
            args += ["-e", DISC]
        self.log_path = os.path.join(WORK, f"{name}.log")
        self.err = open(self.log_path, "w")
        self.proc = subprocess.Popen(args, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=self.err, text=True, bufsize=1)
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        for line in self.proc.stdout:
            line = line.rstrip("\n")
            if line.startswith("orca stats "):
                continue
            if line.startswith("orca menu "):
                self.menu.append(line)
            log(f"{self.name}> {line}")
            self.lines.put(line)
        self.lines.put(None)

    def send(self, line):
        log(f"{self.name}< {line}")
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def expect(self, want, timeout):
        end = time.time() + timeout
        while time.time() < end:
            try:
                line = self.lines.get(timeout=max(0.1, end - time.time()))
            except queue.Empty:
                break
            if line is None:
                fail(f"{self.name} exited (code {self.proc.wait()}) waiting for '{want}'")
            if line.startswith(want):
                return line
            if line.startswith("orca error") or line.startswith("orca menu"):
                fail(f"{self.name}: '{line}' while waiting for '{want}'")
        fail(f"{self.name}: no '{want}' in {timeout} s")

    def log_lines(self, text):
        with open(self.log_path, errors="replace") as f:
            return [l.rstrip() for l in f if text in l]

    def wait_log(self, text, timeout, after=0):
        end = time.time() + timeout
        while time.time() < end:
            if self.proc.poll() is not None:
                fail(f"{self.name} exited (code {self.proc.returncode}) waiting for '{text}'")
            found = self.log_lines(text)
            if len(found) > after:
                return found[after]
            time.sleep(1)
        fail(f"{self.name}: no '{text}' in its log in {timeout} s")

    def stop(self):
        if self.proc.poll() is None:
            try:
                self.send("quit")
                self.proc.wait(timeout=20)
            except Exception:
                self.proc.kill()
        self.err.close()


orcas = []


def fail(why):
    log(f"FAIL: {why}")
    for o in orcas:
        if o.proc.poll() is None:
            o.proc.kill()
    log(f"logs kept in {WORK}")
    sys.exit(1)


log(f"room {ROOM}, work {WORK}")
host = Orca("Host", {"ORCA_ROOM": ROOM}, write("host.txt", HOST_INPUT))
orcas.append(host)
host.expect("orca caps", 30)
host.send("caps join leave stats")
host.expect("orca state playing", 90)
log(f"Host: {host.wait_log('-> muMenuMain', 180)}")
# On the ONLINE page (opened 170-250 frames into the menu) for a few seconds.
time.sleep(10)

friend = Orca("Friend", {}, write("friend.txt", ["# no presses: port 2 stays still"]))
orcas.append(friend)
friend.send(f"join {ROOM}")
friend.expect("orca caps", 30)
friend.send("caps join leave stats")
friend.expect("orca state joining", 60)
host.expect("orca state friend-joining", 60)
friend.expect("orca state playing", 120)
host.expect("orca state friend-joined", 60)
keyframe = host.wait_log("Drop-in: keyframe of frame", 10)
log(f"Host: {keyframe}")

# The host's pick: both leave the menu for character select, at the same frame.
before = {o.name: len(o.log_lines("-> scSelctCharacter")) for o in orcas}
frames = {}
for o in orcas:
    line = o.wait_log("-> scSelctCharacter", 240, after=before[o.name])
    frames[o.name] = int(line.split("Scene frame ")[1].split(":")[0])
    log(f"{o.name}: {line[line.find('Scene frame'):]}")
if frames["Host"] != frames["Friend"]:
    fail(f"character select at frame {frames['Host']} (host) and {frames['Friend']} (friend)")
time.sleep(10)
friend.send("leave")
friend.expect("orca state left", 30)
host.expect("orca state friend-left left", 30)
time.sleep(10)
for o in orcas:
    if o.proc.poll() is not None:
        fail(f"{o.name} stopped")
    if o.menu:
        fail(f"{o.name} printed {o.menu} while not alone")
    desync = o.log_lines("desync") + o.log_lines("doesn't match the host's")
    if desync:
        fail(f"{o.name}: {desync[0]}")
for o in orcas:
    o.stop()
KEYS = ("Scene frame", "Drop-in: keyframe of frame", "Drop-in: loaded keyframe", "Online match",
        "Drop-in: back to solo", "main menu text", "Online menu")
for o in orcas:
    for line in o.log_lines(""):
        if any(k in line for k in KEYS):
            log(f"{o.name}: {line[line.find('N[') if 'N[' in line else 0:]}")
log("PASS")
if os.environ.get("ORCA_TEST_KEEP") == "1":
    log(f"logs kept in {WORK}")
else:
    shutil.rmtree(WORK, ignore_errors=True)
