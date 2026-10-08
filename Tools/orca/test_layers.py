#!/usr/bin/env python3
# Copyright 2026 YouGame
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests for layers.txt, layer-of-diff and forbidden-calls.

  python3 Tools/orca/test_layers.py        (standard library only; needs git)

The map tests read this checkout: every tracked path must resolve to exactly one class, and every
path in Orca's own code must be classed by a rule other than the `**` fallback.
"""

import importlib.machinery
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import layerlib  # noqa: E402


def load_script(name):
    path = os.path.join(HERE, name)
    loader = importlib.machinery.SourceFileLoader(name.replace("-", "_"), path)
    spec = importlib.util.spec_from_loader(loader.name, loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


fc = load_script("forbidden-calls")


def rules_of(text):
    return layerlib.parse_rules("engine **\n" + text)


class GlobTests(unittest.TestCase):
    def match(self, glob, path):
        return bool(layerlib.re.match(layerlib.glob_to_regex(glob), path))

    def test_star_stays_in_one_segment(self):
        self.assertTrue(self.match("Source/*.cpp", "Source/a.cpp"))
        self.assertFalse(self.match("Source/*.cpp", "Source/x/a.cpp"))

    def test_double_star_spans_segments(self):
        self.assertTrue(self.match("Source/**", "Source/a/b/c.cpp"))
        self.assertTrue(self.match("**/*.md", "ORCA.md"))
        self.assertTrue(self.match("**/*.md", "docs/a/b.md"))
        self.assertTrue(self.match("a/**/b.h", "a/b.h"))
        self.assertTrue(self.match("a/**/b.h", "a/x/y/b.h"))
        self.assertFalse(self.match("a/**/b.h", "a/xb.h"))
        self.assertTrue(self.match("**", "anything/at/all"))

    def test_question_and_class(self):
        self.assertTrue(self.match("a?.h", "ab.h"))
        self.assertFalse(self.match("a?.h", "a/.h"))
        self.assertTrue(self.match("RSB[EP]01.ini", "RSBE01.ini"))
        self.assertFalse(self.match("RSB[EP]01.ini", "RSBJ01.ini"))

    def test_whole_path_only(self):
        self.assertFalse(self.match("Orca/*", "Source/Orca/a.cpp"))
        self.assertFalse(self.match("a.cpp", "a.cppx"))

    def test_bad_double_star(self):
        with self.assertRaises(layerlib.RulesError):
            layerlib.glob_to_regex("a**b")

    def test_braces(self):
        self.assertEqual(layerlib.expand_braces("UX/{A,B}.{h,cpp}"),
                         ["UX/A.h", "UX/A.cpp", "UX/B.h", "UX/B.cpp"])
        self.assertEqual(layerlib.expand_braces("plain"), ["plain"])

    def test_literal_count(self):
        self.assertEqual(layerlib.literal_count("**"), 0)
        self.assertEqual(layerlib.literal_count("a/*.h"), 4)
        self.assertEqual(layerlib.literal_count("RSB[EP]01"), 5)


class RuleTests(unittest.TestCase):
    def test_most_specific_wins(self):
        r = rules_of("game-logic UX/**\nengine UX/Overlay.*\n")
        self.assertEqual(r.classify("UX/Queue.cpp").cls, "game-logic")
        self.assertEqual(r.classify("UX/Overlay.cpp").cls, "engine")
        self.assertEqual(r.classify("Other/x.cpp").cls, "engine")
        self.assertTrue(r.classify("Other/x.cpp").fallback)
        self.assertFalse(r.classify("UX/Queue.cpp").fallback)

    def test_order_does_not_matter(self):
        a = rules_of("engine UX/Overlay.*\ngame-logic UX/**\n")
        self.assertEqual(a.classify("UX/Overlay.cpp").cls, "engine")

    def test_tie_with_different_class_is_ambiguous(self):
        r = rules_of("game-logic UX/A*\nengine UX/*A\n")
        self.assertTrue(r.classify("UX/AA").ties)
        same = rules_of("engine UX/A*\nengine UX/*A\n")
        self.assertFalse(same.classify("UX/AA").ties)
        flags = rules_of("engine UX/A*\nengine UX/*A dev\n")
        self.assertTrue(flags.classify("UX/AA").ties)

    def test_dev_comes_from_the_winning_rule(self):
        r = rules_of("engine Tools/** dev\nengine-sensitive Tools/release.sh dev\n"
                     "game-data Tools/inputs/**\n")
        self.assertTrue(r.classify("Tools/x.py").dev)
        self.assertEqual(r.classify("Tools/release.sh").cls, "engine-sensitive")
        self.assertFalse(r.classify("Tools/inputs/a.txt").dev)

    def test_parse_errors(self):
        for bad in ["engine\n", "enginee **\n", "engine ** shipped\n", "engine /abs\n",
                    "%nope x\n", "%orca-root\n"]:
            with self.assertRaises(layerlib.RulesError, msg=bad):
                layerlib.parse_rules("engine **\n" + bad)
        with self.assertRaises(layerlib.RulesError):
            layerlib.parse_rules("engine a/**\n")  # no fallback

    def test_comments_and_directives(self):
        r = layerlib.parse_rules("# x\n%orca-root A/\n%upstream-ref up/master\n"
                                 "%upstream-base abc\nengine **   # fallback\n")
        self.assertEqual(r.orca_roots, ["A/"])
        self.assertEqual((r.upstream_ref, r.upstream_base), ("up/master", "abc"))


class RealMapTests(unittest.TestCase):
    """The checked-in layers.txt against this checkout."""

    @classmethod
    def setUpClass(cls):
        cls.rules = layerlib.load_rules()
        cls.repo = layerlib.repo_root()

    def test_every_tracked_path_has_one_class(self):
        tracked = [p for p in layerlib.git(self.repo, "ls-files", "-z").split("\0") if p]
        self.assertGreater(len(tracked), 1000)
        ambiguous = []
        for p in tracked:
            m = self.rules.classify(p)
            if m.ties:
                ambiguous.append("%s: %s vs %s" % (p, m.rule, m.ties))
        self.assertEqual(ambiguous, [])

    def test_every_orca_path_is_classed_on_purpose(self):
        paths, _ = layerlib.orca_paths(self.repo, self.rules, "HEAD")
        self.assertGreater(len(paths), 300)
        for root in self.rules.orca_roots:
            self.assertTrue(any(p.startswith(root) for p in paths), root)
        fallback = [p for p in paths if self.rules.classify(p).fallback]
        self.assertEqual(fallback, [], "Orca paths only the ** fallback classes; add a rule")

    def test_known_paths(self):
        expect = {
            "Data/Sys/Orca/RSBE01.patches": ("game-data", False),
            "Data/Sys/Orca/PPLUS32.ini": ("game-data", False),
            "Data/Sys/Orca/RSBE01.jitwarm": ("game-data", False),
            "Data/Sys/Orca/Textures/RSBE01/x.png": ("game-data", False),
            "Data/Sys/Orca/Fonts/Roboto-Medium.ttf": ("engine", False),
            "Data/Sys/GameSettings/RSBE01.ini": ("game-data", False),
            "Data/Sys/GameSettings/GALE01.ini": ("engine", False),
            "Source/Core/Core/Orca/UX/OnlineRules.cpp": ("game-logic", False),
            "Source/Core/Core/Orca/UX/BrawlStages.h": ("game-logic", False),
            "Source/Core/Core/Orca/UX/RankedPPlus.cpp": ("game-logic", False),
            "Source/Core/Core/Orca/UX/FreeSpace.h": ("game-logic", False),
            "Source/Core/Core/Orca/UX/MenuText.cpp": ("game-logic", False),
            "Source/Core/Core/Orca/UX/RankedSteps.cpp": ("engine", False),
            "Source/Core/Core/Orca/UX/Overlay.cpp": ("engine", False),
            "Source/Core/Core/Orca/UX/UX.cpp": ("engine", False),
            "Source/Core/Core/Orca/UX/GamePatches.cpp": ("engine-sensitive", False),
            "Source/Core/Core/Orca/UX/Chat.cpp": ("engine-sensitive", False),
            "Source/Core/Core/Orca/Games/BrawlRev2/Module.cpp": ("game-logic", False),
            "Source/Core/Core/Orca/Session/YouGameRoom.cpp": ("engine-sensitive", False),
            "Source/Core/Core/Orca/Session/Session.cpp": ("engine-sensitive", False),
            "Source/Core/Core/Orca/Session/Events.h": ("engine", False),
            "Source/Core/Core/Orca/Profile.cpp": ("engine-sensitive", False),
            "Source/Core/Core/Orca/Music.cpp": ("engine", False),
            "Source/Core/Core/Orca/Mod.cpp": ("engine-sensitive", False),
            "Source/Core/Core/Rollback/Rollback.cpp": ("engine", False),
            "Source/Core/Core/Rollback/OnlineMatch.cpp": ("engine-sensitive", False),
            "Source/Core/Core/PowerPC/JitArm64/Jit.cpp": ("engine", False),
            "Source/Core/Core/HLE/HLE.cpp": ("engine", False),
            "Source/Core/Core/State.cpp": ("engine-sensitive", False),
            "Source/Core/Core/Core.cpp": ("engine", False),
            "Source/Core/Core/IOS/Network/SSL.cpp": ("engine-sensitive", False),
            "Source/Core/Core/IOS/IOS.cpp": ("engine", False),
            "Source/Core/Common/WebSocket.cpp": ("engine-sensitive", False),
            "Source/Core/Common/Timer.cpp": ("engine", False),
            "Source/Core/DolphinNoGUI/Orca.entitlements": ("engine-sensitive", False),
            "Source/Core/VideoCommon/HiresTextures.cpp": ("engine-sensitive", False),
            "Source/Core/VideoCommon/Present.cpp": ("engine", False),
            "Source/Core/DolphinQt/MainWindow.cpp": ("engine", False),
            "Externals/libjuice/CMakeLists.txt": ("engine-sensitive", False),
            "Externals/licenses.md": ("engine-sensitive", False),
            "CMakeLists.txt": ("engine-sensitive", False),
            "ORCA.md": ("engine", True),
            "Source/UnitTests/Core/Orca/QueueTest.cpp": ("game-logic", True),
            "Source/UnitTests/Core/Orca/RankedTest.cpp": ("game-logic", True),
            "Source/UnitTests/Core/Orca/RoomTest.cpp": ("engine", True),
            "Tools/orca/queue-e2e.mjs": ("engine", True),
            "Tools/orca/inputs/bf-dropin.txt": ("game-data", True),
            "Tools/orca/textures/make-labels.py": ("game-data", True),
            "Tools/orca/layers.txt": ("engine-sensitive", True),
            "Tools/orca/layer-of-diff": ("engine-sensitive", True),
            "Tools/orca/layerlib.py": ("engine-sensitive", True),
            "Tools/orca/forbidden-calls": ("engine-sensitive", True),
            "Tools/orca/forbidden-calls.baseline": ("engine-sensitive", True),
            "Tools/orca/test_layers.py": ("engine-sensitive", True),
            "Tools/orca-package-macos.sh": ("engine-sensitive", True),
        }
        for path, (cls, dev) in expect.items():
            m = self.rules.classify(path)
            self.assertEqual((m.cls, m.dev), (cls, dev), "%s matched %s" % (path, m.rule))
            self.assertFalse(m.ties, path)


DIFF = """diff --git a/A.cpp b/A.cpp
index 1..2 100644
--- a/A.cpp
+++ b/A.cpp
@@ -1,3 +1,4 @@
 keep
--- not a header, a removed line starting with --
+++ not a header, an added line starting with ++
+added
 keep
diff --git a/New.h b/New.h
new file mode 100644
--- /dev/null
+++ b/New.h
@@ -0,0 +1,2 @@
+one
+two
diff --git a/Gone.h b/Gone.h
deleted file mode 100644
--- a/Gone.h
+++ /dev/null
@@ -1 +0,0 @@
-bye
diff --git a/Old.cpp b/Moved.cpp
similarity index 90%
rename from Old.cpp
rename to Moved.cpp
--- a/Old.cpp
+++ b/Moved.cpp
@@ -1 +1 @@
-x
+y
diff --git a/pic.png b/pic.png
Binary files a/pic.png and b/pic.png differ
"""


class DiffParseTests(unittest.TestCase):
    def test_unified_diff(self):
        got = {c.path: (c.added, c.deleted, c.binary) for c in layerlib.parse_unified_diff(DIFF)}
        self.assertEqual(got["A.cpp"], (2, 1, False))
        self.assertEqual(got["New.h"], (2, 0, False))
        self.assertEqual(got["Gone.h"], (0, 1, False))
        self.assertEqual(got["Old.cpp"][1], 1)
        self.assertEqual(got["Moved.cpp"][0], 1)
        self.assertTrue(got["pic.png"][2])
        self.assertEqual(set(got), {"A.cpp", "New.h", "Gone.h", "Old.cpp", "Moved.cpp", "pic.png"})

    def test_plain_diff_u_with_two_files(self):
        text = ("--- a/x.txt\t2026-01-01\n+++ b/x.txt\t2026-01-02\n@@ -1,2 +1,2 @@\n-a\n+b\n c\n"
                "--- a/y.txt\n+++ b/y.txt\n@@ -1 +1,2 @@\n y\n+z\n")
        got = {c.path: (c.added, c.deleted) for c in layerlib.parse_unified_diff(text)}
        self.assertEqual(got, {"x.txt": (1, 1), "y.txt": (1, 0)})

    def test_numstat(self):
        got = layerlib.parse_numstat("3\t1\tA.cpp\0-\t-\tpic.png\0")
        self.assertEqual([(c.path, c.added, c.deleted, c.binary) for c in got],
                         [("A.cpp", 3, 1, False), ("pic.png", 0, 0, True)])


def tokens(path, text):
    return [(rule, token) for rule, token, _, _ in fc.scan_text(path, text)]


class ScanTests(unittest.TestCase):
    UX = "Source/Core/Core/Orca/UX/Queue.cpp"
    ROOM = "Source/Core/Core/Orca/Session/YouGameRoom.cpp"

    def test_comments_are_blanked_strings_kept(self):
        text = ('a(); // connect(fd)\n/* getenv("X")\n system("y") */ b();\n'
                's = "// not a comment"; c = \'/\'; n = 1\'000;\n')
        out = fc.strip_c_comments(text)
        self.assertNotIn("connect", out)
        self.assertNotIn("getenv", out)
        self.assertIn('"// not a comment"', out)
        self.assertEqual(out.count("\n"), text.count("\n"))

    def test_raw_string(self):
        out = fc.strip_c_comments('x = R"(a // b)"; // c\n')
        self.assertIn("a // b", out)
        self.assertNotIn("// c", out)

    def test_socket(self):
        self.assertIn(("socket", "connect("), tokens(self.UX, "int r = connect(fd, a, n);\n"))
        self.assertIn(("socket", "::bind("), tokens(self.UX, "::bind(fd, a, n);\n"))
        self.assertIn(("socket", "#include sys/socket.h"), tokens(self.UX, "#include <sys/socket.h>\n"))
        self.assertEqual(tokens(self.UX, "auto f = std::bind(g, 1);\nm_ws.send(x);\nroom->connect(y);\n"),
                         [])

    def test_words_inside_strings_are_not_calls(self):
        self.assertEqual(tokens(self.UX, 'Log("character select (frame {})", f);\n'), [])
        self.assertEqual(tokens(self.UX, 'auto s = R"(system("x"))";\n'), [])

    def test_http(self):
        self.assertIn(("http", "curl_easy_init("), tokens(self.UX, "CURL* c = curl_easy_init();\n"))
        self.assertIn(("http", '"https://evil.example/x"'),
                      tokens(self.UX, 'const char* u = "https://evil.example/x";\n'))
        self.assertIn(("http", "Common::HttpRequest"), tokens(self.UX, "Common::HttpRequest req;\n"))

    def test_file_open_outside_user_dir(self):
        self.assertIn(("file-open", "std::fopen("), tokens(self.UX, 'FILE* f = std::fopen("/etc/x", "r");\n'))
        self.assertIn(("file-open", "File::IOFile"), tokens(self.UX, 'File::IOFile f(path, "rb");\n'))
        self.assertIn(("file-open", "std::ofstream"), tokens(self.UX, "std::ofstream out(p);\n"))
        self.assertEqual(tokens(self.UX, 'File::IOFile f(File::GetUserPath(D_USER_IDX) + "x", "wb");\n'), [])
        self.assertEqual(tokens(self.UX, "fs.Rename(a, b);\nHostFileSystem::Delete(a);\n"), [])

    def test_process_dynload_env(self):
        self.assertIn(("process", 'std::system("rm-rf"'), tokens(self.UX, 'std::system("rm-rf");\n'))
        self.assertIn(("process", "posix_spawn("), tokens(self.UX, "posix_spawn(&pid, p, 0, 0, a, e);\n"))
        self.assertIn(("dynload", 'dlopen("libx.dylib"'), tokens(self.UX, 'void* h = dlopen("libx.dylib", 0);\n'))
        self.assertIn(("dynload", "LoadLibraryW("), tokens(self.UX, "LoadLibraryW(name);\n"))
        self.assertIn(("env", 'std::getenv("ORCA_X"'), tokens(self.UX, 'auto v = std::getenv("ORCA_X");\n'))
        self.assertIn(("env", 'GetEnv("ORCA_Y"'), tokens(self.UX, 'auto v = GetEnv("ORCA_Y");\n'))

    def test_peer_parse_only_in_peer_files(self):
        line = "std::memcpy(&x, data, 4); auto s = v.get<std::string>();\n"
        self.assertIn(("peer-parse", "std::memcpy("), tokens(self.ROOM, line))
        self.assertIn(("peer-parse", ".get<"), tokens(self.ROOM, line))
        self.assertEqual(tokens(self.UX, line), [])

    def test_cmake(self):
        text = ("# find_package(Commented)\nfind_package(CURL REQUIRED)\n"
                "target_link_libraries(core\n  PUBLIC common\n  PRIVATE juice $<$<BOOL:WIN32>:ws2_32>\n)\n")
        got = tokens("Source/Core/Core/CMakeLists.txt", text)
        self.assertIn(("build-deps", "find_package(CURL)"), got)
        self.assertIn(("build-deps", "target_link_libraries(core juice)"), got)
        self.assertIn(("build-deps", "target_link_libraries(core common)"), got)
        self.assertNotIn(("build-deps", "find_package(Commented)"), got)

    def test_baseline_round_trip(self):
        from collections import Counter
        counts = Counter({("env", "a.cpp", 'getenv("X"'): 2, ("socket", "b.cpp", "connect("): 1})
        text = fc.format_baseline(counts, "abc")
        self.assertEqual(fc.load_baseline(text), counts)
        with self.assertRaises(ValueError):
            fc.load_baseline("1\tnot-a-rule\ta\tb\n")


class EndToEndTests(unittest.TestCase):
    """Both scripts against a scratch repository with a fake upstream base."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="orca-layers-test-")
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def git(self, *args):
        return subprocess.run(["git", "-C", self.tmp, "-c", "user.name=t", "-c", "user.email=t@t",
                               "-c", "commit.gpgsign=false"] + list(args),
                              check=True, stdout=subprocess.PIPE).stdout.decode().strip()

    def write(self, path, text):
        full = os.path.join(self.tmp, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w") as f:
            f.write(text)

    def run_tool(self, name, *args):
        p = subprocess.run([sys.executable, os.path.join(HERE, name), "--repo", self.tmp] + list(args),
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return p.returncode, p.stdout.decode(), p.stderr.decode()

    def test_flow(self):
        self.git("init", "-q")
        self.write("Source/Core/Common/Util.cpp", 'const char* p = getenv("PATH");\n')
        self.git("add", "-A")
        self.git("commit", "-qm", "upstream")
        base = self.git("rev-parse", "HEAD")
        rules = os.path.join(self.tmp, "rules.txt")
        with open(os.path.join(HERE, "layers.txt")) as f:
            text = f.read()
        text = text.replace("%upstream-ref upstream/master", "%upstream-ref nope/master")
        text = layerlib.re.sub(r"%upstream-base \w+", "%upstream-base " + base, text)
        with open(rules, "w") as f:
            f.write(text)
        self.write("Source/Core/Core/Orca/UX/Queue.cpp", "void Frame() {}\n")
        self.write("Data/Sys/Orca/RSBE01.patches", "x\n")
        self.git("add", "Source", "Data")
        self.git("commit", "-qm", "orca")
        bl = os.path.join(self.tmp, "bl.txt")
        code, out, err = self.run_tool("forbidden-calls", "--rules", rules, "--update-baseline",
                                       "--baseline", bl)
        self.assertEqual(code, 0, err)

        # A data-only change.
        self.write("Data/Sys/Orca/RSBE01.patches", "x\ny\n")
        self.git("commit", "-qam", "data")
        code, out, err = self.run_tool("layer-of-diff", "--rules", rules, "--json", "HEAD")
        self.assertEqual(code, 0, err)
        s = json.loads(out)
        self.assertEqual((s["highest"], s["highest_shipped"]), ("game-data", "game-data"))

        # Game logic that opens a socket.
        self.write("Source/Core/Core/Orca/UX/Queue.cpp",
                   'void Frame() {}\nvoid Leak() { connect(3, 0, 0); getenv("HOME"); }\n')
        self.git("commit", "-qam", "logic")
        code, out, err = self.run_tool("layer-of-diff", "--rules", rules, "--max", "game-data", "HEAD~1..HEAD")
        self.assertEqual(code, 1, out + err)
        self.assertIn("REFUSED", out)
        code, out, err = self.run_tool("layer-of-diff", "--rules", rules, "--max", "game-logic", "HEAD")
        self.assertEqual(code, 0, out + err)
        code, out, err = self.run_tool("forbidden-calls", "--rules", rules, "--baseline", bl,
                                       "--rev", "HEAD", "--since", "HEAD~1", "--json")
        self.assertEqual(code, 1, err)
        new = json.loads(out)["new"]
        self.assertEqual(sorted((n["rule"], n["token"]) for n in new),
                         [("env", 'getenv("HOME"'), ("socket", "connect(")])
        self.assertTrue(all(l["added"] for n in new for l in n["lines"]))

        # Orca's first change to an upstream file: only what Orca adds is new.
        self.write("Source/Core/Common/Util.cpp",
                   'const char* p = getenv("PATH");\nconst char* h = getenv("HOME");\n')
        self.git("commit", "-qam", "upstream-ish")
        bl2 = os.path.join(self.tmp, "bl2.txt")
        self.run_tool("forbidden-calls", "--rules", rules, "--update-baseline", "--baseline", bl2,
                      "--rev", "HEAD~3")
        code, out, err = self.run_tool("forbidden-calls", "--rules", rules, "--baseline", bl2,
                                       "--rev", "HEAD", "--json")
        got = sorted((n["rule"], n["path"], n["token"]) for n in json.loads(out)["new"])
        self.assertIn(("env", "Source/Core/Common/Util.cpp", 'getenv("HOME"'), got)
        self.assertNotIn(("env", "Source/Core/Common/Util.cpp", 'getenv("PATH"'), got)
        self.git("reset", "-q", "--hard", "HEAD~1")

        # The working tree, a stdin diff, and --files.
        code, out, err = self.run_tool("forbidden-calls", "--rules", rules, "--baseline", bl)
        self.assertEqual(code, 1, err)
        diff = self.git("diff", "HEAD~2", "HEAD")
        p = subprocess.run([sys.executable, os.path.join(HERE, "layer-of-diff"), "--repo", self.tmp,
                            "--rules", rules, "--json", "-"], input=diff.encode(), stdout=subprocess.PIPE)
        self.assertEqual(json.loads(p.stdout)["highest"], "game-logic")
        code, out, err = self.run_tool("layer-of-diff", "--rules", rules, "--json", "--files",
                                       "Tools/orca/layers.txt", "ORCA.md")
        s = json.loads(out)
        self.assertEqual((s["highest"], s["highest_shipped"]), ("engine-sensitive", None))


if __name__ == "__main__":
    unittest.main()
