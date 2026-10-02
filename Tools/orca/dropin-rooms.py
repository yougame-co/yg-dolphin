#!/usr/bin/env python3
# dropin-rooms.py: rooms that go wrong, over real YouGame rooms, steered with the app's stdin
# commands (ORCA.md "Embedding") as in dropin-commands.py, headless and muted.
#
#   Tools/orca/dropin-rooms.py <dolphin-emu-nogui> <disc>
#
# 1. A friend joins a room nobody's game is in: it gives up within seconds (peer_left), plays on.
# 2. The friend reaches a room before its host's game does: the host finds someone else hosting
#    it, says so (orca error network) and opens a fresh room; the friend gives up (peer_left).
# 3. The friend joins the host's fresh room: the host says "friend-joined" once it plugs in.
# 4. The host's connection drops mid-match (the test-only "test-drop-room"): the host says so,
#    reconnects to the same room (its player id comes back into its held seat), and the friend,
#    whose host left, plays on ("host-left") and joins it again.
# 5. A host under the app (Tools/orca/fake_bridge.py as its bridge): the page's lobby Leave is the
#    app's "leave". The host leaves ("left"), the page is told the room is empty, then shown the
#    host's new room.
# Each step waits for the lines it expects and fails on a timeout. Two Orca processes at most.
import json
import os
import queue
import random
import re
import shutil
import string
import subprocess
import sys
import tempfile
import threading
import time

if len(sys.argv) < 3:
    sys.exit("usage: dropin-rooms.py <nogui> <disc>")
BIN, DISC = sys.argv[1], sys.argv[2]
HERE = os.path.dirname(os.path.abspath(__file__))
INPUT = os.path.join(HERE, "inputs", "bf-mario-link-results.txt")


def code(prefix):
    return prefix + "".join(random.choice(string.ascii_lowercase + string.digits) for _ in range(6))


WORK = tempfile.mkdtemp(prefix="orca-rooms-")
T0 = time.time()


def log(text):
    print(f"[{time.time() - T0:6.1f}] {text}", flush=True)


class Orca:
    def __init__(self, name, env_extra):
        self.name = name
        self.lines = queue.Queue()
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
        env.update(env_extra)
        args = [BIN, "-p", "headless", "-u", user, "-v", "Null",
                "-C", "Dolphin.DSP.Backend=No Audio Output",
                "-C", "Logger.Logs.ROLLBACK=True", "-C", "Logger.Logs.NETPLAY=True",
                "-C", "Logger.Options.Verbosity=3", "-e", DISC]
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

    def wait_log(self, pattern, timeout, after=0):
        """Waits for a line of the Dolphin log (stderr) matching `pattern`, past offset `after`."""
        end = time.time() + timeout
        while time.time() < end:
            with open(self.log_path, errors="replace") as f:
                f.seek(after)
                m = re.search(pattern, f.read())
            if m:
                return m
            time.sleep(0.1)
        fail(f"{self.name}: no log line like '{pattern}' in {timeout} s")

    def log_size(self):
        return os.path.getsize(self.log_path)

    def alive(self):
        if self.proc.poll() is not None:
            fail(f"{self.name} stopped (code {self.proc.returncode})")

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


log(f"work {WORK}")

# 1. Nobody's game in the room: the joiner gives up in seconds, not after the 90 s join limit.
friend = Orca("Friend", {})
orcas.append(friend)
friend.expect("orca caps", 30)
friend.send("caps join leave stats")
friend.expect("orca state playing", 90)
time.sleep(3)
empty = code("emp")
sent = time.time()
friend.send(f"join {empty}")
friend.expect("orca error peer_left", 30, forbid=("orca error network", "orca error internal"))
log(f"gave up on an empty room after {time.time() - sent:.1f} s")
if time.time() - sent > 20:
    fail("the friend waited too long in an empty room")
friend.expect("orca state playing", 20)
time.sleep(2)
friend.alive()

# 2. The friend gets to the host's room first: the host gives that room up and opens another.
taken = code("tkn")
mark = friend.log_size()
friend.send(f"join {taken}")
friend.wait_log(rf"in room {taken} on seat 1, joining", 30, after=mark)
host_player = "orca-rooms-" + code("h")
host = Orca("Host", {"ORCA_ROOM": taken, "ORCA_TEST_PLAYER": host_player})
orcas.append(host)
host.expect("orca caps", 30)
host.send("caps join leave stats")
host.expect("orca state playing", 90)
line = host.expect("orca error network", 30)
if "hosting this room" not in line:
    fail(f"the host's error doesn't say why: {line}")
fresh = host.wait_log(r"Drop-in: hosting room ([a-z0-9]+) again", 30).group(1)
if fresh == taken:
    fail("the host kept the room someone else hosts")
log(f"the host now hosts {fresh}")
friend.expect("orca error peer_left", 20, forbid=("orca error network", "orca error internal"))
friend.expect("orca state playing", 20)

# 3. The friend joins the host's fresh room; the host says so once it plugs in.
friend.send(f"join {fresh}")
host.expect("orca state friend-joining", 60)
friend.expect("orca state playing", 120)
host.expect("orca state friend-joined", 60)
time.sleep(15)
host.alive()
friend.alive()

# 4. The host's connection drops: it says so, reconnects to the same room, the friend plays on.
mark = host.log_size()
host.send("test-drop-room")
line = host.expect("orca error network", 20)
if "test drop" not in line:
    fail(f"the host's error doesn't say why: {line}")
again = host.wait_log(r"Drop-in: hosting room ([a-z0-9]+) again", 40, after=mark).group(1)
if again != fresh:
    fail(f"the host reopened {again}, not its room {fresh}")
friend.expect("orca state host-left", 30, forbid=("orca error internal",))
time.sleep(3)
friend.send(f"join {fresh}")
host.expect("orca state friend-joining", 60)
friend.expect("orca state playing", 120)
host.expect("orca state friend-joined", 60)
time.sleep(10)
for o in orcas:
    o.alive()
for o in orcas:
    o.stop()
orcas.clear()

# 5. The page's Leave through the app's bridge. The fake bridge mints dev tickets for its own dev
# game, so the friend uses that one too; it sends the Leave once, two seconds after the mirrored
# room reads ready (the friend's Orca said hello).
paged = code("pge")
bridge_log = os.path.join(WORK, "bridge.log")
bridge = subprocess.Popen([sys.executable, os.path.join(HERE, "fake_bridge.py")], stdout=subprocess.PIPE,
                          text=True, env=dict(os.environ, FAKE_ROOM=paged, FAKE_LOG=bridge_log))
try:
    port = bridge.stdout.readline().strip()
    host = Orca("Host2", {"ORCA_ROOM": paged, "YOUGAME_BRIDGE": f"http://127.0.0.1:{port}",
                          "YOUGAME_TOKEN": "t0ken", "YOUGAME_GAME": "orca-test"})
    orcas.append(host)
    host.expect("orca caps", 30)
    host.send("caps join leave stats")
    host.expect("orca state playing", 90)
    friend = Orca("Friend2", {"ORCA_TEST_DEV_GAME": "orca-bridge-test"})
    orcas.append(friend)
    friend.expect("orca caps", 30)
    friend.send("caps join leave stats")
    friend.expect("orca state playing", 90)
    friend.send(f"join {paged}")
    host.expect("orca state friend-joining", 60)
    host.expect("orca state left", 30, forbid=("orca error",))
    line = friend.expect("orca ", 30, forbid=("orca error internal",))
    while not (line.startswith("orca state host-left") or line.startswith("orca error peer_left")):
        line = friend.expect("orca ", 30, forbid=("orca error internal",))
    # The page ends on the host's new room: the old one emptied first (or, had the new room been
    # quicker, never mirrored again after it).
    end = time.time() + 20
    rooms = []
    while time.time() < end:
        with open(bridge_log) as f:
            rooms = [json.loads(l)["mpRoom"] for l in f if '"mpRoom"' in l]
        if rooms and rooms[-1]["code"] != paged and rooms[-1]["players"]:
            break
        time.sleep(0.5)
    mirrors = ", ".join(f'{r["code"]}:{len(r["players"])}' for r in rooms)
    log(f"mirrors: {mirrors}")
    if not rooms or rooms[-1]["code"] == paged or not rooms[-1]["players"]:
        fail(f"the page doesn't end on the host's new room: {mirrors}")
    if not any(r["code"] == paged and len(r["players"]) >= 2 for r in rooms):
        fail("the page never saw the friend in the host's room")
    first_new = next(i for i, r in enumerate(rooms) if r["code"] != paged)
    if any(r["code"] == paged for r in rooms[first_new:]):
        fail(f"the old room was mirrored after the new one: {mirrors}")
    if not any(r["code"] == paged and not r["players"] for r in rooms):
        log("(the new room's mirror went out before the old one's end)")
    for o in orcas:
        o.alive()
    for o in orcas:
        o.stop()
finally:
    bridge.kill()
log("PASS")
if os.environ.get("ORCA_TEST_KEEP") == "1":
    log(f"logs kept in {WORK}")
else:
    shutil.rmtree(WORK, ignore_errors=True)
