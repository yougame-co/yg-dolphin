#!/usr/bin/env python3
# Copyright 2026 YouGame
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bring upstream Dolphin fixes into Orca fast: triage, branch from the live release, pick, build,
test against the live build, then print the rest of the release with exact commands.
Runbook: ORCA_NOTES.md, "Upstream Dolphin updates". Private tooling (never published).

Run the stable copy, from a checkout of origin/main (the live release branch may predate this
script):  T=~/Documents/brawl-dolphin/orca-upstream-tools; git -C $T fetch -q origin &&
          git -C $T checkout -q --detach origin/main; python3 $T/Tools/orca/upstream-sync.py ...

  status                          live build, upstream heads, the sync worktree, last triage
  triage [--json F] [--dashboard [--save]]
                                  every upstream commit Orca lacks: class (security, crash, platform,
                                  perf, other), whether it ships in Orca, overlap with Orca's own
                                  changes, layer, and whether it picks cleanly onto the live release
  start COMMIT... | --range A..B | --merge REF  [--base REV] [--name BRANCH]
  start --continue                after resolving a conflict (git cherry-pick --continue first)
                                  branch upstream-sync-<date> from the live release in the sync
                                  worktree, cherry-pick -x (or merge a hotfix tag), submodules
  build                           dolphin-emu-nogui + tests (dev flags), under build.lock
  package [--version V]           the release-config unsigned Orca.app (orca-package-macos.sh)
  test [unit] [hash] [sync] [--app Orca.app] [--live-app Orca.app]
                                  Orca* gtests; hashlogs equal to the live build's; YG_SYNCTEST=7,
                                  0 RAM mismatches (every Orca run through run-lock.py check)
  check [--version V]             build, unit tests, package, hash + sync on the package
  gate --cand Orca.app [--version V]
                                  writes the two-Orca perf gate (live vs candidate) to gate-<V>/
  next [--version V]              the remaining release steps, with exact commands

Nothing here signs, uploads, flips a manifest or pushes: those stay in the release steps `next`
prints, so a person (or the release workflow) decides each one.

Exit status: 0 ok; 1 a step failed; 2 usage or setup; triage exits 3 when a security or crash
commit that may reach Orca is new since the last triage.
"""

import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import layerlib  # noqa: E402

HOME = os.path.expanduser("~")
BD = os.environ.get("ORCA_BD") or os.path.join(HOME, "Documents", "brawl-dolphin")
WATCH = os.path.join(BD, "upstream-watch")
STATE = os.path.join(WATCH, "state.json")
SYNC_WT = os.environ.get("ORCA_SYNC_WORKTREE") or os.path.join(BD, "orca-upstream-sync")
RUN_LOCK = os.path.join(BD, "run-lock.py")
BUILD_LOCK = os.path.join(BD, "build.lock")
FLOCK = shutil.which("flock") or "/opt/homebrew/bin/flock"
CMAKE_FLAGS = os.path.join(BD, "orca-cmake-flags.txt")
CCACHE_DIR = os.path.join(BD, "ccache")
SITE = os.environ.get("ORCA_SITE_URL") or "https://yougame.co"
YOUGAME = os.environ.get("YOUGAME_REPO") or os.path.join(HOME, "Documents", "YouGame")
UPSTREAM_REF = "upstream/master"
UPSTREAM_WEB = "https://github.com/dolphin-emu/dolphin"
UA = ("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) "
      "Chrome/140.0 Safari/537.36")
NO_HIDAPI = {"SDL_JOYSTICK_HIDAPI": "0", "SDL_JOYSTICK_HIDAPI_GAMECUBE": "0", "SDL_HIDAPI_LIBUSB": "0"}
ROMS = os.path.join(HOME, "Documents", "ROMs", "Wii")
DISC = os.environ.get("ORCA_TEST_DISC") or os.path.join(ROMS, "Super Smash Bros. Brawl (USA, Canada) (Rev 2).wbfs")
DISC_REV1 = os.environ.get("ORCA_TEST_DISC_REV1") or os.path.join(ROMS, "Super Smash Bros. Brawl (USA) (Rev 1).rvz")
PPLUS = os.environ.get("ORCA_PPLUS_LAUNCHER") or os.path.join(BD, "pplus-internal", "run",
                                                               "Project+ Netplay Launcher.dol")
MIN_FREE_GB = 10
LOAD_OUTSIDE, LOAD_INSIDE = 18, 20


def die(msg, code=2):
    print("upstream-sync: " + msg, file=sys.stderr)
    sys.exit(code)


def now():
    return datetime.datetime.now().astimezone().strftime("%Y-%m-%d %H:%M:%S %Z")


def say(msg):
    print("[%s] %s" % (datetime.datetime.now().strftime("%H:%M:%S"), msg), flush=True)


REPO = layerlib.repo_root(HERE)


def git(*args, repo=None, check=True):
    return layerlib.git(repo or REPO, *args, check=check)


def git_ok(*args, repo=None):
    return subprocess.run(["git", "-C", repo or REPO] + list(args), stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode == 0


def rev(ref, repo=None):
    return git("rev-parse", "--verify", "--quiet", ref + "^{commit}", repo=repo, check=False).strip()


# --- state -------------------------------------------------------------------------------------


def load_state():
    try:
        with open(STATE, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_state(state, *keys):
    """Writes `keys` of state (all of them when none are named) over a fresh read of the file, so a
    triage and a sync step running at once don't undo each other's records."""
    os.makedirs(WATCH, exist_ok=True)
    merged = load_state() if keys else {}
    for k in keys or list(state):
        if k in state:
            merged[k] = state[k]
    fd, tmp = tempfile.mkstemp(dir=WATCH, prefix=".state.")
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump(merged, f, indent=1, sort_keys=True)
        f.write("\n")
    os.replace(tmp, STATE)


# --- the live build ----------------------------------------------------------------------------


def http_get(url, timeout=60):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def live_manifest():
    try:
        m = json.loads(http_get("%s/api/orca/manifest?check=%d" % (SITE, int(time.time()))))
    except Exception as e:  # noqa: BLE001 (any failure means: no live build known)
        die("could not read the live manifest from %s: %s" % (SITE, e))
    if not re.fullmatch(r"[0-9a-f]{40}", m.get("commit", "")) or not m.get("version"):
        die("the live manifest has no version or commit: %r" % m)
    return m


def ensure_commit(sha):
    if not git_ok("cat-file", "-e", sha + "^{commit}"):
        say("fetching origin for %s" % sha[:10])
        git("fetch", "-q", "origin")
    if not git_ok("cat-file", "-e", sha + "^{commit}"):
        die("commit %s is not in origin" % sha)


def resolve_base(base_arg):
    """(commit, version) of the release to build on: --base, else the live manifest's commit."""
    if base_arg:
        sha = rev(base_arg)
        if not sha:
            die("no such revision: %s" % base_arg)
        return sha, None
    m = live_manifest()
    ensure_commit(m["commit"])
    return m["commit"], m["version"]


def bump(version):
    parts = version.split(".")
    parts[-1] = str(int(parts[-1]) + 1)
    return ".".join(parts)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def ensure_live_app(m=None):
    """The live Mac build, downloaded from the site and checked against the manifest's sha256:
    upstream-watch/live/<version>/Orca.app (never the app's own copy: run-lock counts that as Luis
    playing)."""
    m = m or live_manifest()
    v = m["version"]
    d = os.path.join(WATCH, "live", v)
    app = os.path.join(d, "Orca.app")
    if os.path.exists(os.path.join(app, "Contents", "MacOS", "Orca")):
        return app, m
    b = m["builds"]["macos-arm64"]
    os.makedirs(d, exist_ok=True)
    z = os.path.join(d, "orca-macos-arm64.zip")
    say("downloading live Orca %s for macOS (%d bytes)" % (v, b["bytes"]))
    data = http_get("%s/api/orca/download/%s/macos-arm64" % (SITE, v), timeout=600)
    with open(z, "wb") as f:
        f.write(data)
    got = sha256_file(z)
    if got != b["sha256"] or os.path.getsize(z) != b["bytes"]:
        die("the live zip is %s (%d bytes), the manifest says %s (%d)" %
            (got, os.path.getsize(z), b["sha256"], b["bytes"]))
    subprocess.run(["ditto", "-x", "-k", z, d], check=True)
    if not os.path.exists(os.path.join(app, "Contents", "MacOS", "Orca")):
        die("no Orca.app in the live zip")
    say("live Orca %s at %s (sha256 ok)" % (v, app))
    return app, m


# --- triage ------------------------------------------------------------------------------------

CLASSES = [
    ("security", re.compile(
        r"secur|vulnerab|exploit|\bcve-\d|\bghsa-|overflow|underflow|out[- ]of[- ]bounds|\boob\b|"
        r"bounds?[- ]check|sanity[- ]check|untrusted|malicious|malformed|oversized|sanitiz|validat|"
        r"use[- ]after[- ]free|\buaf\b|double[- ]free|uninitiali[sz]ed|info(rmation)?[- ]leak|"
        r"traversal|\bheap\b|stack (buffer|overflow|write|read)|\breject", re.I)),
    ("crash", re.compile(
        r"crash|segfault|sigsegv|sigtrap|sigbus|\bassert|\babort|\bhangs?\b|deadlock|\bfreez|"
        r"null ?pointer|nullptr|\bexception|\bthrows?\b|\bUB\b|undefined behaviou?r", re.I)),
    ("platform", re.compile(
        r"macos|\bmac\b|darwin|windows|win32|\blinux|android|openbsd|freebsd|arm64|aarch64|x86|"
        r"memarena|metal|moltenvk|vulkan|\bd3d|direct3d|opengl|\bsdl\b|msvc|clang|\bgcc\b|cmake|"
        r"flatpak|externals", re.I)),
    ("perf", re.compile(r"\bperf\b|performance|faster|speed ?up|optimi[sz]|latency|stutter|\bslow", re.I)),
]

# Paths that never reach an Orca build (Orca ships dolphin-emu-nogui only: no Qt, Android,
# updater, DolphinTool) or are another game's data.
NOT_SHIPPED = ("Source/Android/", "Source/Core/DolphinQt/", "Source/Core/MacUpdater/",
               "Source/Core/WinUpdater/", "Source/Core/UpdaterCommon/", "Source/Core/DolphinTool/",
               "Source/DSPTool/", "Source/DSPSpy/", "Source/UnitTests/", "Flatpak/", "Languages/",
               "docs/", ".github/", ".tx/", "Tools/", "Externals/Qt/", "Externals/rcheevos/",
               "Externals/libadrenotools/", "CMakeSettings.json", "AndroidSetup.md", "Readme.md",
               "Contributing.md", "CODE_OF_CONDUCT.md", "BuildMacOSUniversalBinary.py")
# Built into Orca but not reached while a session runs (a hint for the person triaging; each is
# checked again before relying on it).
UNREACHABLE = (
    ("Source/Core/Core/NetPlay", "Dolphin NetPlay (Orca never constructs NetPlayClient/Server)"),
    ("Source/Core/Common/Traversal", "NetPlay traversal"),
    ("Source/Core/Core/HW/EXI/EXI_DeviceMic", "the EXI mic (sessions force EXI slots to None)"),
    ("Source/Core/Common/Assembler/", "the Gekko assembler (Qt debugger)"),
    ("Source/Core/Core/AchievementManager", "RetroAchievements (built off)"),
    ("Source/Core/Core/Debugger/", "the debugger"),
    ("Source/Core/Core/HW/GBA", "GBA link"),
    ("Source/Core/Core/HW/SI/SI_DeviceGBA", "GBA link"),
    ("Source/Core/InputCommon/ControllerInterface/Android/", "Android input"),
)
LAYER_ORDER = ["game-data", "game-logic", "engine", "engine-sensitive"]


def classify(message, files):
    out = [name for name, rx in CLASSES if rx.search(message)]
    if "platform" not in out and any(f.startswith("Externals/") for f in files):
        out.append("platform")
    return out or ["other"]


def shipped(path):
    if path.startswith("Data/Sys/GameSettings/"):
        return os.path.basename(path).startswith("RSB")  # Brawl's own settings ship and count
    return not path.startswith(NOT_SHIPPED)


def reach(files):
    ships = [f for f in files if shipped(f)]
    if not ships:
        return "not-shipped", []
    why = []
    for f in ships:
        hit = next((w for p, w in UNREACHABLE if f.startswith(p)), None)
        if hit is None:
            return "ships", []
        if hit not in why:
            why.append(hit)
    return "unreachable?", why


def commit_files(c):
    return [p for p in git("diff-tree", "-r", "--no-commit-id", "--name-only", "--no-renames", c).splitlines() if p]


def pick_test(base, c):
    """Would `git cherry-pick c` onto base apply cleanly? (a three-way merge, nothing written)"""
    p = subprocess.run(["git", "-C", REPO, "merge-tree", "--write-tree", "--name-only", "--no-messages",
                        "--merge-base=%s^" % c, base, c], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       text=True)
    if p.returncode == 0:
        return "clean", []
    if p.returncode == 1:
        return "conflict", [l for l in p.stdout.splitlines()[1:] if l.strip()]
    return "error", [p.stderr.strip()[:200]]


def series_test(base, commits):
    """Would picking `commits` in order onto base apply cleanly? Each step is a three-way merge onto
    the previous step's result (throwaway commit objects, no ref moves)."""
    cur = base
    for c in commits:
        p = subprocess.run(["git", "-C", REPO, "merge-tree", "--write-tree", "--name-only", "--no-messages",
                            "--merge-base=%s^" % c, cur, c], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if p.returncode != 0:
            return "conflict at %s" % c[:10], [l for l in p.stdout.splitlines()[1:] if l.strip()]
        tree = p.stdout.split()[0]
        env = dict(os.environ, GIT_AUTHOR_NAME="upstream-sync", GIT_AUTHOR_EMAIL="upstream-sync@invalid",
                   GIT_COMMITTER_NAME="upstream-sync", GIT_COMMITTER_EMAIL="upstream-sync@invalid")
        q = subprocess.run(["git", "-C", REPO, "commit-tree", tree, "-p", cur, "-m", "upstream-sync series test"],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
        if q.returncode != 0:
            return "error", [q.stderr.strip()[:200]]
        cur = q.stdout.strip()
    return "clean", []


def live_equivalents(base, mb):
    """What base already carries from upstream since the merge-base: -x trailers and (author,
    subject) pairs (a hotfix tag's picks have other shas than master's commits)."""
    picked, subjects = set(), {}
    for h, meta in commit_meta("%s..%s" % (mb, base)).items():
        trailers = re.findall(r"\(cherry picked from commit ([0-9a-f]{40})\)", meta["body"])
        picked.update(trailers)
        if trailers:  # only Orca's own picks count by subject, never a coincidental same title
            subjects[(meta["author"], meta["subject"])] = h
    return picked, subjects


def commit_meta(spec):
    """{sha: {author, date, subject, body}} for the non-merge commits of a range, in one git call."""
    out = {}
    for rec in git("log", "--no-merges", "--format=%H%x00%an%x00%cs%x00%B%x1e", spec).split("\x1e"):
        parts = rec.strip("\n").split("\x00", 3)
        if len(parts) == 4:
            body = parts[3]
            out[parts[0]] = {"author": parts[1], "date": parts[2], "subject": body.split("\n", 1)[0], "body": body}
    return out


def in_live(c, base, mb, picked, subjects, cherry_minus, meta=None):
    if c in picked or c in cherry_minus:
        return True
    if meta is None:
        an, s = git("log", "-1", "--format=%an%x09%s", c).rstrip("\n").split("\t", 1)
    else:
        an, s = meta["author"], meta["subject"]
    return (an, s) in subjects


def cherry_minus(base, head, mb):
    out = git("cherry", base, head, mb)
    return {l[2:].strip() for l in out.splitlines() if l.startswith("- ")}


def fetch_upstream():
    say("fetching upstream Dolphin (master, tags, release-prep branches)")
    p = subprocess.run(["git", "-C", REPO, "fetch", "-q", "--tags", "upstream"], stderr=subprocess.PIPE, text=True)
    if p.returncode != 0:
        say("warning: git fetch upstream failed (%s); using what is already fetched" % p.stderr.strip()[:200])


def dolphin_version(sha):
    out = git("describe", "--tags", "--abbrev=0", "--match", "2[0-9][0-9][0-9]*", sha, check=False).strip()
    return out


def release_refs(mb, base_version):
    """Upstream release tags and release-prep branches at or after the base's Dolphin release."""
    try:
        floor = int(base_version[:4])
    except ValueError:
        floor = 0
    refs = []
    for t in git("tag", "--list").split():
        if re.fullmatch(r"\d{4}[a-z]?(-\d+)?", t) and int(t[:4]) >= floor:
            refs.append("refs/tags/" + t)
    for b in git("for-each-ref", "--format=%(refname:short)", "refs/remotes/upstream/").split():
        m = re.fullmatch(r"upstream/release-prep-(\d{4})[a-z]?(-\d+)?", b)
        if m and int(m.group(1)) >= floor:
            refs.append(b)
    return refs


def load_layer_rules(base):
    try:
        return layerlib.load_rules(repo=REPO, rev=base)
    except Exception:  # noqa: BLE001 (an old release without the map: use this checkout's)
        return layerlib.load_rules()


def max_layer(rules, files):
    best = None
    for f in files:
        try:
            cls = rules.classify(f).cls
        except layerlib.RulesError:
            cls = "engine"
        if best is None or LAYER_ORDER.index(cls) > LAYER_ORDER.index(best):
            best = cls
    return best or "-"


def cmd_triage(args):
    state = load_state()
    if not args.no_fetch:
        fetch_upstream()
    base, version = resolve_base(args.base)
    head = rev(args.head or UPSTREAM_REF)
    if not head:
        die("no %s: git remote add upstream https://github.com/dolphin-emu/dolphin.git" % (args.head or UPSTREAM_REF))
    mb = git("merge-base", base, head).strip()
    base_version = dolphin_version(mb)
    picked, subjects = live_equivalents(base, mb)
    minus = cherry_minus(base, head, mb)
    orca_changed = set(p for p in git("diff", "--name-only", "--no-renames", mb, base).splitlines() if p)
    rules = load_layer_rules(base)
    prev = state.get("triage", {})
    seen_before = set(prev.get("seen", []))

    commits = git("rev-list", "--reverse", "--no-merges", "%s..%s" % (mb, head)).split()
    metas = commit_meta("%s..%s" % (mb, head))
    pr_of = {}  # each commit's PR: the first merge on its way to head
    for line in git("log", "--merges", "--reverse", "--format=%H %s", "%s..%s" % (mb, head)).splitlines():
        h, subj = line.split(" ", 1)
        m = re.match(r"Merge pull request #(\d+)", subj)
        if m:
            for c in git("rev-list", "--no-merges", "%s^1..%s" % (h, h)).split():
                pr_of.setdefault(c, int(m.group(1)))
    pending, already = [], []
    for c in commits:
        meta = metas[c]
        if in_live(c, base, mb, picked, subjects, minus, meta):
            already.append(c)
            continue
        msg, subject = meta["body"], meta["subject"]
        files = commit_files(c)
        classes = classify(msg, files)
        where, why = reach(files)
        status, conflicts = pick_test(base, c)
        pending.append({
            "sha": c, "subject": subject, "date": meta["date"], "author": meta["author"], "pr": pr_of.get(c),
            "classes": classes, "reach": where, "reach_why": why, "files": len(files),
            "overlap": sorted(set(files) & orca_changed), "layer": max_layer(rules, files),
            "pick": status, "conflicts": conflicts, "new": c not in seen_before,
        })

    # A PR's commits often only apply together (a refactor series before the fix): test each PR's
    # pending commits as one series too.
    by_pr = {}
    for p in pending:
        if p["pr"]:
            by_pr.setdefault(p["pr"], []).append(p)
    for pr, ps in by_pr.items():
        if len(ps) > 1:
            status, conflicts = series_test(base, [p["sha"] for p in ps])
            for p in ps:
                p["series"] = {"pr": pr, "commits": len(ps), "pick": status, "conflicts": conflicts}

    releases, seen_refs = [], set()
    for ref in release_refs(mb, base_version):
        rc = git("rev-list", "--no-merges", "%s..%s" % (mb, ref)).split()
        if not rc or rev(ref) in seen_refs or git_ok("merge-base", "--is-ancestor", ref, base):
            continue
        seen_refs.add(rev(ref))
        rminus = cherry_minus(base, ref, mb)
        missing = [c for c in rc if not in_live(c, base, mb, picked, subjects, rminus)]
        releases.append({"ref": ref, "sha": rev(ref), "commits": len(rc), "missing": missing,
                         "missing_subjects": [git("log", "-1", "--format=%s", c).strip() for c in missing],
                         "merge": "clean" if git_ok("merge-tree", "--write-tree", base, ref) else "conflict"})

    report = {"at": now(), "live": {"version": version, "commit": base}, "merge_base": mb,
              "dolphin": base_version, "upstream_head": head, "in_live": len(already),
              "pending": pending, "releases": releases}
    print_triage(report)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=1)
        say("wrote %s" % args.json)
    if args.dashboard:
        dashboard(report, state, save=args.save)
    state["triage"] = {k: report[k] for k in ("at", "live", "merge_base", "dolphin", "upstream_head", "in_live")}
    state["triage"]["pending"] = [{k: p[k] for k in ("sha", "subject", "classes", "reach", "pick", "layer")}
                                  for p in pending if set(p["classes"]) & {"security", "crash"} and p["reach"] != "not-shipped"]
    state["triage"]["seen"] = sorted(seen_before | {p["sha"] for p in pending})
    save_state(state, "triage", "filed")
    hot = [p for p in pending if set(p["classes"]) & {"security", "crash"} and p["reach"] != "not-shipped"]
    return 3 if any(p["new"] for p in hot) else 0


def print_triage(r):
    live = r["live"]
    print("\nLive Orca %s (%s) is Dolphin %s + Orca; merge-base %s; upstream head %s." % (
        live["version"] or "?", live["commit"][:10], r["dolphin"] or "?", r["merge_base"][:10], r["upstream_head"][:10]))
    print("%d upstream commits since the merge-base are already in Orca (picked); %d are not.\n" %
          (r["in_live"], len(r["pending"])))
    for rel in r["releases"]:
        n = len(rel["missing"])
        print("Upstream release %s (%s): %d commits, %s; a merge onto live is %s." % (
            rel["ref"].replace("refs/tags/", "tag "), rel["sha"][:10], rel["commits"],
            "all in Orca" if not n else "%d not in Orca" % n, rel["merge"]))
        for subj in rel["missing_subjects"]:
            print("    not in Orca: %s" % subj[:100])
    if r["releases"]:
        print()
    order = {"security": 0, "crash": 1, "platform": 2, "perf": 3, "other": 4}
    rows = sorted(r["pending"], key=lambda p: (p["reach"] == "not-shipped", order[p["classes"][0]], p["date"]))
    print("new  sha         date        class            reach         layer             pick      overlap  subject")
    for p in rows:
        print("%-4s %-11s %-11s %-16s %-13s %-17s %-9s %-8s %s%s" % (
            "*" if p["new"] else "", p["sha"][:10], p["date"], ",".join(p["classes"])[:16], p["reach"],
            p["layer"], p["pick"], len(p["overlap"]) or "-", p["subject"][:80],
            " (#%d)" % p["pr"] if p["pr"] else ""))
        if p["reach_why"]:
            print("%s  not reached in a session: %s" % (" " * 16, "; ".join(p["reach_why"])))
        if p["overlap"]:
            print("%s  touches Orca's changes in: %s" % (" " * 16, ", ".join(p["overlap"][:6])))
        if p["conflicts"]:
            print("%s  conflicts with live in: %s" % (" " * 16, ", ".join(p["conflicts"][:6])))
        if p.get("series") and p["pick"] != "clean":
            print("%s  as PR #%d's %d-commit series: %s" % (" " * 16, p["series"]["pr"], p["series"]["commits"],
                                                         p["series"]["pick"]))
    hot = [p for p in r["pending"] if set(p["classes"]) & {"security", "crash"} and p["reach"] != "not-shipped"]
    print("\n%d security/crash commit(s) that may reach Orca (%d new since the last triage)." %
          (len(hot), sum(1 for p in hot if p["new"])))
    if hot:
        take = []
        for p in hot:
            series = p.get("series")
            if p["pick"] != "clean" and series and series["pick"] == "clean":
                group = [q["sha"] for q in r["pending"] if (q.get("series") or {}).get("pr") == series["pr"]]
            else:
                group = [p["sha"]]
            take += [c for c in group if c not in take]
        print("To take them:  upstream-sync.py start %s" % " ".join(c[:10] for c in take))


def dashboard(r, state, save):
    """One feedback item per upstream security/crash commit that may reach Orca (status watching):
    writes the triage.mjs JSON, and with --save files it as the lead."""
    filed = set(state.get("filed", []))
    items = []
    for p in r["pending"]:
        if not set(p["classes"]) & {"security", "crash"} or p["reach"] == "not-shipped" or p["sha"] in filed:
            continue
        sev = "medium" if "security" in p["classes"] and p["reach"] == "ships" else "low"
        reach_txt = {"ships": "ships in Orca builds",
                     "unreachable?": "is built into Orca but likely not reached in a session (%s)" % "; ".join(p["reach_why"])}[p["reach"]]
        items.append({
            "key": "upstream-dolphin-%s" % p["sha"][:10],
            "title": ("Upstream Dolphin fix not in Orca yet: %s" % p["subject"])[:160],
            "kind": "bug", "area": "orca", "severity": sev, "status": "watching",
            "summary": ("Upstream Dolphin commit %s (%s, %s) is not in live Orca %s. Classes: %s. The change %s. "
                        "It touches %d file(s) Orca changed; layer %s; a cherry-pick onto the live release is %s."
                        % (p["sha"][:10], p["date"], p["author"], r["live"]["version"], ", ".join(p["classes"]),
                           reach_txt, len(p["overlap"]), p["layer"],
                           p["pick"] + (" (%s)" % ", ".join(p["conflicts"][:4]) if p["conflicts"] else ""))),
            "recommendation": ("Read the change against Orca's exposure (who supplies the bytes). If it reaches "
                               "players: Tools/orca/upstream-sync.py start %s, then check, gate and next "
                               "(ORCA_NOTES.md, Upstream Dolphin updates)." % p["sha"][:10]),
            "sources": [{"kind": "other", "url": "%s/commit/%s" % (UPSTREAM_WEB, p["sha"]),
                         "quote": p["subject"][:300]}],
        })
    if not items:
        say("dashboard: nothing new to file")
        return
    d = os.path.join(WATCH, "dashboard")
    os.makedirs(d, exist_ok=True)
    path = os.path.join(d, "%s-%s.json" % (datetime.date.today().isoformat(), r["upstream_head"][:10]))
    with open(path, "w", encoding="utf-8") as f:
        json.dump({"author": "lead", "items": items}, f, indent=1)
    tool = os.environ.get("ORCA_TRIAGE_TOOL") or os.path.join(YOUGAME, "web", "scripts", "triage.mjs")
    say("dashboard: %d item(s) in %s" % (len(items), path))
    if not save:
        print("File them:  node %s save %s" % (tool, path))
        return
    if not os.path.exists(tool):
        die("no triage tool at %s (set ORCA_TRIAGE_TOOL)" % tool)
    p = subprocess.run(["node", tool, "save", path])
    if p.returncode != 0:
        die("triage.mjs save failed (exit %d)" % p.returncode, 1)
    keys = {i["key"] for i in items}
    state["filed"] = sorted(filed | {p["sha"] for p in r["pending"] if "upstream-dolphin-%s" % p["sha"][:10] in keys})


# --- start -------------------------------------------------------------------------------------


def wt_git(*args, check=True):
    return git(*args, repo=SYNC_WT, check=check)


def wt_busy():
    gd = wt_git("rev-parse", "--git-dir").strip()
    gd = gd if os.path.isabs(gd) else os.path.join(SYNC_WT, gd)
    for f in ("CHERRY_PICK_HEAD", "MERGE_HEAD", "REVERT_HEAD", "rebase-merge", "rebase-apply"):
        if os.path.exists(os.path.join(gd, f)):
            return f
    return None


def submodules():
    say("submodules (git submodule update --init --recursive)")
    log = os.path.join(BD, "upstream-sync-submodules.log")
    with open(log, "w") as f:
        p = subprocess.run(["git", "-C", SYNC_WT, "-c", "protocol.file.allow=always", "submodule", "update",
                            "--init", "--recursive"], stdout=f, stderr=subprocess.STDOUT)
    if p.returncode != 0:
        die("submodule update failed: %s" % log, 1)


def expand_picks(specs, base, mb):
    picked, subjects = live_equivalents(base, mb)
    out = []
    for s in specs:
        if ".." in s:
            cs = git("rev-list", "--reverse", "--no-merges", s).split()
        else:
            c = rev(s)
            if not c:
                die("no such commit: %s (git fetch upstream?)" % s)
            cs = [c]
        for c in cs:
            if git_ok("merge-base", "--is-ancestor", c, base) or in_live(c, base, mb, picked, subjects, set()):
                say("skip %s: already in the base (%s)" % (c[:10], git("log", "-1", "--format=%s", c).strip()[:70]))
            elif c not in out:
                out.append(c)
    return out


def apply_picks(state):
    sync = state["sync"]
    while sync["remaining"]:
        c = sync["remaining"][0]
        say("cherry-pick -x %s %s" % (c[:10], git("log", "-1", "--format=%s", c).strip()[:70]))
        p = subprocess.run(["git", "-C", SYNC_WT, "cherry-pick", "-x", c], stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True)
        if p.returncode != 0:
            conflicted = wt_git("diff", "--name-only", "--diff-filter=U").split()
            save_state(state, "sync")
            print(p.stdout[-2000:])
            die("the pick of %s stopped (conflicts: %s). Resolve in %s, `git cherry-pick --continue` "
                "(or `--skip` if it is already there), then: upstream-sync.py start --continue"
                % (c[:10], ", ".join(conflicted) or "?", SYNC_WT), 1)
        sync["applied"].append(c)
        sync["remaining"].pop(0)
        save_state(state, "sync")


def cmd_start(args):
    state = load_state()
    if args.cont:
        sync = state.get("sync")
        if not sync or not os.path.isdir(SYNC_WT):
            die("no sync in progress")
        busy = wt_busy()
        if busy:
            die("%s is still in progress in %s: finish it first (git cherry-pick --continue / git commit)" % (busy, SYNC_WT))
        if sync["remaining"] and sync.get("mode") == "pick":
            # The stopped pick was resolved by hand: continued (HEAD carries its -x trailer) or skipped.
            c = sync["remaining"].pop(0)
            if c in wt_git("log", "-1", "--format=%B"):
                sync["applied"].append(c)
            else:
                sync.setdefault("skipped", []).append(c)
                say("%s was skipped by hand" % c[:10])
        apply_picks(state)
        submodules()
        finish_start(state)
        return 0
    if not args.commits and not args.range and not args.merge:
        die("name commits, --range A..B or --merge REF")
    if not args.no_fetch:
        fetch_upstream()
    base, version = resolve_base(args.base)
    mb = git("merge-base", base, UPSTREAM_REF).strip()
    name = args.name or "upstream-sync-%s" % datetime.date.today().strftime("%Y%m%d")
    n = 2
    while rev("refs/heads/" + name) and not args.name:
        name = "upstream-sync-%s-%d" % (datetime.date.today().strftime("%Y%m%d"), n)
        n += 1
    if rev("refs/heads/" + name):
        die("branch %s exists" % name)
    if not os.path.isdir(SYNC_WT):
        say("creating the sync worktree %s" % SYNC_WT)
        git("worktree", "add", "-q", "--detach", SYNC_WT, base)
    else:
        if wt_busy():
            die("%s has a %s in progress" % (SYNC_WT, wt_busy()))
        dirty = wt_git("status", "--porcelain", "--untracked-files=no", "--ignore-submodules=all").strip()
        if dirty:
            die("%s has uncommitted changes:\n%s" % (SYNC_WT, dirty))
    wt_git("switch", "-q", "-c", name, base)
    say("branch %s at %s (live %s)" % (name, base[:10], version or "?"))
    state["sync"] = {"branch": name, "base": base, "base_version": version, "worktree": SYNC_WT,
                     "started": now(), "applied": [], "remaining": [], "mode": "pick"}
    if args.merge:
        ref = rev(args.merge) or die("no such ref: %s" % args.merge)
        state["sync"].update(mode="merge", merge=args.merge, merge_sha=ref)
        save_state(state, "sync")
        say("merge %s (%s)" % (args.merge, ref[:10]))
        p = subprocess.run(["git", "-C", SYNC_WT, "merge", "--no-ff", "-m",
                            "Merge upstream Dolphin %s" % args.merge.replace("refs/tags/", ""), ref],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if p.returncode != 0:
            print(p.stdout[-2000:])
            die("the merge stopped (conflicts: %s). Resolve in %s, `git commit`, then: upstream-sync.py start --continue"
                % (", ".join(wt_git("diff", "--name-only", "--diff-filter=U").split()) or "?", SYNC_WT), 1)
    else:
        specs = list(args.commits) + ([args.range] if args.range else [])
        state["sync"]["remaining"] = expand_picks(specs, base, mb)
        if not state["sync"]["remaining"]:
            save_state(state, "sync")
            die("nothing to pick: every named commit is already in the base", 1)
        save_state(state, "sync")
        apply_picks(state)
    submodules()
    finish_start(state)
    return 0


def finish_start(state):
    sync = state["sync"]
    sync["head"] = wt_git("rev-parse", "HEAD").strip()
    sync["picked"] = now()
    save_state(state, "sync")
    print()
    print(wt_git("log", "--oneline", "%s..HEAD" % sync["base"]))
    lod = subprocess.run([sys.executable, os.path.join(HERE, "layer-of-diff"), "--repo", SYNC_WT,
                          "%s...HEAD" % sync["base"]], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    print(lod.stdout.strip())
    print("\nNext: upstream-sync.py check   (build, unit tests, package, hashlogs and synctests vs live)")


# --- build -------------------------------------------------------------------------------------


def free_gb(path):
    st = os.statvfs(path)
    return st.f_bavail * st.f_frsize / 1e9


def load1():
    return float(subprocess.run(["sysctl", "-n", "vm.loadavg"], stdout=subprocess.PIPE, text=True).stdout.split()[1])


def wait_load(limit, where):
    while True:
        l = load1()
        if l < limit:
            return
        say("load %.1f, waiting %s" % (l, where))
        time.sleep(30)


def perf_running():
    try:
        st = json.loads(subprocess.run([sys.executable, RUN_LOCK, "status", "--json"], stdout=subprocess.PIPE,
                                       stderr=subprocess.DEVNULL, text=True, timeout=60).stdout)
    except Exception:  # noqa: BLE001 (no queue answer: don't wait on it)
        return None
    return next((t["label"] for t in st.get("tickets", []) if t.get("cls") == "perf" and t.get("state") == "running"), None)


def wait_perf(minutes):
    """A build beside a timed run skews it: wait (up to `minutes`) while run-lock shows a perf run."""
    end = time.time() + minutes * 60
    while time.time() < end:
        label = perf_running()
        if not label:
            return
        say("a perf run is going (%s); the build waits up to %d min" % (label[:80], minutes))
        time.sleep(60)
    say("still a perf run after %d min: building anyway, at low priority" % minutes)


def under_build_lock(cmd, log, env):
    """Waits outside build.lock for the load, then runs cmd holding it (one Orca build at a time on
    the Mac), re-checking the load inside."""
    if not os.path.isdir(SYNC_WT):
        die("no sync worktree: upstream-sync.py start first")
    if free_gb(SYNC_WT) < MIN_FREE_GB:
        die("only %.1f GB free (the house floor is %d GB)" % (free_gb(SYNC_WT), MIN_FREE_GB))
    others = subprocess.run(["pgrep", "-fl", "ninja"], stdout=subprocess.PIPE, text=True).stdout.strip()
    if others:
        say("another build is running (build.lock serializes):\n" + others)
    wait_load(LOAD_OUTSIDE, "outside build.lock")
    inner = [sys.executable, os.path.abspath(__file__), "_locked", "--log", log, "--"] + cmd
    t0 = time.time()
    p = subprocess.run([FLOCK, BUILD_LOCK] + inner, env=env)
    say("%s: exit %d after %.1f min (log %s)" % (" ".join(cmd[3:6]), p.returncode, (time.time() - t0) / 60, log))
    return p.returncode


def cmd_locked(args):
    wait_load(LOAD_INSIDE, "inside build.lock")
    with open(args.log, "w") as f:
        f.write("%s %s\n" % (now(), " ".join(args.argv)))
        f.flush()
        p = subprocess.run(args.argv, cwd=SYNC_WT, stdout=f, stderr=subprocess.STDOUT)
        f.write("exit %d %s\n" % (p.returncode, now()))
    return p.returncode


def build_env():
    env = dict(os.environ, CCACHE_DIR=CCACHE_DIR, CCACHE_BASEDIR=SYNC_WT)
    return env


def cmd_build(args):
    log = os.path.join(BD, "build-upstream-sync.log")
    flags = open(CMAKE_FLAGS).read().split()
    steps = []
    if not os.path.exists(os.path.join(SYNC_WT, "build", "build.ninja")):
        steps.append((["nice", "-n", "10", "cmake", "-S", ".", "-B", "build", "-G", "Ninja"] + flags +
                      ["-DENABLE_TESTS=ON"], os.path.join(BD, "cmake-upstream-sync.log")))
    steps.append((["nice", "-n", "10", "ninja", "-C", "build", "-j6"] + (args.targets or ["dolphin-emu-nogui", "tests"]), log))
    wait_perf(getattr(args, "perf_wait", 15))
    for s, slog in steps:
        rc = under_build_lock(s, slog, build_env())
        log = slog
        if rc != 0:
            die("build failed: tail %s" % log, 1)
    warn = sorted(set(l.strip() for l in open(log, errors="replace") if " warning: " in l))
    st = load_state().get("sync") or {}
    changed = set(p for p in wt_git("diff", "--name-only", "%s..HEAD" % st["base"]).splitlines() if p) if st.get("base") else set()
    mine = [w for w in warn if any(w.startswith("../" + c + ":") for c in changed)]
    say("build ok; %d distinct warning line(s) (ninja only prints a file's warnings when it recompiles it), "
        "%d in files the sync changed%s" % (len(warn), len(mine), "" if not mine else ":\n  " + "\n  ".join(mine[:20])))
    return 0


def sync_state():
    st = load_state()
    if not st.get("sync"):
        die("no sync recorded: upstream-sync.py start first")
    head = wt_git("rev-parse", "HEAD").strip()
    if head != st["sync"].get("head"):
        say("note: the sync worktree is at %s, not the recorded %s (commits added by hand?)" %
            (head[:10], (st["sync"].get("head") or "?")[:10]))
    st["sync"]["head"] = head
    return st


def run_dir(st):
    d = os.path.join(WATCH, "runs", st["sync"]["branch"])
    os.makedirs(d, exist_ok=True)
    return d


def cmd_package(args):
    st = sync_state()
    v = args.version or bump(st["sync"].get("base_version") or live_manifest()["version"])
    log = os.path.join(BD, "build-upstream-sync-mac.log")
    out = os.path.join(SYNC_WT, "build-orca-release")
    wait_perf(getattr(args, "perf_wait", 15))
    env = dict(build_env(), ORCA_VERSION=v, CMAKE_BUILD_PARALLEL_LEVEL="10")
    for k in ("CSC_LINK", "CSC_KEY_PASSWORD", "APPLE_API_KEY", "APPLE_API_KEY_ID", "APPLE_API_ISSUER",
              "ORCA_SOURCE_URL", "ORCA_INTERNAL"):
        env.pop(k, None)  # unsigned here: signing is orca-release.sh's job (next, step 2)
    rc = under_build_lock(["nice", "-n", "5", "bash", os.path.join(SYNC_WT, "Tools", "orca-package-macos.sh"),
                           out, os.path.join(out, "package")], log, env)
    if rc != 0:
        die("package failed: tail %s" % log, 1)
    z = os.path.join(out, "package", "Orca-%s-macos-arm64-unsigned.zip" % v)
    if not os.path.exists(z):
        die("no %s after the package step" % z, 1)
    d = os.path.join(run_dir(st), "unsigned")
    if os.path.isdir(d):
        shutil.rmtree(d)
    os.makedirs(d)
    subprocess.run(["ditto", "-x", "-k", z, d], check=True)
    st["sync"]["package"] = {"version": v, "zip": z, "app": os.path.join(d, "Orca.app"), "head": st["sync"]["head"]}
    save_state(st, "sync")
    say("unsigned Orca %s at %s" % (v, os.path.join(d, "Orca.app")))
    return 0


# --- test --------------------------------------------------------------------------------------

Q = {"ORCA_UX_TEST_NAMES": "ada,bo", "YG_RESULTS": "1"}
JOBS = {
    # Synctests over the queue's whole casual flows and ranked game 1 (Orca 0.3.14's env).
    "bf-casual": ("brawl", 4700, "bf-queue2-flow.txt", dict(Q, YG_SYNCTEST="7", YG_SYNCTEST_FROM="1440",
                                                             ORCA_TEST_QUEUE="casual:solo", ORCA_TEST_QUEUE_PICK="2057:3:2:-7.5:14.23")),
    "pp-casual": ("pplus", 3200, "pplus-queue2-flow.txt", dict(Q, YG_SYNCTEST="7", YG_SYNCTEST_FROM="200",
                                                                ORCA_TEST_QUEUE="casual:solo", ORCA_TEST_QUEUE_PICK="824:43:2:-7.4:7.7")),
    "bf-ranked": ("brawl", 2900, "bf-queue2-ranked.txt", dict(Q, YG_SYNCTEST="7", YG_SYNCTEST_FROM="1440",
                                                               ORCA_TEST_QUEUE="ranked:solo", ORCA_TEST_QUEUE_PICK="2106:3:2:-7.5:14.23:1532")),
    "pp-ranked": ("pplus", 4300, "pplus-queue2-ranked.txt", dict(Q, YG_SYNCTEST="7", YG_SYNCTEST_FROM="200",
                                                                  ORCA_TEST_QUEUE="ranked:solo", ORCA_TEST_QUEUE_PICK="874:43:2:-7.4:7.7:1532")),
    # Hashlogs of the default inputs (release checklist step 3).
    "bf-hash": ("brawl", 10800, "bf-mario-link-results.txt", {}),
    "pp-hash": ("pplus", 9000, "pplus-1v1.txt", {}),
}
DONE = re.compile(r"Harness done at frame (\d+): (\d+) re-run frames checked, (\d+) RAM mismatches")


def run_job(spec):
    """One headless Orca run (called inside run-lock.py check by `_jobs`)."""
    name, binary, out = spec["name"], spec["bin"], spec["out"]
    game, frames, inp, extra = JOBS[name]
    user = tempfile.mkdtemp(prefix="u-", dir=out)
    env = dict(HOME=HOME, PATH="/usr/bin:/bin", ORCA_SESSION="1", YG_SCENES="1", YG_EXIT_AFTER=str(frames),
               YG_INPUT=os.path.join(spec["inputs"], inp), **NO_HIDAPI, **extra)
    g = ["-e", DISC]
    if game == "pplus":
        env["ORCA_PROFILE"] = "PPLUS32"
        g = ["-C", "Dolphin.Core.DefaultISO=" + DISC, "-e", PPLUS]
    if name.endswith("-hash"):
        env["YG_HASHLOG"] = os.path.join(out, name + ".hash")
    cmd = ["nice", "-n", "10", binary, "-p", "headless", "-u", user, "-v", "Null",
           "-C", "Dolphin.DSP.Backend=No Audio Output", "-C", "Logger.Logs.ROLLBACK=True",
           "-C", "Logger.Options.Verbosity=2", "-C", "Logger.Options.WriteToConsole=True"] + g
    log = os.path.join(out, name + ".log")
    t0 = time.time()
    with open(log, "w") as f:
        rc = subprocess.run(cmd, env=env, stdout=f, stderr=subprocess.STDOUT).returncode
    shutil.rmtree(user, ignore_errors=True)
    m = None
    for m in DONE.finditer(open(log, errors="replace").read()):
        pass
    res = {"name": name, "exit": rc, "seconds": round(time.time() - t0)}
    if m:
        res.update(frame=int(m.group(1)), rerun=int(m.group(2)), ram_mismatches=int(m.group(3)))
    return res


def cmd_jobs(args):
    with open(args.spec) as f:
        specs = json.load(f)
    with ThreadPoolExecutor(max_workers=args.parallel) as ex:
        results = list(ex.map(run_job, specs))
    with open(args.spec + ".out", "w") as f:
        json.dump(results, f, indent=1)
    for s, r in zip(specs, results):
        print("%s %s: exit %s, frame %s, %s re-run, %s RAM mismatches (%d s)" % (
            s["bin"], r["name"], r["exit"], r.get("frame"), r.get("rerun"), r.get("ram_mismatches"), r["seconds"]))
    return 0


def run_lock(label, cmd, cwd=None, log=None):
    full = [sys.executable, RUN_LOCK, "check", "--label", "AGENT TEST " + label, "--"] + cmd
    env = dict(os.environ, **NO_HIDAPI)
    if log:
        with open(log, "w") as f:
            return subprocess.run(full, cwd=cwd, env=env, stdout=f, stderr=subprocess.STDOUT).returncode
    return subprocess.run(full, cwd=cwd, env=env).returncode


def hashcmp(a, b, upto=None):
    def load(p):
        d = {}
        for line in open(p, errors="replace"):
            parts = line.split()
            if len(parts) >= 2 and parts[0].isdigit():
                d.setdefault(int(parts[0]), []).append(" ".join(parts[1:]))
        return d
    A, B = load(a), load(b)
    shared = sorted(k for k in A if k in B and (upto is None or k <= upto))
    diff = [k for k in shared if A[k] != B[k]]
    return {"shared": len(shared), "last": shared[-1] if shared else None, "differ": len(diff),
            "first_differ": diff[0] if diff else None}


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def test_unit(st, out):
    tests = os.path.join(SYNC_WT, "build", "Binaries", "Tests", "tests")
    if not os.path.exists(tests):
        die("no %s: upstream-sync.py build first" % tests)
    port = free_port()
    fk = subprocess.Popen([sys.executable, os.path.join(SYNC_WT, "Tools", "orca", "fake_keyframes.py"), str(port)],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    log = os.path.join(out, "unit.log")
    try:
        time.sleep(1)
        cmd = ["env", "ORCA_TEST_KEYFRAME_SERVER=http://127.0.0.1:%d" % port, "ORCA_TEST_DISC=" + DISC,
               "ORCA_TEST_DISC_REV1=" + DISC_REV1, "nice", "-n", "10", tests, "--gtest_filter=Orca*"]
        say("Orca* gtests (run-lock check; log %s)" % log)
        rc = run_lock("upstream-sync %s Orca* gtests" % st["sync"]["branch"], cmd, cwd=os.path.join(SYNC_WT, "build"), log=log)
    finally:
        fk.terminate()
    text = open(log, errors="replace").read()

    def num(rx):
        m = re.search(rx, text)
        return int(m.group(1)) if m else 0
    res = {"exit": rc, "ran": num(r"\[==========\] (\d+) tests? from \d+ test suites? ran"),
           "passed": num(r"\[  PASSED  \] (\d+) test"), "skipped": num(r"\[  SKIPPED \] (\d+) test"),
           "failed": num(r"\[  FAILED  \] (\d+) tests?, listed below")}
    res["pass"] = rc == 0 and res["failed"] == 0 and res["passed"] > 0
    say("Orca* gtests: %(ran)d run, %(passed)d passed, %(skipped)d skipped, %(failed)d failed (exit %(exit)d)" % res)
    return res


def candidate_bin(st, app):
    if app:
        b = os.path.join(app, "Contents", "MacOS", "Orca") if app.endswith(".app") else app
    elif (st["sync"].get("package") or {}).get("head") == st["sync"]["head"]:
        b = os.path.join(st["sync"]["package"]["app"], "Contents", "MacOS", "Orca")
    else:
        b = os.path.join(SYNC_WT, "build", "Binaries", "dolphin-emu-nogui")
        say("note: no package of %s yet; hashlogs/synctests use the dev build %s" % (st["sync"]["head"][:10], b))
    if not os.access(b, os.X_OK):
        die("no runnable %s" % b)
    return b


def test_runs(st, out, which, app, live_app):
    m = live_manifest()
    if live_app:
        lbin = os.path.join(live_app, "Contents", "MacOS", "Orca")
        lv = "given"
    else:
        lapp, m = ensure_live_app(m)
        lbin, lv = os.path.join(lapp, "Contents", "MacOS", "Orca"), m["version"]
    cbin = candidate_bin(st, app)
    names = [n for n in JOBS if (n.endswith("-hash") and "hash" in which) or (not n.endswith("-hash") and "sync" in which)]
    inputs = os.path.join(SYNC_WT, "Tools", "orca", "inputs")
    if not os.path.isdir(inputs):
        inputs = os.path.join(REPO, "Tools", "orca", "inputs")
    # The live results are cached per live version and per content of the inputs they ran.
    h = hashlib.sha256()
    for n in names:
        with open(os.path.join(inputs, JOBS[n][2]), "rb") as f:
            h.update(n.encode() + b"\0" + f.read())
    lout = (os.path.join(WATCH, "live", lv, "results-" + h.hexdigest()[:10]) if lv != "given"
            else os.path.join(out, "live"))
    cout = os.path.join(out, "cand")
    for d in (lout, cout):
        os.makedirs(d, exist_ok=True)
    specs = [{"name": n, "bin": cbin, "out": cout, "inputs": inputs, "role": "cand"} for n in names]
    live_cached = os.path.join(lout, "results.json")
    lres = {}
    if os.path.exists(live_cached):
        lres = {r["name"]: r for r in json.load(open(live_cached))}
    specs += [{"name": n, "bin": lbin, "out": lout, "inputs": inputs, "role": "live"} for n in names
              if n not in lres or (n.endswith("-hash") and not os.path.exists(os.path.join(lout, n + ".hash")))]
    spec_path = os.path.join(out, "jobs.json")
    with open(spec_path, "w") as f:
        json.dump(specs, f, indent=1)
    say("%d headless Orca run(s): %s (run-lock check)" % (len(specs), ", ".join(
        "%s/%s" % (s["role"], s["name"]) for s in specs)))
    rc = run_lock("upstream-sync %s hashlogs+synctests (headless)" % st["sync"]["branch"],
                  [sys.executable, os.path.abspath(__file__), "_jobs", spec_path], log=os.path.join(out, "jobs.log"))
    if not os.path.exists(spec_path + ".out"):
        die("the runs did not finish (run-lock exit %d; %s)" % (rc, os.path.join(out, "jobs.log")), 1)
    done = json.load(open(spec_path + ".out"))
    cres = {}
    for s, r in zip(specs, done):
        (cres if s["role"] == "cand" else lres)[r["name"]] = r
    with open(live_cached, "w") as f:
        json.dump([r for r in lres.values() if r.get("exit") == 0], f, indent=1)
    results = {}
    for n in names:
        c, l = cres.get(n, {}), lres.get(n, {})
        if n.endswith("-hash"):
            cmpd = hashcmp(os.path.join(lout, n + ".hash"), os.path.join(cout, n + ".hash"))
            frames = JOBS[n][1]  # both logs must cover the run, not just agree on a short stretch
            ok = (c.get("exit") == 0 and cmpd["differ"] == 0 and cmpd["last"] is not None
                  and cmpd["last"] >= frames - 30)
            results[n] = dict(cmpd, exit=c.get("exit"), pass_=ok)
            say("%s: %d shared frames (0..%s), %d differ%s -> %s" % (
                n, cmpd["shared"], cmpd["last"], cmpd["differ"],
                ", first %s" % cmpd["first_differ"] if cmpd["differ"] else "", "PASS" if ok else "FAIL"))
        else:
            ok = c.get("exit") == 0 and c.get("ram_mismatches") == 0 and (c.get("rerun") or 0) > 0
            same = c.get("rerun") == l.get("rerun")
            results[n] = dict(c, live_rerun=l.get("rerun"), same_as_live=same, pass_=ok)
            say("%s: %s re-run frames, %s RAM mismatches (live %s re-run%s) -> %s" % (
                n, c.get("rerun"), c.get("ram_mismatches"), l.get("rerun"), "" if same else ": DIFFERENT, explain it",
                "PASS" if ok else "FAIL"))
    return results


def cmd_test(args):
    st = sync_state()
    which = set(args.what or ["unit", "hash", "sync"])
    out = run_dir(st)
    res = {"head": st["sync"]["head"], "at": now()}
    if "unit" in which:
        res["unit"] = test_unit(st, out)
    if which & {"hash", "sync"}:
        res["runs"] = test_runs(st, out, which, args.app, args.live_app)
    ok = all(v.get("pass", v.get("pass_", True)) for k, v in res.items() if k == "unit") and \
        all(r["pass_"] for r in res.get("runs", {}).values())
    res["pass"] = ok
    st.setdefault("sync", {}).setdefault("tests", []).append(res)
    save_state(st, "sync")
    with open(os.path.join(out, "results-%s.json" % datetime.datetime.now().strftime("%H%M%S")), "w") as f:
        json.dump(res, f, indent=1)
    say("tests at %s: %s" % (st["sync"]["head"][:10], "PASS" if ok else "FAIL"))
    return 0 if ok else 1


def cmd_check(args):
    for step in (lambda: cmd_build(argparse.Namespace(targets=None, perf_wait=15)),
                 lambda: cmd_test(argparse.Namespace(what=["unit"], app=None, live_app=None)),
                 lambda: cmd_package(argparse.Namespace(version=args.version, perf_wait=15)),
                 lambda: cmd_test(argparse.Namespace(what=["hash", "sync"], app=None, live_app=None))):
        rc = step()
        if rc:
            return rc
    return cmd_next(args)


# --- perf gate ---------------------------------------------------------------------------------

GATE_ONE = r'''#!/bin/bash
# one.sh <run> <Orca.app> [queue-e2e args...]: one two-Orca perf-gate run (gate-0334fb method; written
# by upstream-sync.py gate). A Project+ ranked set (queue-e2e --q2 --queue ranked), both Orcas
# rendering with Metal, 40 +- 8 ms simulated latency, input delay 3, SDL HIDAPI off, over the LOCAL
# rooms stack (site @SITE@), Brawl disc from the internal disk. Call it under run-lock.py perf.
G=@G@
W=@W@
SITE=${GATE_SITE:-@SITE@}
run=$1; APP=$2; shift 2
BIN=$APP/Contents/MacOS/Orca
R2="@DISC@"
PP="@PPLUS@"
[ -x "$BIN" ] || { echo "no $BIN"; exit 2; }
curl -s -m 5 -o /dev/null -X POST "$SITE/api/multiplayer/ticket" || { echo "the local site $SITE is down"; exit 2; }
cat "$PP" "$(dirname "$PP")/sd.raw" > /dev/null || exit 2
mkdir -p $G/runs
start=$(date '+%Y-%m-%d %H:%M:%S')
env ORCA_TEST_NET_DELAY_MS=40 ORCA_TEST_NET_JITTER_MS=8 \
  SDL_JOYSTICK_HIDAPI=0 SDL_JOYSTICK_HIDAPI_GAMECUBE=0 SDL_HIDAPI_LIBUSB=0 \
  node $W/Tools/orca/queue-e2e.mjs "$BIN" "$R2" --pplus "$PP" --q2 --queue ranked --shots 0 \
  --shots-who ada,bo --keep --delay 3 --site "$SITE" "$@" > $G/runs/$run.log 2>&1
rc=$?
end=$(date '+%Y-%m-%d %H:%M:%S')
echo "$run exit $rc, $start .. $end, load $(sysctl -n vm.loadavg)" >> $G/runs/$run.log
dir=$(grep -o 'logs in .*' $G/runs/$run.log | tail -1 | cut -c9-)
if [ -n "$dir" ] && [ -d "$dir" ]; then
  mkdir -p $G/runs/$run && cp "$dir"/ada.log "$dir"/bo.log $G/runs/$run/ && rm -rf "$dir"
fi
echo "$run: exit $rc"
exit $rc
'''
GATE_RUNS = r'''#!/bin/bash
# runs.sh [run...]: the perf gate (written by upstream-sync.py gate). B<n> = LIVE Orca @LIVE_V@
# (the signed release from the site), C<n> = the candidate @CAND_V@. B0 and C0 are warm-ups (each
# binary's first run fills its Metal shader cache, and macOS checks each new program once), not
# counted; 4 interleaved pairs after. Needs AC power (pmset -g batt) and the local rooms stack.
G=@G@
pmset -g batt | grep -q "AC Power" || { echo "on battery: wait for AC before a perf gate"; exit 2; }
for run in ${@:-B0 C0 B1 C1 B2 C2 B3 C3 B4 C4}; do
  case $run in B*) APP="@LIVE_APP@" ;; C*) APP="@CAND_APP@" ;; esac
  python3 @RUN_LOCK@ perf --label "AGENT TEST @CAND_V@ perf gate $run (local rooms, delay 3)" -- \
    bash $G/one.sh $run "$APP" 2> $G/runs/$run.lock.log
  echo "$run exit $? $(date +%T) $(tail -1 $G/runs/$run.lock.log)"
done
python3 $G/fullset.py
'''
GATE_FULLSET = r'''#!/usr/bin/env python3
# fullset.py: per build, the joiner's whole-set totals ("Online match at leave") and the host's last
# periodic line, each with its session's span, plus per 10,000 session frames (written by
# upstream-sync.py gate; the method of gate-0336/fullset.py).
import os, re
G = "@G@/runs"
FINAL = re.compile(r"Online match at (leave|frame \d+|host left|ended [^:]*): frame (\d+), (\d+) rollbacks, (\d+) re-run frames, deepest (\d+), (\d+) stalls \((\d+) counted\), (\d+) waits, (\d+) hitches here(?: \((\d+) more in game loads\))?, (\d+) there, .*?(\d+) checksums matched.*?snapshots \(every \d+, ([\d.]+) ms each\)")
START = re.compile(r"Drop-in: port 2 is \w+ from frame (\d+)")
def side(path):
    txt = open(path, errors="replace").read()
    s = START.search(txt)
    ls = list(FINAL.finditer(txt))
    if not s or not ls:
        return None
    m = ls[-1]
    return dict(span=int(m[2]) - int(s[1]), rb=int(m[3]), rerun=int(m[4]), stalls=int(m[6]),
                slow=int(m[9]) + int(m[10] or 0), here=int(m[9]), cks=int(m[12]), snap=float(m[13]))
runs = sorted(r for r in os.listdir(G) if os.path.isdir(os.path.join(G, r)) and r[1:] != "0")
keys = ["slow", "here", "stalls", "rb", "rerun", "snap"]
for who in ("bo", "ada"):
    print("%s (%s):" % ("joiner" if who == "bo" else "host", who))
    for build in ("B", "C"):
        rows = [side(os.path.join(G, r, who + ".log")) for r in runs if r[0] == build]
        rows = [r for r in rows if r]
        if not rows:
            continue
        avg = {k: sum(r[k] for r in rows) / len(rows) for k in keys}
        span = sum(r["span"] for r in rows)
        per = {k: sum(r[k] for r in rows) * 10000 / span for k in ("slow", "stalls")} if span else {}
        print("  %s n=%d " % (build, len(rows)) + " ".join("%s %.2f" % (k, avg[k]) for k in keys) +
              "  per 10k: " + " ".join("%s %.1f" % (k, v) for k, v in per.items()))
'''


def cmd_gate(args):
    st = sync_state()
    m = live_manifest()
    lapp, m = ensure_live_app(m)
    v = args.version or (st["sync"].get("package") or {}).get("version") or bump(m["version"])
    cand = os.path.abspath(args.cand)
    if not os.access(os.path.join(cand, "Contents", "MacOS", "Orca"), os.X_OK):
        die("no runnable Orca in %s" % cand)
    g = os.path.join(BD, "gate-" + v.replace(".", ""))
    if os.path.exists(os.path.join(g, "runs.sh")) and not args.force:
        die("%s exists (--force rewrites its scripts; its runs/ stay)" % g)
    os.makedirs(os.path.join(g, "runs"), exist_ok=True)
    subs = {"@G@": g, "@W@": SYNC_WT, "@SITE@": args.site, "@DISC@": DISC, "@PPLUS@": PPLUS,
            "@LIVE_V@": m["version"], "@CAND_V@": v, "@LIVE_APP@": lapp, "@CAND_APP@": cand, "@RUN_LOCK@": RUN_LOCK}
    for name, text in (("one.sh", GATE_ONE), ("runs.sh", GATE_RUNS), ("fullset.py", GATE_FULLSET)):
        for k, val in subs.items():
            text = text.replace(k, val)
        path = os.path.join(g, name)
        with open(path, "w") as f:
            f.write(text)
        os.chmod(path, 0o755)
    up = subprocess.run(["curl", "-s", "-m", "5", "-o", "/dev/null", "-w", "%{http_code}", "-X", "POST",
                         args.site + "/api/multiplayer/ticket"], stdout=subprocess.PIPE, text=True).stdout
    print("Perf gate written to %s: B = live %s (%s), C = %s (%s)." % (g, m["version"], lapp, v, cand))
    print("Local rooms stack at %s: %s" % (args.site, "up" if up and up != "000" else
                                           "DOWN (start one: %s/local-rooms-stack/start.sh serves 8795/8796; pass --site)" % BD))
    print("Run (about 10 x 9 min, the Mac to itself, on AC; nothing heavy on the external SSD meanwhile):")
    print("  nohup bash %s/runs.sh > %s/runs.out 2>&1 &" % (g, g))
    print("Verdict: %s/fullset.py; no regression = the C means within the B runs' spread on slow frames,"
          " hitches here, stalls and rollbacks (as 0.3.36: gate-0336, release notes)." % g)
    return 0


# --- next --------------------------------------------------------------------------------------


def newest_kit():
    kits = sorted(d for d in os.listdir(WATCH) if d.startswith("kit-")) if os.path.isdir(WATCH) else []
    return os.environ.get("ORCA_RELEASE_KIT") or (os.path.join(WATCH, kits[-1]) if kits else "<no kit>")


def cmd_next(args):
    st = sync_state()
    s = st["sync"]
    head = s["head"]
    m = live_manifest()
    v = args.version or (s.get("package") or {}).get("version") or bump(m["version"])
    kit = newest_kit()
    rel = os.path.join(WATCH, "rel-" + v)
    out = os.path.join(BD, "orca-%s-release-out" % v.replace(".", ""))
    tests = [t for t in s.get("tests", []) if t["head"] == head]
    print("""
Upstream sync %(branch)s at %(head10)s on live %(live)s; candidate Orca %(v)s.
Tests at this commit: %(tests)s

The rest of the release (the 0.3.36 kit is %(kit)s; copy it to %(rel)s, swap versions and shas):

 1. Push the branch as the release branch (private repo; never force):
      git -C %(wt)s push origin HEAD:refs/heads/release-%(v)s
    ORCA.md "Base:" line: name the upstream commits/tag taken. kCompatVersion stays unless the
    hashlogs moved on purpose (they must equal live's otherwise).

 2. Mac release build, signed + notarized (YouGame's orca-release.sh from origin/main, no upload):
      python3 %(me)s package --version %(v)s
      K=%(rel)s/relscript; mkdir -p $K && git -C %(yg)s fetch -q origin &&
        git -C %(yg)s archive origin/main desktop/scripts | tar -x -C $K
      ORCA_ALLOW_PRIVATE_SOURCE=1 bash $K/desktop/scripts/orca-release.sh --app %(unsigned)s \\
        --version %(v)s --repo %(wt)s --commit %(head)s --out %(out)s --no-upload
      ditto -x -k %(out)s/orca-macos-arm64.zip %(rel)s/signed
      python3 %(me)s test hash sync --app %(rel)s/signed/Orca.app     # same results on the signed bits

 3. Perf gate, live vs candidate (about 95 min; the Mac to itself; on AC):
      python3 %(me)s gate --cand %(rel)s/signed/Orca.app --version %(v)s
      nohup bash %(gate)s/runs.sh > %(gate)s/runs.out 2>&1 &

 4. Windows (PC over SSH, %(kit)s/rel0336/win: pc.sh, pc/launch.ps1 runs a script as a one-shot
    task in Luis's desktop session; label every window AGENT TEST). Get the commit there (push it,
    or a git bundle into refs/mac/incoming), then from the kit's pc/*.ps1 with 0.3.36 -> %(v)s and
    the sha -> %(head)s:
      step1.ps1  remote-release.ps1 -Version %(v)s -Sha %(head)s -Branch release-%(v)s -DryRun -LowPriority
                 (build, Authenticode sign, stage F:\\src\\orca-work-<v>-dry\\orca-%(v)s-windows-x64.zip; 0 MSVC warnings)
      step2.ps1  check-staged-orca.ps1 -Version %(v)s -ExpectSha256 <staged sha> (shader check + gate 1)
      step3.ps1  Orca* gtests on F:\\src\\orca-rel at %(head10)s (fake keyframe server, Rev 2 disc)

 5. Mac <-> PC on prod (kit win/xf.sh, sandbox-gus on the Mac, sandbox-hal on the PC): a friends join
    each way (PC hosts, then Mac hosts), 0 desync, checksums matched.

 6. Go live (kit rel0336/pub: go.sh, flip.sh, verify.sh): set V=%(v)s, COMMIT=%(head)s, the Mac zip's
    sha/bytes from %(out)s, the Windows zip's from the PC; flip.sh's backup check expects live %(live)s.
      PC:  publish-staged-orca.ps1 -Version %(v)s -ExpectSha256 <staged sha>   (Windows zip to R2)
      Mac: bash %(rel)s/pub/go.sh all     (pre-checks, backup, flip, served-manifest check, verify)
    Rollback: (cd %(rel)s/pub/web && npx wrangler r2 object put yougame-games/runtime/orca/manifest.json \\
      --file=../backup/manifest.before-%(v)s.json --content-type=application/json --remote)

 7. Prod smoke as /sandbox roster players with the released build (kit prod0336): tickets
    (an old build's queue search gets 409 orca_outdated), a Brawl and a P+ ranked set, aged-host
    friends drop-ins (long-join-prod.py); roll back if online play breaks.

 8. After: merge release-%(v)s into private main (no force; forbidden-calls baseline as 0.3.35/0.3.36
    did), then the public source tag: Tools/orca-publish-public.sh <shipped ref> "Orca %(v)s (source of
    %(head10)s)" orca-%(v)s (screened; neutral wording). Dashboard note (triage.mjs, as=lead) and a
    dated line in the lead's queue.
""" % {"branch": s["branch"], "head10": head[:10], "head": head, "live": m["version"], "v": v,
       "tests": ", ".join("%s %s" % (t["at"], "PASS" if t["pass"] else "FAIL") for t in tests) or "none yet (upstream-sync.py check)",
       "kit": kit, "rel": rel, "wt": SYNC_WT, "me": os.path.abspath(__file__), "yg": YOUGAME,
       "unsigned": (s.get("package") or {}).get("app", "<upstream-sync.py package first>"), "out": out,
       "gate": os.path.join(BD, "gate-" + v.replace(".", ""))})
    return 0


# --- status ------------------------------------------------------------------------------------


def cmd_status(args):
    st = load_state()
    m = live_manifest()
    print("Live Orca %s, commit %s (minDesktop %s)" % (m["version"], m["commit"][:10], m.get("minDesktop")))
    print("Upstream master %s (fetched); merge-base with live %s (Dolphin %s)" % (
        rev(UPSTREAM_REF)[:10], git("merge-base", m["commit"], UPSTREAM_REF, check=False).strip()[:10],
        dolphin_version(m["commit"])))
    t = st.get("triage")
    if t:
        print("Last triage %s: upstream %s, %d security/crash commit(s) pending:" % (
            t["at"], t["upstream_head"][:10], len(t.get("pending", []))))
        for p in t.get("pending", []):
            print("  %s %-16s %-13s %-8s %s" % (p["sha"][:10], ",".join(p["classes"]), p["reach"], p["pick"], p["subject"][:70]))
    s = st.get("sync")
    if s:
        here = wt_git("rev-parse", "--abbrev-ref", "HEAD", check=False).strip() if os.path.isdir(SYNC_WT) else "-"
        print("Sync %s from %s (live %s): %d picked, %d left; worktree %s on %s%s" % (
            s["branch"], s["base"][:10], s.get("base_version"), len(s.get("applied", [])), len(s.get("remaining", [])),
            SYNC_WT, here, " (%s in progress)" % wt_busy() if os.path.isdir(SYNC_WT) and wt_busy() else ""))
        for tr in s.get("tests", [])[-3:]:
            print("  tests %s at %s: %s" % (tr["at"], tr["head"][:10], "PASS" if tr["pass"] else "FAIL"))
    return 0


def main(argv):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="command", required=True)
    sub.add_parser("status")
    t = sub.add_parser("triage")
    t.add_argument("--base", help="the release to compare (default: the live manifest's commit)")
    t.add_argument("--head", help="upstream ref (default %s)" % UPSTREAM_REF)
    t.add_argument("--json")
    t.add_argument("--dashboard", action="store_true", help="write triage.mjs items for security/crash commits")
    t.add_argument("--save", action="store_true", help="with --dashboard: file them (as lead)")
    t.add_argument("--no-fetch", action="store_true")
    s = sub.add_parser("start")
    s.add_argument("commits", nargs="*")
    s.add_argument("--range")
    s.add_argument("--merge", help="merge an upstream ref (a hotfix tag) instead of picking")
    s.add_argument("--base", help="the release to branch from (default: the live manifest's commit)")
    s.add_argument("--name")
    s.add_argument("--continue", dest="cont", action="store_true")
    s.add_argument("--no-fetch", action="store_true")
    b = sub.add_parser("build")
    b.add_argument("targets", nargs="*")
    b.add_argument("--perf-wait", type=int, default=15, help="minutes to wait while a perf run is going (0: don't)")
    k = sub.add_parser("package")
    k.add_argument("--version")
    k.add_argument("--perf-wait", type=int, default=15)
    te = sub.add_parser("test")
    te.add_argument("what", nargs="*", choices=["unit", "hash", "sync"])
    te.add_argument("--app", help="candidate Orca.app (default: the package of this commit, else the dev build)")
    te.add_argument("--live-app", help="the live Orca.app (default: downloaded from the site and checked)")
    c = sub.add_parser("check")
    c.add_argument("--version")
    g = sub.add_parser("gate")
    g.add_argument("--cand", required=True)
    g.add_argument("--version")
    g.add_argument("--site", default="http://127.0.0.1:8836")
    g.add_argument("--force", action="store_true")
    n = sub.add_parser("next")
    n.add_argument("--version")
    lk = sub.add_parser("_locked")
    lk.add_argument("--log", required=True)
    lk.add_argument("argv", nargs=argparse.REMAINDER)
    j = sub.add_parser("_jobs")
    j.add_argument("spec")
    j.add_argument("--parallel", type=int, default=int(os.environ.get("ORCA_SYNC_PARALLEL", "4")))
    a = p.parse_args(argv)
    if a.command == "_locked" and a.argv[:1] == ["--"]:
        a.argv = a.argv[1:]
    return {"status": cmd_status, "triage": cmd_triage, "start": cmd_start, "build": cmd_build,
            "package": cmd_package, "test": cmd_test, "check": cmd_check, "gate": cmd_gate, "next": cmd_next,
            "_locked": cmd_locked, "_jobs": cmd_jobs}[a.command](a)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
