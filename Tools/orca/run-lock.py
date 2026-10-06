#!/usr/bin/env python3
# run-lock.py: the queue every scripted Orca run on this Mac goes through (ORCA_NOTES.md, "Sharing the
# Mac: the run queue"). Two classes of run:
#
#   Tools/orca/run-lock.py check [options] -- <command...>   a correctness run
#   Tools/orca/run-lock.py perf  [options] -- <command...>   a measurement
#   Tools/orca/run-lock.py status [--json | --luis]          who runs, who waits, and why
#
# check: queue-e2e.mjs, the drop-in tools, synctests, hashlogs, anything whose verdict does not depend
#   on a quiet machine. Up to ORCA_CHECK_SLOTS (3) run at once while the 1-minute load is under 12 and
#   more than 30% of memory is free, one at a time under 20 and 20%, none above that. A check waits
#   only for perf runs (and for runs outside this queue that hold the old locks).
# perf: two-Orca perf gates, frame-time A/Bs, anything timed. It has the machine to itself: it waits
#   for every check to finish, and no check starts while it waits or runs.
# Both wait while Luis's own Orca is open, saying so in one line, and take turns first come, first
# served: a run keeps its place while it waits, so nothing that waits is starved.
#
# Options:
#   --label TEXT     what `status` shows for this run (default: the command)
#   --wait-max S     give up after waiting S seconds: exit 9 (default: wait as long as it takes)
#   --serial NAME    never beside another run with the same NAME (repeatable). Added by itself for
#                    runs that share accounts or a queue: queue-e2e.mjs with --slug and page-e2e.mjs
#                    (two signed-in players), and a queue-e2e.mjs that still uses one shared dev game.
#   --yield          stop the command if Luis's Orca opens while it runs: exit 10. A perf run without
#                    it notes the moment in its output instead (the numbers after it are suspect).
#
# Exit status: the command's own; 9 gave up waiting; 10 stopped by --yield; 11 the command takes
# orca-run-solo.lock or orca-run.lock itself and would wait for this very run forever (drop that
# flock, or run the script without run-lock.py); 2 usage.
#
# How it shares with the old locks: a check holds orca-run-solo.lock and orca-run.lock shared and a
# perf run holds both exclusive, so a script that still takes them with plain `flock` (exclusive)
# acts as a perf run: it waits for the checks, and the checks wait for it. While such a script is
# waiting for the locks, no new check starts, so it is never starved either. The guard that holds
# both locks while Luis plays (luis-orca-guard.sh) works as before. The queue itself is
# orca-run-queue/ beside the locks: one ticket file per run, locked by its owner for as long as it
# lives, so a run that dies leaves no ticket behind.
#
# Environment: ORCA_LOCK_DIR (default ~/Documents/brawl-dolphin), ORCA_CHECK_SLOTS (3). Tests use
# ORCA_RUN_LOCK_POLL (seconds between looks, 2), ORCA_RUN_LOCK_TEST_PLAYING (a file: Luis "plays"
# while it exists) and ORCA_RUN_LOCK_TEST_LOAD (a file holding "<load> <free %>").
import argparse
import errno
import fcntl
import json
import os
import re
import shlex
import signal
import subprocess
import sys
import time

LOCK_DIR = os.environ.get("ORCA_LOCK_DIR") or os.path.expanduser("~/Documents/brawl-dolphin")
SOLO = os.path.join(LOCK_DIR, "orca-run-solo.lock")
TWO = os.path.join(LOCK_DIR, "orca-run.lock")
QUEUE = os.path.join(LOCK_DIR, "orca-run-queue")
SLOTS = max(1, int(os.environ.get("ORCA_CHECK_SLOTS") or 3))
POLL = float(os.environ.get("ORCA_RUN_LOCK_POLL") or 2)
REMIND_S = 600
# Luis's Orca, matched on executable paths only (ps comm), never on whole command lines: a waiting
# wrapper carrying this pattern in its own arguments would look like him playing.
LUIS_RE = re.compile(r"Resources/runtimes/orca/Orca\.app/Contents/MacOS/Orca|yougame-desktop/orca/apps/")
LUIS = "Luis's Orca is open"
EXIT_GAVE_UP, EXIT_YIELDED, EXIT_DEADLOCK = 9, 10, 11


def say(text):
    sys.stderr.write(f"run-lock {time.strftime('%H:%M:%S')} {text}\n")
    sys.stderr.flush()


def ago(seconds):
    seconds = int(max(0, seconds))
    if seconds < 60:
        return f"{seconds}s"
    if seconds < 3600:
        return f"{seconds // 60}m {seconds % 60:02d}s"
    return f"{seconds // 3600}h {seconds // 60 % 60:02d}m"


def luis_playing():
    fake = os.environ.get("ORCA_RUN_LOCK_TEST_PLAYING")
    if fake:
        return os.path.exists(fake)
    out = subprocess.run(["ps", "-axo", "comm="], capture_output=True, text=True).stdout
    return any(LUIS_RE.search(line) for line in out.splitlines())


def machine():
    """(1-minute load, % of memory free), the numbers `uptime` and `memory_pressure` print."""
    fake = os.environ.get("ORCA_RUN_LOCK_TEST_LOAD")
    if fake:
        load, free = open(fake).read().split()
        return float(load), int(free)
    load = os.getloadavg()[0]
    try:
        free = int(subprocess.run(["sysctl", "-n", "kern.memorystatus_level"], capture_output=True,
                                  text=True).stdout.strip())
    except ValueError:
        out = subprocess.run(["memory_pressure"], capture_output=True, text=True).stdout
        m = re.search(r"free percentage: (\d+)%", out)
        free = int(m.group(1)) if m else 0
    return load, free


def checks_allowed(load, free):
    if load < 12 and free > 30:
        return SLOTS
    if load < 20 and free > 20:
        return 1
    return 0


def perf_roomy(load, free):
    return load < 20 and free > 20


# ---- processes: plain `flock` runs on the old locks, and a run's own descendants ----

def processes():
    """{pid: (ppid, argv words)} from ps; argv split on spaces (good enough for flock's arguments)."""
    out = subprocess.run(["ps", "-axo", "pid=,ppid=,args="], capture_output=True, text=True).stdout
    procs = {}
    for line in out.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and parts[0].isdigit() and parts[1].isdigit():
            procs[int(parts[0])] = (int(parts[1]), parts[2].split())
    return procs


def parse_flock(words):
    """`flock [options] <file> ...` -> (file, shared, nonblocking); file None for the fd form."""
    shared = nonblock = False
    i = 0
    while i < len(words) and words[i].startswith("-") and words[i] != "-":
        w = words[i]
        if w.startswith("--"):
            name = w[2:].split("=", 1)[0]
            shared |= name == "shared"
            nonblock |= name in ("nb", "nonblock")
            if name in ("timeout", "conflict-exit-code") and "=" not in w:
                i += 1
        else:
            flags = w[1:]
            shared |= "s" in flags
            nonblock |= "n" in flags
            if flags[-1:] in ("w", "E"):
                i += 1
        i += 1
    if i >= len(words) or words[i].isdigit():
        return None, shared, nonblock
    return words[i], shared, nonblock


_OURS = None


def is_run_lock(path):
    global _OURS
    if _OURS is None:
        _OURS = {os.path.realpath(SOLO), os.path.realpath(TWO)}
    if os.path.isabs(path):
        return os.path.realpath(path) in _OURS
    # A relative path: the basename is all we can tell from here, and only for the real lock folder.
    default = os.path.expanduser("~/Documents/brawl-dolphin")
    return os.path.realpath(LOCK_DIR) == os.path.realpath(default) and \
        os.path.basename(path) in ("orca-run-solo.lock", "orca-run.lock")


def flock_processes(procs):
    """Every `flock` process on the old locks: [(pid, file, shared, nonblocking)]."""
    found = []
    for pid, (_, argv) in procs.items():
        if not argv or os.path.basename(argv[0]) != "flock":
            continue
        target, shared, nonblock = parse_flock(argv[1:])
        if target and is_run_lock(target):
            found.append((pid, target, shared, nonblock))
    return found


def descends_from(pid, ancestor, procs):
    seen = 0
    while pid in procs and seen < 64:
        if pid == ancestor:
            return True
        pid = procs[pid][0]
        seen += 1
    return pid == ancestor


def descendants(root, procs):
    kids = {}
    for pid, (ppid, _) in procs.items():
        kids.setdefault(ppid, []).append(pid)
    out, todo = [], [root]
    while todo:
        p = todo.pop()
        out.append(p)
        todo.extend(kids.get(p, []))
    return out


# ---- locks and tickets ----

def try_lock(path, mode):
    fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o644)
    try:
        fcntl.flock(fd, mode | fcntl.LOCK_NB)
        return fd
    except OSError as e:
        os.close(fd)
        if e.errno in (errno.EWOULDBLOCK, errno.EAGAIN):
            return None
        raise


def serial_path(name):
    return os.path.join(QUEUE, "serial-" + re.sub(r"[^A-Za-z0-9._-]+", "-", name) + ".lock")


class Ticket:
    """This run's place in the queue: orca-run-queue/<seq>.<class>.<state>.<pid>.ticket, locked by
    this process for as long as it lives. The state is in the name (a rename keeps the lock); the
    JSON inside only says what `status` shows."""

    def __init__(self, cls, info):
        os.makedirs(QUEUE, exist_ok=True)
        self.cls, self.pid, self.info = cls, os.getpid(), info
        with open(os.path.join(QUEUE, "seq.lock"), "a") as guard:
            fcntl.flock(guard, fcntl.LOCK_EX)
            seq_file = os.path.join(QUEUE, "seq")
            try:
                with open(seq_file) as f:
                    self.seq = int(f.read().strip() or 0) + 1
            except (FileNotFoundError, ValueError):
                self.seq = 1
            with open(seq_file + ".tmp", "w") as f:
                f.write(f"{self.seq}\n")
            os.replace(seq_file + ".tmp", seq_file)
            tmp = os.path.join(QUEUE, f".new-{self.pid}-{os.urandom(4).hex()}")
            self.fd = os.open(tmp, os.O_RDWR | os.O_CREAT | os.O_EXCL, 0o644)
            fcntl.flock(self.fd, fcntl.LOCK_EX)
            self.state = "waiting"
            self.write()
            self.path = self.name("waiting")
            os.rename(tmp, self.path)

    def name(self, state):
        return os.path.join(QUEUE, f"{self.seq:09d}.{self.cls}.{state}.{self.pid}.ticket")

    def write(self, **changes):
        self.info.update(changes)
        data = json.dumps(self.info).encode()
        os.ftruncate(self.fd, 0)
        os.pwrite(self.fd, data, 0)

    def set_state(self, state):
        path = self.name(state)
        os.rename(self.path, path)
        self.path, self.state = path, state

    def close(self):
        try:
            os.unlink(self.path)
        except FileNotFoundError:
            pass
        os.close(self.fd)


def read_ticket(fd):
    data = b""
    while True:
        chunk = os.pread(fd, 65536, len(data))
        if not chunk:
            break
        data += chunk
    try:
        return json.loads(data or b"{}")
    except ValueError:
        return {}


def scan(me=None):
    """Every live ticket in queue order; tickets whose owner is gone are removed on the way."""
    for _ in range(50):
        try:
            listing = os.listdir(QUEUE)
        except FileNotFoundError:
            return []
        names = sorted(n for n in listing if n.endswith(".ticket"))
        for n in listing:  # a ticket its run never finished making (killed in those microseconds)
            if n.startswith(".new-"):
                path = os.path.join(QUEUE, n)
                try:
                    if time.time() - os.stat(path).st_mtime > 60:
                        fd = try_lock(path, fcntl.LOCK_EX)
                        if fd is not None:
                            os.unlink(path)
                            os.close(fd)
                except OSError:
                    pass
        tickets, again = [], False
        for n in names:
            parts = n.split(".")
            if len(parts) != 5 or not parts[0].isdigit() or not parts[3].isdigit():
                continue
            t = {"seq": int(parts[0]), "cls": parts[1], "state": parts[2], "pid": int(parts[3])}
            if me is not None and t["seq"] == me.seq:
                tickets.append(dict(me.info, **t))
                continue
            path = os.path.join(QUEUE, n)
            try:
                fd = os.open(path, os.O_RDONLY)
            except FileNotFoundError:
                again = True  # renamed (waiting -> running) or finished while we looked: look again
                break
            try:
                try:
                    fcntl.flock(fd, fcntl.LOCK_SH | fcntl.LOCK_NB)
                except OSError as e:
                    if e.errno not in (errno.EWOULDBLOCK, errno.EAGAIN):
                        raise
                    tickets.append(dict(read_ticket(fd), **t))
                    continue
                try:  # nobody holds it: its run is gone
                    os.unlink(path)
                except FileNotFoundError:
                    pass
            finally:
                os.close(fd)
        if not again:
            return tickets
    return tickets


# ---- one run ----

class Run:
    def __init__(self, cls, cmd, label, serials, wait_max, yield_):
        self.cls, self.cmd, self.serials = cls, cmd, serials
        self.wait_max, self.yield_ = wait_max, yield_
        self.held = []
        self.ticket = None
        self.child = None
        self.signals = 0
        self.stop_signal = None
        self.info = {
            "label": label,
            "cwd": os.getcwd(),
            "cmd": shlex.join(cmd)[:400],
            "serial": serials,
            "since": time.time(),
            "reason": "",
        }

    def release(self):
        for fd in self.held:
            os.close(fd)
        self.held = []

    def take_locks(self):
        """All the locks this run needs, or none of them: a run never holds one while it waits."""
        mode = fcntl.LOCK_EX if self.cls == "perf" else fcntl.LOCK_SH
        for name in self.serials:
            fd = try_lock(serial_path(name), fcntl.LOCK_EX)
            if fd is None:
                self.release()
                return f"another run with --serial {name} is going"
            self.held.append(fd)
        for path in (SOLO, TWO):
            fd = try_lock(path, mode)
            if fd is None:
                self.release()
                if self.cls == "perf":
                    return f"a run holds {os.path.basename(path)} (a check finishing, or a plain flock outside the queue)"
                return f"a run outside the queue holds {os.path.basename(path)} (plain flock)"
            self.held.append(fd)
        if self.cls == "check":
            waiting = [p for p, _, shared, nb in flock_processes(processes()) if not shared and not nb]
            if waiting:
                self.release()
                return f"a plain flock run (pid {waiting[0]}) is waiting for the old lock; it goes first"
        return None

    def blocked(self):
        """Why this run cannot start now, or None once it holds its locks."""
        if luis_playing():
            return LUIS
        tickets = scan(self.ticket)
        ahead = [t for t in tickets if t["seq"] < self.ticket.seq and t["state"] == "waiting"]
        if ahead:
            first = ahead[0]
            why = first.get("reason") or "?"
            return f"{len(ahead)} ahead in the queue; the first ({first['cls']}, pid {first['pid']}) is waiting: {why}"
        load, free = machine()
        running = [t for t in tickets if t["state"] == "running" and t["seq"] != self.ticket.seq]
        checks = [t for t in running if t["cls"] == "check"]
        if self.cls == "perf":
            if not perf_roomy(load, free):
                return f"the machine is busy (load {load:.1f}, {free}% memory free)"
            if checks:
                return f"{len(checks)} check run(s) still going"
        else:
            if any(t["cls"] == "perf" for t in running):
                return "a perf run has the machine"
            allowed = checks_allowed(load, free)
            if len(checks) >= allowed:
                return f"{len(checks)} check run(s) going; load {load:.1f} and {free}% memory free allow {allowed}"
        why = self.take_locks()
        if why:
            return why
        if luis_playing():
            self.release()
            return LUIS
        self.start_note = f"load {load:.1f}, {free}% memory free"
        if self.cls == "check":
            self.start_note = f"check {len(checks) + 1} of up to {checks_allowed(load, free)} ({self.start_note})"
        return None

    def wait_turn(self):
        self.ticket = Ticket(self.cls, self.info)
        t0 = time.time()
        last, last_said = None, 0.0
        while True:
            why = self.blocked()
            if why is None:
                break
            now = time.time()
            if why != last:
                self.ticket.write(reason=why)
                say(f"waiting: {why}")
                last, last_said = why, now
            elif now - last_said >= REMIND_S:
                say(f"still waiting ({ago(now - t0)}): {why}")
                last_said = now
            if self.wait_max and now - t0 >= self.wait_max:
                say(f"gave up waiting after {ago(now - t0)}: {why}")
                return False
            time.sleep(POLL)
        self.ticket.write(reason="", started=time.time())
        self.ticket.set_state("running")
        waited = time.time() - t0
        say(f"start: {self.cls}, {self.start_note}" + (f"; waited {ago(waited)}" if waited >= 1 else ""))
        return True

    def stop_child(self, why, grace=20):
        procs = processes()
        tree = descendants(self.child.pid, procs) if self.child.pid in procs else [self.child.pid]
        for pid in tree:
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        try:
            self.child.wait(timeout=grace)
        except subprocess.TimeoutExpired:
            pass
        for pid in tree:
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        self.child.wait()
        say(f"stopped: {why}")

    def deadlock(self):
        """A flock of the command's own on the old locks: it waits for this run, which waits for it."""
        procs = processes()
        for pid, path, shared, nb in flock_processes(procs):
            if nb or (shared and self.cls == "check"):
                continue
            if descends_from(pid, os.getpid(), procs):
                return pid, path
        return None

    def run_command(self):
        env = dict(os.environ, ORCA_RUN_LOCK=f"{self.cls}:{os.getpid()}")
        t0 = time.time()
        self.child = subprocess.Popen(self.cmd, env=env)
        noted = False
        rc = None
        while rc is None:
            try:
                rc = self.child.wait(timeout=POLL)
                break
            except subprocess.TimeoutExpired:
                pass
            if self.stop_signal is not None:
                self.stop_child(f"signal {self.stop_signal}", grace=2 if self.signals > 1 else 20)
                return 128 + self.stop_signal
            if self.yield_ or (self.cls == "perf" and not noted):
                if luis_playing():
                    if self.yield_:
                        self.stop_child(f"{LUIS}; run it again once it closes")
                        return EXIT_YIELDED
                    say("note: Luis's Orca opened during this perf run; numbers from here on are suspect")
                    noted = True
            stuck = self.deadlock()
            if stuck:
                say(f"the command takes {os.path.basename(stuck[1])} itself (flock pid {stuck[0]}): it would "
                    "wait for this run forever. Drop that flock from the script, or run it without run-lock.py.")
                self.stop_child("deadlock")
                return EXIT_DEADLOCK
        say(f"done: exit {rc} after {ago(time.time() - t0)}")
        return rc if rc >= 0 else 128 - rc

    def on_signal(self, signum, _frame):
        self.signals += 1
        if self.child is None or self.child.poll() is not None:
            raise KeyboardInterrupt
        self.stop_signal = signum  # run_command stops the command's whole tree at its next look

    def main(self):
        for s in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            signal.signal(s, self.on_signal)
        try:
            if not self.wait_turn():
                return EXIT_GAVE_UP
            return self.run_command()
        except KeyboardInterrupt:
            if self.child is None:
                say("interrupted while waiting")
            return 130
        finally:
            self.release()
            if self.ticket:
                self.ticket.close()


def auto_serials(cmd):
    """Runs that must not overlap whatever their class: two signed-in players, or one shared dev
    game's queue (where two runs at once would match each other). Looks inside `bash -c '...'` too;
    a queue-e2e.mjs it cannot read counts as one with the shared dev game."""
    words = " ".join(cmd).replace("'", " ").replace('"', " ").split()
    names = []
    for word in words:
        base = os.path.basename(word)
        if base == "page-e2e.mjs" or (base == "queue-e2e.mjs" and "--slug" in words):
            names.append("signed-in-players")
        elif base == "queue-e2e.mjs":
            try:
                with open(word) as f:
                    per_run = "QE2E_DEV_SUFFIX" in f.read()
            except OSError:
                per_run = False
            if not per_run or os.environ.get("QE2E_DEV_SUFFIX") == "":
                names.append("queue-e2e-shared-dev-game")
    return list(dict.fromkeys(names))


def status(as_json, luis_only):
    playing = luis_playing()
    load, free = machine()
    tickets = scan() if os.path.isdir(QUEUE) else []
    procs = processes()
    outside = [{"pid": p, "file": os.path.basename(f), "shared": s}
               for p, f, s, nb in flock_processes(procs) if not nb]
    now = time.time()
    waiting = [t for t in tickets if t["state"] == "waiting"]
    if luis_only:
        if playing and waiting:
            oldest = min(t.get("since", now) for t in waiting)
            print(f"{len(waiting)} Orca run(s) waiting for Luis's Orca to close, the oldest for {ago(now - oldest)}")
            return 0
        print("Luis's Orca is open; nothing waits on it" if playing else "Luis's Orca is closed")
        return 1
    if as_json:
        print(json.dumps({"luis_orca_open": playing, "load": round(load, 2), "free": free,
                          "checks_allowed": checks_allowed(load, free), "tickets": tickets,
                          "outside_queue": outside}, indent=1))
        return 0
    print(f"Luis's Orca: {'OPEN' if playing else 'closed'} | load {load:.1f} | {free}% memory free | "
          f"checks at once now: {checks_allowed(load, free)}")
    if not tickets:
        print("queue: empty")
    for t in tickets:
        since = t.get("started") if t["state"] == "running" else t.get("since")
        line = f"{t['state']:8} {t['cls']:5} #{t['seq']} pid {t['pid']} {ago(now - (since or now)):>8}  {t.get('label', '?')}"
        if t["state"] == "waiting" and t.get("reason"):
            line += f"  [{t['reason']}]"
        print(line)
        if t.get("cwd"):
            print(f"{'':26}in {t['cwd'].replace(os.path.expanduser('~'), '~')}")
    for o in outside:
        print(f"outside the queue: flock pid {o['pid']} on {o['file']} ({'shared' if o['shared'] else 'exclusive'})")
    return 0


def main(argv):
    if argv[:1] == ["status"]:
        p = argparse.ArgumentParser(prog="run-lock.py status")
        p.add_argument("--json", action="store_true")
        p.add_argument("--luis", action="store_true", help="exit 0 if runs are waiting for Luis's Orca to close")
        a = p.parse_args(argv[1:])
        return status(a.json, a.luis)
    if "--" not in argv:
        sys.stderr.write("usage: run-lock.py check|perf [--label T] [--wait-max S] [--serial NAME] "
                         "[--yield] -- <command...>\n       run-lock.py status [--json | --luis]\n")
        return 2
    split = argv.index("--")
    p = argparse.ArgumentParser(prog="run-lock.py")
    p.add_argument("cls", choices=["check", "perf"])
    p.add_argument("--label")
    p.add_argument("--wait-max", type=float, default=0)
    p.add_argument("--serial", action="append", default=[])
    p.add_argument("--yield", dest="yield_", action="store_true")
    a = p.parse_args(argv[:split])
    cmd = argv[split + 1:]
    if not cmd:
        p.error("no command after --")

    outer = os.environ.get("ORCA_RUN_LOCK", "")
    if outer:
        ocls, _, opid = outer.partition(":")
        try:
            os.kill(int(opid), 0)
            alive = True
        except (ValueError, ProcessLookupError, PermissionError):
            alive = False
        if alive:
            if a.cls == "perf" and ocls == "check":
                say("a perf run cannot start inside a check run: run it outside, or take perf for the whole script")
                return 2
            say(f"already inside a {ocls} run (pid {opid}): running as part of it")
            os.execvp(cmd[0], cmd)

    serials = list(dict.fromkeys(a.serial + auto_serials(cmd)))
    label = a.label or " ".join(os.path.basename(w) if i < 2 else w for i, w in enumerate(cmd))[:160]
    return Run(a.cls, cmd, label, serials, a.wait_max, a.yield_).main()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
