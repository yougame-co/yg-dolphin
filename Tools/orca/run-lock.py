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
# served: a run keeps its place while it waits, so nothing that waits is starved. A check that is
# running when he opens it stops (exit 10) unless it was started with --no-yield.
#
# Options:
#   --label TEXT     what `status` shows for this run (default: the command)
#   --wait-max S     give up after waiting S seconds: exit 9 (default: wait as long as it takes)
#   --serial NAME    never beside another run with the same NAME (repeatable). Added by itself for
#                    runs that share accounts or a queue: queue-e2e.mjs with --slug and page-e2e.mjs
#                    (two signed-in players), and a queue-e2e.mjs that still uses one shared dev game.
#   --yield          stop the command if Luis's Orca opens while it runs (exit 10; run it again once
#                    it closes). The default for a check: three checks are six test Orcas beside his
#                    game, enough to make it stutter. A perf run takes no default stop; it notes the
#                    moment in its output instead (the numbers after it are suspect).
#   --no-yield       a run that must finish even if Luis opens his Orca: it notes the moment instead.
#
# Exit status: the command's own; 9 gave up waiting; 10 stopped because Luis opened his Orca (--yield,
# a check's default); 11 the command takes orca-run-solo.lock or orca-run.lock itself and would wait
# for this very run forever (drop that flock, or run the script without run-lock.py); 128+N after
# signal N; 2 usage. A signal to this process stops the command's whole tree; SIGKILL cannot, and the
# command would run on with its turn given away, so kill the process group instead. Under nohup a
# hangup is ignored as usual.
#
# How it shares with the old locks: a check holds orca-run-solo.lock and orca-run.lock shared and a
# perf run holds both exclusive, so a script that still takes them with plain `flock` (exclusive)
# acts as a perf run: it waits for the checks, and the checks wait for it. While such a script is
# waiting for the locks, no new check starts, so it is never starved either. The guard that holds
# both locks while Luis plays (luis-orca-guard.sh) works as before. The queue itself is
# orca-run-queue/ beside the locks: one ticket file per run, locked by its owner for as long as it
# lives, so a run that dies leaves no ticket behind.
#
# The external SSD (perf runs only, 2026-10-06): the USB SSD that holds the worktree image
# (~/ClaudeWorktrees) and the discs stalls for 10-180 s while other agents copy GBs or run npm
# installs on it, and an Orca read from it waits as long: Project+ streams its music from the SD
# image on the emulation thread, so 0.3.28's Mac<->PC run froze for 36-44 s. A perf run therefore
#  - runs Project+ from the internal copy (ORCA_PERF_PPLUS_DIR, default
#    ~/Documents/brawl-dolphin/pplus-internal/run) when its command names the launcher on an
#    external disk and the internal copy is the same release;
#  - waits until the SSD has gone ORCA_SSD_QUIET_S (60) without a stall over ORCA_SSD_STALL_S (2):
#    the kernel's APFS lines for the worktree image's disk ("disk11 tx ... took N us", written when
#    a stall ends), and a timed 1 MB read around the page cache (one stuck now) of
#    ~/ClaudeWorktrees/.orca-ssd-probe and of the largest file the command reads from any other
#    external disk (the discs);
#  - keeps timing those reads every 5 s while it runs, and when it ends prints "SSD: TAINTED ..."
#    with every stall that overlapped it, or "SSD: no stall".
#    Measured 2026-10-06 while 2 GB went into the image: APFS flushes of 2-5 s, reads of the probe
#    file 32 ms -> 414 ms (median), the disc's on the exFAT side 26 -> 74 ms. The log is what sees a
#    stall; a read over 2 s means a bad one is going on right now.
# `status` shows the wait and a reminder to keep the SSD quiet while a perf run is queued or going.
# ORCA_SSD_WATCH (paths, ':'-separated, default ~/ClaudeWorktrees) names the SSD's APFS disks to
# watch, ORCA_SSD_PROBE adds files to time, ORCA_SSD_GUARD=0 turns all of this off.
#
# Environment: ORCA_LOCK_DIR (default ~/Documents/brawl-dolphin), ORCA_CHECK_SLOTS (3). Tests use
# ORCA_RUN_LOCK_POLL (seconds between looks, 2), ORCA_RUN_LOCK_TEST_PLAYING (a file: Luis "plays"
# while it exists), ORCA_RUN_LOCK_TEST_LOAD (a file holding "<load> <free %>") and
# ORCA_RUN_LOCK_TEST_SSD (a file of "<start> <end> <what>" stalls, read instead of the kernel log).
import argparse
import errno
import fcntl
import json
import os
import random
import re
import shlex
import signal
import subprocess
import sys
import threading
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
SERIAL_BUSY = "another run with --serial"
EXIT_GAVE_UP, EXIT_YIELDED, EXIT_DEADLOCK = 9, 10, 11
SSD_GUARD = os.environ.get("ORCA_SSD_GUARD", "1") != "0"
SSD_STALL_S = float(os.environ.get("ORCA_SSD_STALL_S") or 2)
SSD_QUIET_S = float(os.environ.get("ORCA_SSD_QUIET_S") or 60)
SSD_PROBE_EVERY_S = 5
SSD_LOG_EVERY_S = 10
SSD_QUIET_HINT = ("a perf run is queued or going: keep the external SSD quiet (no multi-GB copies onto it, "
                  "no npm installs in worktrees on it) until it is done")
PPLUS_INTERNAL = os.environ.get("ORCA_PERF_PPLUS_DIR", "~/Documents/brawl-dolphin/pplus-internal/run")
F_NOCACHE = 48  # fcntl.h: read around the page cache


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
            except (FileNotFoundError, ValueError):  # never behind a ticket already there
                taken = [int(n.split(".")[0]) for n in os.listdir(QUEUE)
                         if n.endswith(".ticket") and n.split(".")[0].isdigit()]
                self.seq = max(taken, default=0) + 1
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


# ---- the external SSD (perf runs) ----

def internal_devices():
    devs = set()
    for p in ("/", os.path.expanduser("~"), os.environ.get("TMPDIR") or "/tmp"):
        try:
            devs.add(os.stat(p).st_dev)
        except OSError:
            pass
    return devs


def on_external_disk(path, internal):
    try:
        return os.stat(path).st_dev not in internal
    except OSError:
        return False


def command_files(cmd):
    """Existing files the command names: its words, `--opt=<file>` and `VAR=<file>`, and the words
    of a script string (`bash -c '...'`)."""
    words = []
    for arg in cmd:
        words.append(arg)
        if "=" in arg:
            words.append(arg.split("=", 1)[1])
        if any(c in arg for c in " \n;'\""):
            try:
                words.extend(shlex.split(arg, comments=True))
            except ValueError:
                pass
    out = []
    for w in words:
        w = os.path.expanduser(w)
        if os.path.isabs(w) and os.path.isfile(w) and w not in out:
            out.append(w)
    return out


def disk_of(path):
    """The whole disk an APFS volume sits on, as the kernel log names it: disk11s1 -> disk11."""
    try:
        dev = subprocess.run(["/usr/bin/stat", "-f", "%Sd", path], capture_output=True, text=True,
                             timeout=10).stdout.strip()
    except (OSError, subprocess.TimeoutExpired):
        return None
    m = re.match(r"(disk\d+)", dev)
    return m.group(1) if m else None


APFS_LINE = re.compile(r"^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)(\.\d+)? .*?\(apfs\) (\w+):\d+: (disk\d+) .*?(\d+) us\b")


def apfs_stalls(disks, since):
    """[(start, end, what)]: the kernel's APFS lines for these disks ("disk11 tx xid N took M us to
    flush", "... waited M us to open tx") of at least SSD_STALL_S that ended after `since`. The
    kernel writes one when the wait is over, so a stall still going shows up only in the reads."""
    fake = os.environ.get("ORCA_RUN_LOCK_TEST_SSD")
    lines = []
    if fake:
        try:
            with open(fake) as f:
                for line in f:
                    parts = line.split(None, 2)
                    if len(parts) == 3:
                        lines.append((float(parts[0]), float(parts[1]), parts[2].strip()))
        except (OSError, ValueError):
            pass
        return [s for s in lines if s[1] >= since and s[1] - s[0] >= SSD_STALL_S]
    if not disks:
        return []
    pred = 'process == "kernel" AND (' + " OR ".join(f'eventMessage CONTAINS "{d} "' for d in disks) + ")"
    start = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(since - 1))
    try:
        out = subprocess.run(["/usr/bin/log", "show", "--style", "compact", "--start", start,
                              "--predicate", pred], capture_output=True, text=True, timeout=120).stdout
    except (OSError, subprocess.TimeoutExpired):
        return []
    for line in out.splitlines():
        m = APFS_LINE.match(line)
        if not m:
            continue
        end = time.mktime(time.strptime(m.group(1), "%Y-%m-%d %H:%M:%S")) + float(m.group(2) or 0)
        secs = int(m.group(5)) / 1e6
        if end >= since and secs >= SSD_STALL_S:
            lines.append((end - secs, end, f"{m.group(4)} APFS {m.group(3)}"))
    return lines


def read_probe(path):
    """One 1 MB read at a random offset around the page cache (on APFS a page already cached is
    still served from memory; the SSD's exFAT side reads the drive every time)."""
    fd = os.open(path, os.O_RDONLY)
    try:
        try:
            fcntl.fcntl(fd, F_NOCACHE, 1)
        except OSError:
            pass
        size = os.fstat(fd).st_size
        off = random.randrange(0, max(1, size >> 20)) << 20
        try:
            os.pread(fd, 1 << 20, off)
        except OSError:
            os.read(fd, 1 << 20)
    finally:
        os.close(fd)


def size_of(path):
    try:
        return os.stat(path).st_size
    except OSError:
        return 0


PROBE_NAME, PROBE_MB = ".orca-ssd-probe", 64


def probe_file(folder):
    """<folder>/.orca-ssd-probe, 64 MB written once around the page cache, so a timed read of it
    always goes to the drive (a file Orca reads, like the SD image, sits in memory). None if it
    cannot be made."""
    path = os.path.join(folder, PROBE_NAME)
    if size_of(path) >= PROBE_MB << 20:
        return path
    tmp = f"{path}.{os.getpid()}"
    try:
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
        try:
            fcntl.fcntl(fd, F_NOCACHE, 1)
            for _ in range(PROBE_MB):
                os.write(fd, os.urandom(1 << 20))
        finally:
            os.close(fd)
        os.replace(tmp, path)
        return path
    except OSError:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        return None


def clock(t):
    return time.strftime("%H:%M:%S", time.localtime(t))


class SSD:
    """The external SSD's stalls, for one perf run: what it waits for, and what it reports."""

    def __init__(self, cmd):
        internal = internal_devices()
        self.external = [f for f in command_files(cmd) if on_external_disk(f, internal)]
        for f in list(self.external):  # Orca reads the SD image next to a launcher
            sd = os.path.join(os.path.dirname(f), "sd.raw")
            if f.endswith(".dol") and os.path.isfile(sd) and sd not in self.external:
                self.external.append(sd)
        watch = [os.path.expanduser(p) for p in os.environ.get("ORCA_SSD_WATCH", "~/ClaudeWorktrees").split(":") if p]
        watch = [p for p in watch if os.path.isdir(p) and on_external_disk(p, internal)]
        # One file per disk to time: on a watched APFS disk a probe file of its own that nothing reads
        # into the page cache, elsewhere the largest file the run reads from that disk.
        per_dev = {}
        for f in [probe_file(p) for p in watch] + sorted(self.external, key=size_of, reverse=True):
            try:
                per_dev.setdefault(os.stat(f).st_dev, f)
            except (OSError, TypeError):
                continue
        self.files = list(per_dev.values())
        self.files += [p for p in os.environ.get("ORCA_SSD_PROBE", "").split(":") if p and p not in self.files]
        disks = [disk_of(p) for p in watch + self.external]
        self.disks = sorted(set(d for d in disks if d))
        self.active = bool(self.files or self.disks or os.environ.get("ORCA_RUN_LOCK_TEST_SSD"))
        self.slow = []          # [(start, end, what)] timed reads that took SSD_STALL_S or more
        self.inflight = None    # (start, file) of the read going now
        self.lock = threading.Lock()
        self.round_lock = threading.Lock()
        self.logged = (0.0, [])  # (when, apfs_stalls) cache
        self.on_slow = None
        self.stop = threading.Event()
        self.thread = None

    def describe(self):
        parts = []
        if self.disks:
            parts.append(f"the kernel's APFS lines for {', '.join(self.disks)}")
        if self.files:
            parts.append("timed reads of " + ", ".join(os.path.basename(f) for f in self.files))
        return " and ".join(parts) or "a test log"

    def _round(self):
        """Time one read of each file, unless a round is still going (a stalled read)."""
        if not self.round_lock.acquire(blocking=False):
            return
        try:
            for f in self.files:
                t0 = time.time()
                with self.lock:
                    self.inflight = (t0, f)
                try:
                    read_probe(f)
                except OSError:
                    pass  # gone (the drive dropped out?): the run's own reads will say so
                t1 = time.time()
                with self.lock:
                    self.inflight = None
                    if t1 - t0 >= SSD_STALL_S:
                        what = f"a 1 MB read of {os.path.basename(f)} took {t1 - t0:.1f} s"
                        self.slow.append((t0, t1, what))
                        if self.on_slow:
                            self.on_slow(t0, t1, what)
        finally:
            self.round_lock.release()

    def stalling_now(self):
        with self.lock:
            if self.inflight and time.time() - self.inflight[0] >= SSD_STALL_S:
                return self.inflight
        return None

    def recent(self, since):
        now = time.time()
        if now - self.logged[0] >= SSD_LOG_EVERY_S:
            self.logged = (now, apfs_stalls(self.disks, now - SSD_QUIET_S - SSD_LOG_EVERY_S))
        with self.lock:
            slow = list(self.slow)
        return [s for s in self.logged[1] + slow if s[1] >= since]

    def why_wait(self):
        """Why a perf run should not start yet, or None once the SSD has been quiet SSD_QUIET_S."""
        if not self.active:
            return None
        if self.files:
            t = threading.Thread(target=self._round, daemon=True)
            t.start()
            t.join(SSD_STALL_S)
        going = self.stalling_now()
        if going:
            return (f"the external SSD is stalling now: a 1 MB read of {os.path.basename(going[1])} has "
                    f"waited since {clock(going[0])}")
        recent = self.recent(time.time() - SSD_QUIET_S)
        if recent:
            start, end, what = max(recent, key=lambda s: s[1])
            return (f"the external SSD stalled {end - start:.0f} s at {clock(start)} ({what}); a perf run "
                    f"waits for {SSD_QUIET_S:.0f} s without a stall over {SSD_STALL_S:g} s")
        return None

    def watch_run(self, on_slow):
        self.on_slow = on_slow
        if not self.files:
            return

        def loop():
            while not self.stop.wait(SSD_PROBE_EVERY_S):
                self._round()

        self.thread = threading.Thread(target=loop, daemon=True)
        self.thread.start()

    def report(self, t0, t1):
        """Every stall that overlapped [t0, t1], as one line for the run's output."""
        self.stop.set()
        stalls = [s for s in apfs_stalls(self.disks, t0) if s[0] <= t1]
        with self.lock:
            stalls += [s for s in self.slow if s[1] >= t0 and s[0] <= t1]
        going = self.stalling_now()
        if going:
            stalls.append((going[0], time.time(),
                           f"a 1 MB read of {os.path.basename(going[1])} still waiting at the end"))
        if not stalls:
            return f"SSD: no stall over {SSD_STALL_S:g} s during this perf run ({self.describe()})"
        stalls.sort()
        shown = "; ".join(f"{clock(s)} {e - s:.1f} s ({what})" for s, e, what in stalls[:6])
        more = f"; and {len(stalls) - 6} more" if len(stalls) > 6 else ""
        return (f"SSD: TAINTED: the external SSD stalled {len(stalls)} time(s) during this perf run, so its "
                f"timings are suspect: {shown}{more}")


def internal_pplus(cmd):
    """The command with a Project+ launcher on an external disk swapped for the internal copy of the
    same release (same launcher bytes, same SD image size and date), and what was swapped."""
    if not PPLUS_INTERNAL:
        return cmd, []
    home = os.path.expanduser(PPLUS_INTERNAL)
    internal = internal_devices()
    out, swapped = [], []
    for arg in cmd:
        key, eq, path = arg.partition("=") if arg.startswith("-") and "=" in arg else ("", "", arg)
        mine = os.path.join(home, os.path.basename(path))
        if path.endswith(".dol") and os.path.isfile(path) and on_external_disk(path, internal) and \
                os.path.isfile(mine) and not on_external_disk(mine, internal):
            try:
                theirs_sd, mine_sd = (os.stat(os.path.join(os.path.dirname(p), "sd.raw")) for p in (path, mine))
                with open(path, "rb") as a, open(mine, "rb") as b:
                    same = a.read() == b.read() and theirs_sd.st_size == mine_sd.st_size and \
                        int(theirs_sd.st_mtime) == int(mine_sd.st_mtime)
            except OSError:
                same = False
            if same:
                out.append(key + eq + mine)
                swapped.append((path, mine))
                continue
        out.append(arg)
    return out, swapped


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
        self.ssd = SSD(cmd) if cls == "perf" and SSD_GUARD else None

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
                return f"{SERIAL_BUSY} {name} is going"
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

    def may_pass(self, t):
        """A check may go past a check ahead of it that waits only for its --serial key (another run
        with that key is going), when this one does not need the key: it would not start sooner if
        this one waited. Once the key frees, that check waits for something else and is passed no
        longer."""
        why = t.get("reason") or ""
        if self.cls != "check" or t["cls"] != "check" or not why.startswith(SERIAL_BUSY):
            return False
        return not set(t.get("serial") or []) & set(self.serials)

    def blocked(self):
        """Why this run cannot start now, or None once it holds its locks."""
        tickets = scan(self.ticket)
        ahead = [t for t in tickets if t["seq"] < self.ticket.seq and t["state"] == "waiting"
                 and not self.may_pass(t)]
        if ahead:  # only the first in line looks at the machine; the rest repeat what it waits for
            first = ahead[0]
            text = f"{len(ahead)} ahead in the queue; the first is a {first['cls']} run (pid {first['pid']})"
            return text + (f", waiting: {first['reason']}" if first.get("reason") else "")
        if luis_playing():
            return LUIS
        load, free = machine()
        running = [t for t in tickets if t["state"] == "running" and t["seq"] != self.ticket.seq]
        checks = [t for t in running if t["cls"] == "check"]
        if any(t["cls"] == "perf" for t in running):
            return "a perf run has the machine"
        if self.cls == "perf":
            if not perf_roomy(load, free):
                return f"the machine is busy (load {load:.1f}, {free}% memory free)"
            if checks:
                return f"{len(checks)} check run(s) still going"
            why = self.ssd.why_wait() if self.ssd else None
            if why:
                return why
        else:
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
        elif self.ssd and self.ssd.active:
            self.start_note += f", the external SSD quiet for {SSD_QUIET_S:.0f} s"
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

    def note_ssd(self, start, end, what):
        try:
            self.ticket.write(ssd=f"tainted: the SSD stalled {end - start:.0f} s at {clock(start)} ({what})")
        except (OSError, ValueError):
            pass

    def run_command(self):
        t0 = time.time()
        watch = self.ssd is not None and self.ssd.active
        if watch:
            self.ssd.watch_run(self.note_ssd)
        try:
            return self.run_child()
        finally:
            if watch:
                say(self.ssd.report(t0, time.time()))

    def run_child(self):
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
            if (self.yield_ or not noted) and luis_playing():
                if self.yield_:
                    self.stop_child(f"{LUIS}; run it again once it closes")
                    return EXIT_YIELDED
                say(f"note: Luis's Orca opened during this {self.cls} run (--no-yield); "
                    + ("numbers from here on are suspect" if self.cls == "perf" else "it runs on beside his game"))
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
        if self.child is None:
            self.stop_signal = signum
            raise KeyboardInterrupt
        if self.child.poll() is None:
            self.stop_signal = signum  # run_command stops the command's whole tree at its next look

    def main(self):
        for s in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            if signal.getsignal(s) != signal.SIG_IGN:  # nohup's ignored SIGHUP stays ignored
                signal.signal(s, self.on_signal)
        try:
            if self.ssd and self.ssd.external:
                say("note: this perf run reads " + ", ".join(os.path.basename(f) for f in self.ssd.external)
                    + " from an external disk; a stall there freezes Orca, so timed runs keep their files on the "
                    "internal disk where they can (Project+: " + PPLUS_INTERNAL + ")")
            if not self.wait_turn():
                return EXIT_GAVE_UP
            return self.run_command()
        except KeyboardInterrupt:
            say("interrupted while waiting")
            return 128 + (self.stop_signal or signal.SIGINT)
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
            suffix = os.environ.get("QE2E_DEV_SUFFIX")
            if not per_run:
                names.append("queue-e2e-shared-dev-game")
            elif suffix is not None:  # a suffix of its own choosing: runs that share it share a game
                names.append(f"queue-e2e-dev-{suffix.lower()}" if re.search(r"[A-Za-z0-9]", suffix)
                             else "queue-e2e-shared-dev-game")
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
        if t.get("ssd"):
            print(f"{'':26}{t['ssd']}")
    if any(t["cls"] == "perf" for t in tickets):
        print(SSD_QUIET_HINT)
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
                         "[--yield | --no-yield] -- <command...>\n       run-lock.py status [--json | --luis]\n")
        return 2
    split = argv.index("--")
    p = argparse.ArgumentParser(prog="run-lock.py")
    p.add_argument("cls", choices=["check", "perf"])
    p.add_argument("--label")
    p.add_argument("--wait-max", type=float, default=0)
    p.add_argument("--serial", action="append", default=[])
    y = p.add_mutually_exclusive_group()
    y.add_argument("--yield", dest="yield_", action="store_const", const=True,
                   help="stop the command if Luis's Orca opens (exit 10); the default for check")
    y.add_argument("--no-yield", dest="yield_", action="store_const", const=False,
                   help="run on if Luis's Orca opens; the default for perf")
    a = p.parse_args(argv[:split])
    if a.yield_ is None:
        a.yield_ = a.cls == "check"
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

    if a.cls == "perf" and SSD_GUARD:
        cmd, swapped = internal_pplus(cmd)
        for theirs, mine in swapped:
            say(f"Project+ from the internal copy {mine}, not {theirs} on the external SSD "
                "(ORCA_PERF_PPLUS_DIR= keeps the path as given)")
    serials = list(dict.fromkeys(a.serial + auto_serials(cmd)))
    label = a.label or " ".join(os.path.basename(w) if i < 2 else w for i, w in enumerate(cmd))[:160]
    return Run(a.cls, cmd, label, serials, a.wait_max, a.yield_).main()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
