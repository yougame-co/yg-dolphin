#!/usr/bin/env python3
# Copyright 2026 YouGame
# SPDX-License-Identifier: GPL-2.0-or-later
"""Refuse a package build that trusts the bundle-flat TEST key (ORCA.md "Flat bundles").

  python Tools/orca/bundle-key-guard.py <build-dir> <program>

The same three checks orca-package-macos.sh and orca-package-linux.sh make, for the Windows release
script (on the PC, outside this repo), which has no `strings` or `grep`:

  1. ORCA_BUNDLE_PUBLIC_KEY, when set, is 64 lowercase hex digits and not the TEST key.
  2. <build-dir>/CMakeCache.txt says ORCA_BUNDLE_TRUST_TEST_KEY:BOOL=OFF (configure with
     -DORCA_BUNDLE_TRUST_TEST_KEY=OFF -DORCA_BUNDLE_PUBLIC_KEY=<key or empty> on every run, so an
     older cache can't keep it on).
  3. <program> (e.g. Binaries\\Release\\yg-baiji.exe) doesn't hold the TEST key's text, and holds the
     pinned key's when one is set (Bundle.cpp keeps both as ASCII hex).

Exit 0 when all hold, 1 with the reason otherwise. Prints no key.
"""
import os
import re
import sys

# The bundle-flat TEST key (Source/Core/Core/Orca/Bundle.cpp).
TEST_BUNDLE_KEY = "afb9463d5c16f0f617432d76f75f37a8c649e36d7c626088ba00b19dea115a43"


def fail(why: str) -> None:
    print(f"bundle-key-guard: {why}", file=sys.stderr)
    sys.exit(1)


def main() -> None:
    if len(sys.argv) != 3:
        fail("usage: bundle-key-guard.py <build-dir> <program>")
    build_dir, program = sys.argv[1], sys.argv[2]
    pinned = os.environ.get("ORCA_BUNDLE_PUBLIC_KEY", "")
    if pinned:
        if not re.fullmatch(r"[0-9a-f]{64}", pinned):
            fail("ORCA_BUNDLE_PUBLIC_KEY is not 64 lowercase hex digits")
        if pinned == TEST_BUNDLE_KEY:
            fail("ORCA_BUNDLE_PUBLIC_KEY is the TEST key")
    try:
        with open(os.path.join(build_dir, "CMakeCache.txt"), encoding="utf-8", errors="replace") as f:
            cache = f.read().splitlines()
    except OSError as e:
        fail(f"can't read CMakeCache.txt: {e}")
    if "ORCA_BUNDLE_TRUST_TEST_KEY:BOOL=OFF" not in cache:
        fail("CMake's cache still trusts the bundle-flat TEST key (or doesn't name the option)")
    try:
        with open(program, "rb") as f:
            data = f.read()
    except OSError as e:
        fail(f"can't read {program}: {e}")
    if not data:
        fail(f"{program} is empty")
    if TEST_BUNDLE_KEY.encode() in data:
        fail("the program trusts the bundle-flat TEST key")
    if pinned and pinned.encode() not in data:
        fail("the program doesn't hold ORCA_BUNDLE_PUBLIC_KEY")
    print("bundle-key-guard: ok, flat bundles: " +
          ("the pinned key only" if pinned else "off (every bundle is refused)"))


if __name__ == "__main__":
    main()
