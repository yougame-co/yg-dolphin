#!/usr/bin/env python3
# Copyright 2026 YouGame
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests for compat-key-template.py (the lobby compatibility key template of a build).

  python3 Tools/orca/test_compat_key_template.py        (standard library only)

The shared vectors (mods/compat-key-vectors.json) hold keys a real Orca logged for the TEST packs;
YouGame's TS port (web/src/lib/orca-compat-keys.ts) runs the same file. When this checkout's key code
no longer has a known fingerprint, the template refuses, and so does this test: update the tool,
the TS port and the vectors together.
"""

import importlib.machinery
import importlib.util
import json
import os
import shutil
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
VECTORS = os.path.join(HERE, "mods", "compat-key-vectors.json")


def load_tool():
    path = os.path.join(HERE, "compat-key-template.py")
    loader = importlib.machinery.SourceFileLoader("compat_key_template", path)
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


ck = load_tool()


class CompatKeyTemplate(unittest.TestCase):
    def setUp(self):
        with open(VECTORS, encoding="utf-8") as f:
            self.v = json.load(f)

    def test_vectors(self):
        self.assertEqual(ck.selftest(self.v), 0)

    def test_real_keys_are_xxh3_of_their_parts(self):
        for r in self.v["real"]:
            self.assertEqual(f"orca2:{ck.xxh3_64(r['parts'].encode()):016x}", r["key"], r["name"])

    def test_key_code_is_known(self):
        self.assertIn(ck.key_code_fingerprint(ROOT), ck.KEY_CODE)

    def test_template_is_deterministic(self):
        sys_dir = os.path.join(ROOT, "Data", "Sys")
        commit = "0" * 40
        a = ck.make_template(ROOT, sys_dir, commit, "9.9.9", [])
        b = ck.make_template(ROOT, sys_dir, commit, "9.9.9", [])
        self.assertEqual(json.dumps(a, sort_keys=True), json.dumps(b, sort_keys=True))
        self.assertTrue(a["parts"].startswith(f"orca2|{commit}|api="))
        # The Brawl and Project+ games are always covered, whatever --cover says.
        self.assertIn("RSBE01", a["fill"]["sysIni"]["covers"])
        self.assertIn("PPLUS32", a["fill"]["sysIni"]["covers"])

    def test_a_changed_key_function_refuses(self):
        tmp = tempfile.mkdtemp(prefix="ck-")
        try:
            for path, _ in ck.KEY_FUNCTIONS:
                dst = os.path.join(tmp, path)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copy(os.path.join(ROOT, path), dst)
            online = os.path.join(tmp, "Source/Core/Core/Orca/Session/Online.cpp")
            with open(online, encoding="utf-8") as f:
                text = f.read()
            with open(online, "w", encoding="utf-8") as f:
                f.write(text.replace('"|ux={}.{}"', '"|ux={}+{}"'))
            self.assertNotIn(ck.key_code_fingerprint(tmp), ck.KEY_CODE)
        finally:
            shutil.rmtree(tmp)

    def test_a_tampered_template_refuses(self):
        t = json.loads(json.dumps(self.v["templates"]["train-fri-005be8ff1f"]))
        r = self.v["real"][0]
        for parts in (t["parts"].replace("{tree}", "x"), t["parts"] + "{sd}",
                      t["parts"].replace("|ux=", "|ux={"), " " + t["parts"]):
            bad = dict(t, parts=parts)
            with self.assertRaises(ck.Refused):
                ck.derive(bad, r["mod"], r["tree"], r["sd"])


if __name__ == "__main__":
    unittest.main()
