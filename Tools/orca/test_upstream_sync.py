#!/usr/bin/env python3
# Copyright 2026 YouGame
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests for upstream-sync.py's triage logic (standard library only; needs git).

  python3 Tools/orca/test_upstream_sync.py
"""

import importlib.machinery
import importlib.util
import os
import shutil
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))


def load_script(name):
    loader = importlib.machinery.SourceFileLoader(name.replace("-", "_"), os.path.join(HERE, name))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


us = load_script("upstream-sync.py")


class Classify(unittest.TestCase):
    def test_classes(self):
        self.assertEqual(us.classify("NetPlay: reject oversized input buffer messages", ["Source/Core/Core/NetPlayClient.cpp"]), ["security"])
        self.assertEqual(us.classify("VideoCommon: validate restored FIFO pointers", []), ["security"])
        self.assertEqual(us.classify("Fix crashes when using std::locale{\"\"} with an invalid LC_ALL", []), ["crash"])
        self.assertEqual(us.classify("MemArenaDarwin: Eliminate use of mach_make_memory_entry_64", []), ["platform"])
        self.assertEqual(us.classify("Update to release 3.4.16", ["Externals/SDL/SDL"]), ["platform"])
        self.assertEqual(us.classify("Input Syntax: Add floor, ceil, round, and trunc functions", []), ["other"])

    def test_reach(self):
        self.assertEqual(us.reach(["Source/Core/DolphinQt/Main.cpp", "Languages/po/de.po"])[0], "not-shipped")
        self.assertEqual(us.reach(["Source/Android/app/build.gradle"])[0], "not-shipped")
        where, why = us.reach(["Source/Core/Core/HW/EXI/EXI_DeviceMic.cpp"])
        self.assertEqual(where, "unreachable?")
        self.assertTrue(why)
        self.assertEqual(us.reach(["Source/Core/Core/HW/EXI/EXI_DeviceMic.cpp",
                                   "Source/Core/VideoCommon/Fifo.cpp"])[0], "ships")
        self.assertTrue(us.shipped("Data/Sys/GameSettings/RSBE01.ini"))
        self.assertFalse(us.shipped("Data/Sys/GameSettings/GXXE01.ini"))
        self.assertEqual(us.reach(["Source/UnitTests/Core/PageFaultTest.cpp"])[0], "not-shipped")

    def test_bump(self):
        self.assertEqual(us.bump("0.3.36"), "0.3.37")
        self.assertEqual(us.bump("0.3.9"), "0.3.10")


class HashCmp(unittest.TestCase):
    def test_hashcmp(self):
        d = tempfile.mkdtemp()
        try:
            a, b = os.path.join(d, "a"), os.path.join(d, "b")
            open(a, "w").write("0 aa\n1 bb\n2 cc\nnoise\n")
            open(b, "w").write("0 aa\n1 bx\n")
            r = us.hashcmp(a, b)
            self.assertEqual((r["shared"], r["differ"], r["first_differ"]), (2, 1, 1))
            self.assertEqual(us.hashcmp(a, a)["differ"], 0)
        finally:
            shutil.rmtree(d)


class Picks(unittest.TestCase):
    """A throwaway repo: upstream changes line 1 twice (c1, then c2 on top); the release changed
    line 3 and carries c0 as a -x pick. c2 alone conflicts with the release; c1+c2 apply."""

    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.old_repo = us.REPO
        us.REPO = self.d

        def g(*a):
            return subprocess.run(["git", "-C", self.d] + list(a), check=True, stdout=subprocess.PIPE,
                                  text=True).stdout.strip()
        self.g = g
        g("init", "-q", "-b", "master")
        g("config", "user.email", "t@example.invalid")
        g("config", "user.name", "Test")

        def commit(text, msg, name="f.txt"):
            open(os.path.join(self.d, name), "w").write(text)
            g("add", name)
            g("commit", "-q", "-m", msg)
            return g("rev-parse", "HEAD")
        self.root = commit("1\n2\n3\n4\n5\n6\n", "root")
        self.c0 = commit("ok\n", "Upstream fix zero", name="z.txt")
        self.c1 = commit("1a\n2\n3\n4\n5\n6\n", "Upstream refactor")
        self.c2 = commit("1b\n2\n3\n4\n5\n6\n", "Upstream fix: reject oversized")
        g("switch", "-q", "-c", "release", self.root)
        commit("1\n2\n3 orca\n4\n5\n6\n", "Orca change")
        g("cherry-pick", "-x", self.c0)
        self.base = g("rev-parse", "HEAD")

    def tearDown(self):
        us.REPO = self.old_repo
        shutil.rmtree(self.d)

    def test_pick_and_series(self):
        self.assertEqual(us.pick_test(self.base, self.c1)[0], "clean")
        status, files = us.pick_test(self.base, self.c2)
        self.assertEqual((status, files), ("conflict", ["f.txt"]))
        self.assertEqual(us.series_test(self.base, [self.c1, self.c2])[0], "clean")
        self.assertEqual(self.g("rev-parse", "release"), self.base)  # no ref moved

    def test_in_live(self):
        mb = self.g("merge-base", self.base, "master")
        picked, subjects = us.live_equivalents(self.base, mb)
        minus = us.cherry_minus(self.base, "master", mb)
        self.assertTrue(us.in_live(self.c0, self.base, mb, picked, subjects, minus))
        self.assertFalse(us.in_live(self.c1, self.base, mb, picked, subjects, minus))
        self.assertFalse(us.in_live(self.c2, self.base, mb, picked, subjects, minus))


if __name__ == "__main__":
    unittest.main()
