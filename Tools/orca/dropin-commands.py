#!/usr/bin/env python3
# dropin-commands.py: two Orcas over a real YouGame room, steered with the app's stdin commands
# (ORCA.md "Embedding": caps, join, leave) the way the desktop app would, headless and muted.
#
#   Tools/orca/dropin-commands.py <dolphin-emu-nogui> <disc> [--pplus <launcher.dol>]
#
# 1. Host and Friend each boot their own game, solo, in rooms of their own.
# 2. Friend sends "join <host's room>": it drops into the host's game during play.
# 3. Friend sends "leave": unplugged at an agreed frame, it plays on solo ("left").
# 4. Friend joins again; then the host sends "leave": Friend sees "host-left" and plays on.
# Each step waits for the states it expects and fails on a timeout. Two Orca processes at most.
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
    sys.exit(__doc__ if __doc__ else "usage: dropin-commands.py <nogui> <disc> [--pplus <dol>]")
BIN, DISC = sys.argv[1], sys.argv[2]
PPLUS = sys.argv[sys.argv.index("--pplus") + 1] if "--pplus" in sys.argv else None
HERE = os.path.dirname(os.path.abspath(__file__))
INPUT = os.path.join(HERE, "inputs", "bf-mario-link-results.txt")
ROOM = "cmd" + "".join(random.choice(string.ascii_lowercase + string.digits) for _ in range(6))
WORK = tempfile.mkdtemp(prefix="orca-cmd-")
T0 = time.time()


def log(text):
    print(f"[{time.time() - T0:6.1f}] {text}", flush=True)


class Orca:
    def __init__(self, name, env_extra):
        self.name = name
        self.lines = queue.Queue()
        self.states = []
        self.stats = []
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
            "YG_INPUT": INPUT,
        }
        if PPLUS:
            env["ORCA_PROFILE"] = "PPLUS32"
        env.update(env_extra)
        args = [BIN, "-p", "headless", "-u", user, "-v", "Null",
                "-C", "Dolphin.DSP.Backend=No Audio Output",
                "-C", "Logger.Logs.ROLLBACK=True", "-C", "Logger.Logs.NETPLAY=True",
                "-C", "Logger.Logs.CORE=True", "-C", "Logger.Logs.IOS_FS=True",
                "-C", "Logger.Options.Verbosity=2"]
        if PPLUS:
            args += ["-C", f"Dolphin.Core.DefaultISO={DISC}", "-e", PPLUS]
        else:
            args += ["-e", DISC]
        self.err = open(os.path.join(WORK, f"{name}.log"), "w")
        self.proc = subprocess.Popen(args, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=self.err, text=True, bufsize=1)
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        for line in self.proc.stdout:
            line = line.rstrip("\n")
            if line.startswith("orca stats "):
                self.stats.append(line[len("orca stats "):])
                continue
            if line.startswith("orca state "):
                self.states.append(line[len("orca state "):])
            log(f"{self.name}> {line}")
            self.lines.put(line)
        self.lines.put(None)

    def send(self, line):
        log(f"{self.name}< {line}")
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def expect(self, want, timeout, forbid=("orca error",)):
        """Waits for a stdout line starting with `want`; fails on a line starting with `forbid`."""
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
            if any(line.startswith(f) for f in forbid):
                fail(f"{self.name}: '{line}' while waiting for '{want}'")
        fail(f"{self.name}: no '{want}' in {timeout} s")

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


def last_stats(orca):
    return orca.stats[-1] if orca.stats else "(none)"


log(f"room {ROOM}, work {WORK}")
host = Orca("Host", {"ORCA_ROOM": ROOM})
orcas.append(host)
host.expect("orca caps", 30)
host.send("caps join leave stats")
friend = Orca("Friend", {})
orcas.append(friend)
friend.expect("orca caps", 30)
friend.send("caps join leave stats")
host.expect("orca state playing", 90)
friend.expect("orca state playing", 90)
time.sleep(40)

# 2. A join during play.
friend.send(f"join {ROOM}")
friend.expect("orca state joining", 20)
host.expect("orca state friend-joining", 60)
friend.expect("orca state playing", 120)
log(f"joined; host stats {last_stats(host)}")
time.sleep(30)
log(f"after 30 s together: host {last_stats(host)} / friend {last_stats(friend)}")

# 3. The friend leaves at an agreed frame and plays on.
friend.send("leave")
friend.expect("orca state left", 30)
host.expect("orca state friend-left left", 30)
time.sleep(10)
if friend.proc.poll() is not None:
    fail("friend stopped after leaving")

# 4. Again; then the host leaves.
friend.send(f"join {ROOM}")
friend.expect("orca state playing", 120)
time.sleep(20)
host.send("leave")
host.expect("orca state left", 30)
friend.expect("orca state host-left", 30)
time.sleep(10)
for o in orcas:
    if o.proc.poll() is not None:
        fail(f"{o.name} stopped after the host left")
log(f"host stats {last_stats(host)} / friend {last_stats(friend)}")
for o in orcas:
    o.stop()
log("PASS")
if os.environ.get("ORCA_TEST_KEEP") == "1":
    log(f"logs kept in {WORK}")
else:
    shutil.rmtree(WORK, ignore_errors=True)
