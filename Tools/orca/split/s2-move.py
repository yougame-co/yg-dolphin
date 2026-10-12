#!/usr/bin/env python3
# Copyright 2026 YouGame
# SPDX-License-Identifier: GPL-2.0-or-later
"""S2 commit 1 of the Orca engine/pack split: move the game modules into Orca/Games/Brawl/.

  Tools/orca/split/s2-move.py               git mv the 18 game modules (35 files) and their 11
                                            tests, then rewrite every path that names them
  Tools/orca/split/s2-move.py --rewrite-only
                                            only rewrite paths (no git mv): for a branch that was
                                            rebased onto the move and added a new #include or CMake
                                            line naming an old path
  Tools/orca/split/s2-move.py --check       list code that still names an old path (exit 1 if any)

Run from anywhere inside the checkout. The script only moves files and edits path strings, so
`git diff -M HEAD` shows 100% renames apart from #include lines, CMake source lists, layers.txt and
the tests that pin classified paths. It never commits. It is idempotent: a second run changes
nothing.

Replaying on another branch (one that forked from the V2 line before the move):

  git rebase split-s2            # rename detection carries edits to moved files over
  Tools/orca/split/s2-move.py --rewrite-only
  Tools/orca/split/s2-move.py --check
  git commit -am "Follow the S2 move: include paths"   # only if the rewrite changed anything

A branch that adds a whole new game-module file under Orca/UX/ should `git mv` it into
Orca/Games/Brawl/ by hand and add it to MODULES below on that branch.

What is rewritten (tracked files only; docs (*.md), ORCA_NOTES and the forbidden-calls baseline are
left alone: the baseline is reviewed by a person, and notes are history):
  - `Orca/UX/<Module>.<ext>` -> `Orca/Games/Brawl/<Module>.<ext>` (RankedPPlus: Games/Brawl/PPlus/)
    in Source/** and Tools/orca/** (includes, CMake lists, layer tests, free-space.py);
  - `Orca/<Game>Test.cpp` -> `Orca/Games/Brawl/<Game>Test.cpp` (tests' CMake list, layer tests);
  - moved tests that find Data/Sys through `__FILE__` get two more `../`;
  - Tools/orca/layers.txt: the UX/ game-logic lines become one review line under Games/Brawl/ (the
    existing `Games/**` rule classes the rest), and the game tests' lines become `Games/**`.
"""

import argparse
import os
import re
import subprocess
import sys

UX = "Source/Core/Core/Orca/UX/"
GAMES = "Source/Core/Core/Orca/Games/Brawl/"
TESTS = "Source/UnitTests/Core/Orca/"
GAME_TESTS_DIR = TESTS + "Games/Brawl/"

# The 18 modules (layers.txt's game-logic lines under UX/, modules.md section 2) and where they go,
# relative to Orca/Games/Brawl/.
MODULES = {
    "BrawlStages": "", "CharOrder": "", "CssTitle": "", "NameTags": "", "OnlineMenu": "",
    "OnlineRules": "", "OnlineSeats": "", "RankedPPlus": "PPlus/", "StageCursors": "",
    "Queue": "", "RankedSet": "", "Results": "", "FreeSpace": "", "MatchBlock": "",
    "SetBlock": "", "MenuText": "", "NativeText": "", "Relabel": "",
}
# Their 11 tests (layers.txt's game-logic test lines).
GAME_TESTS = ["BrawlStages", "CharOrder", "CssTitle", "NativeText", "OnlineMenu", "OnlineRules",
              "OnlineSeats", "Queue", "RankedSet", "Relabel", "Results"]

SKIP = {"Tools/orca/forbidden-calls.baseline", "Tools/orca/split/s2-move.py"}
REWRITE_ROOTS = ("Source/", "Tools/orca/")
REWRITE_EXT = (".h", ".hpp", ".cpp", ".inc", ".ipp", ".mm", ".m", ".c", ".txt", ".cmake", ".py",
               ".mjs", ".sh", ".json", ".in")

MOD_RE = re.compile(r"Orca/UX/(" + "|".join(sorted(MODULES, key=len, reverse=True)) +
                    r")\.(\w+)")
TEST_RE = re.compile(r"(?<=Orca/)(" + "|".join(GAME_TESTS) + r")Test\.cpp")

LAYERS_OLD = """\
# Game logic: hardcoded Brawl rev 2 / Project+ memory.
game-logic        Source/Core/Core/Orca/UX/{BrawlStages,CharOrder,CssTitle,NameTags,OnlineMenu}.*
game-logic        Source/Core/Core/Orca/UX/{OnlineRules,OnlineSeats,RankedPPlus,StageCursors}.*
# ...carrying competitive policy until S2 splits it out: the queue's ready/timer/skip machine,
# ranked stocks and minutes (RankedSet), and the winner and set result the room rates (Results).
game-logic        Source/Core/Core/Orca/UX/{Queue,RankedSet,Results}.* review
# Game memory layouts: free space, code caves, the match block and the set block.
game-logic        Source/Core/Core/Orca/UX/{FreeSpace,MatchBlock,SetBlock}.*
# Mixed: an engine mechanism carrying game data (texture names, message buffers, menu strings).
# Classed by the game data, which is what changes; the mechanism moves to the engine in S5.
game-logic        Source/Core/Core/Orca/UX/{MenuText,NativeText,Relabel}.*
"""
LAYERS_NEW = """\
# Game logic (hardcoded Brawl rev 2 / Project+ memory) moved to Orca/Games/Brawl/ in S2 commit 1
# (Tools/orca/split/s2-move.py) and is classed by the Games/** rule above: stage flows, rules, name
# tags, the match block and set block layouts, free space, and the mixed text modules (an engine
# mechanism carrying game data, classed by the data until S5 moves the mechanism to the engine).
# Three of them carry competitive policy until S2 splits it out: the queue's ready/timer/skip machine,
# ranked stocks and minutes (RankedSet), and the winner and set result the room rates (Results).
game-logic        Source/Core/Core/Orca/Games/Brawl/{Queue,RankedSet,Results}.* review
"""
LAYERS_TESTS_OLD = """\
game-logic        Source/UnitTests/Core/Orca/{BrawlStages,CharOrder,CssTitle,NativeText}Test.cpp dev
game-logic        Source/UnitTests/Core/Orca/{OnlineMenu,OnlineRules,OnlineSeats,Queue}Test.cpp dev
game-logic        Source/UnitTests/Core/Orca/{RankedSet,Relabel,Results}Test.cpp dev
"""
LAYERS_TESTS_NEW = """\
game-logic        Source/UnitTests/Core/Orca/Games/**     dev     # game-module tests (S2)
"""


def git(root, *args):
    return subprocess.run(["git", "-C", root, *args], check=True, stdout=subprocess.PIPE,
                          text=True).stdout


def moves(root):
    out = []
    for name, sub in MODULES.items():
        for ext in (".h", ".cpp"):
            src = UX + name + ext
            if os.path.exists(os.path.join(root, src)):
                out.append((src, GAMES + sub + name + ext))
    for name in GAME_TESTS:
        src = TESTS + name + "Test.cpp"
        if os.path.exists(os.path.join(root, src)):
            out.append((src, GAME_TESTS_DIR + name + "Test.cpp"))
    return out


def rewrite_text(path, text):
    def mod(m):
        return "Orca/Games/Brawl/" + MODULES[m.group(1)] + m.group(1) + "." + m.group(2)
    new = MOD_RE.sub(mod, text)
    new = TEST_RE.sub(lambda m: "Games/Brawl/" + m.group(1) + "Test.cpp", new)
    if path.startswith(GAME_TESTS_DIR):
        # Two directories deeper than before: tests that find Data/Sys from __FILE__.
        new = re.sub(r'(__FILE__\)\.parent_path\(\) / ")\.\./\.\./\.\./\.\./(?!\.\./)',
                     r"\1../../../../../../", new)
    if path == "Tools/orca/layers.txt":
        new = new.replace(LAYERS_OLD, LAYERS_NEW).replace(LAYERS_TESTS_OLD, LAYERS_TESTS_NEW)
    return new


def candidates(root):
    for p in git(root, "ls-files", "-z").split("\0"):
        if not p or p in SKIP or not p.startswith(REWRITE_ROOTS):
            continue
        if p.endswith(".md") or not (p.endswith(REWRITE_EXT) or os.path.basename(p) in
                                     ("forbidden-calls", "layer-of-diff")):
            continue
        yield p


def rewrite(root):
    changed = []
    for p in candidates(root):
        full = os.path.join(root, p)
        if not os.path.isfile(full):
            continue
        with open(full, encoding="utf-8", newline="") as f:
            try:
                text = f.read()
            except UnicodeDecodeError:
                continue
        new = rewrite_text(p, text)
        if new != text:
            with open(full, "w", encoding="utf-8", newline="") as f:
                f.write(new)
            changed.append(p)
    return changed


def check(root):
    left = []
    for p in candidates(root):
        full = os.path.join(root, p)
        if not os.path.isfile(full):
            continue
        try:
            text = open(full, encoding="utf-8").read()
        except UnicodeDecodeError:
            continue
        for i, line in enumerate(text.splitlines(), 1):
            if MOD_RE.search(line) or TEST_RE.search(line):
                left.append("%s:%d: %s" % (p, i, line.strip()))
    stray = [s for s, _ in moves(root)]
    return left, stray


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rewrite-only", action="store_true")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    root = git(os.path.dirname(os.path.abspath(__file__)), "rev-parse", "--show-toplevel").strip()

    if args.check:
        left, stray = check(root)
        for s in stray:
            print("not moved: " + s)
        for l in left:
            print("old path: " + l)
        print("s2-move check: %d old paths, %d files not moved" % (len(left), len(stray)))
        return 1 if left or stray else 0

    if not args.rewrite_only:
        todo = moves(root)
        for src, dst in todo:
            os.makedirs(os.path.join(root, os.path.dirname(dst)), exist_ok=True)
            git(root, "mv", src, dst)
        print("moved %d files" % len(todo))
    changed = rewrite(root)
    for p in changed:
        print("rewrote " + p)
    if changed:
        git(root, "add", "--", *changed)
    print("rewrote %d files" % len(changed))
    return 0


if __name__ == "__main__":
    sys.exit(main())
