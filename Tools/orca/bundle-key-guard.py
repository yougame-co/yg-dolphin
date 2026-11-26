#!/usr/bin/env python3
# Copyright 2026 YouGame
# SPDX-License-Identifier: GPL-2.0-or-later
"""Refuse a package build that trusts the bundle-flat TEST key (ORCA.md "Flat bundles").

  python Tools/orca/bundle-key-guard.py <build-dir> <program>

The same five checks orca-package-macos.sh and orca-package-linux.sh make, for the Windows release
script (on the PC, outside this repo), which has no `strings` or `grep`. That script runs it after
the build and before it signs or zips anything:

  1. ORCA_BUNDLE_PUBLIC_KEY, when set, is 64 lowercase hex digits and not the TEST key.
  2. <build-dir>/CMakeCache.txt says ORCA_BUNDLE_TRUST_TEST_KEY:BOOL=OFF (configure with
     -DORCA_BUNDLE_TRUST_TEST_KEY=OFF -DORCA_BUNDLE_PUBLIC_KEY=<key or empty> on every run, so an
     older cache can't keep it on).
  3. <program> (e.g. Binaries\\Release\\yg-baiji.exe) doesn't hold the TEST key's text, and holds the
     pinned key's when one is set (Bundle.cpp keeps both as ASCII hex).
  4. The cache says ORCA_SIGNIN_PREVIEW:BOOL=OFF and <program> doesn't hold ORCA_BO_SIGNIN_PREVIEW:
     no release carries the sign-in screen's screenshot preview (UX/BrawlOnlineSignIn.cpp).
  5. The cache says ORCA_BO_LIVE:BOOL=OFF and <program> holds neither brawlonline.net (any case) nor
     ORCA_BO_LIVE_TRANSPORT: no release can reach Brawl Online's real servers
     (Core/Orca/BrawlOnlineAccount.h).

The STAGING flat-bundle key (public half below) is refused the same way, as ORCA_BUNDLE_PUBLIC_KEY
and in the program, unless ORCA_STAGING_BUILD=1 marks a staging build (YouGame's staging scripts
set it); ORCA_STAGING_BUILD is 1, 0 or unset.

Exit 0 when all hold, 1 with the reason otherwise. Prints no key.
"""
import os
import re
import sys

# The bundle-flat TEST key (Source/Core/Core/Orca/Bundle.cpp).
TEST_BUNDLE_KEY = "afb9463d5c16f0f617432d76f75f37a8c649e36d7c626088ba00b19dea115a43"
# The variable only the sign-in screen's screenshot preview reads (ORCA_SIGNIN_PREVIEW builds).
PREVIEW_ENV = b"ORCA_BO_SIGNIN_PREVIEW"
# Brawl Online's host, and the text only its live transport carries (ORCA_BO_LIVE builds).
BO_LIVE_HOST = b"brawlonline.net"
BO_LIVE_MARKER = b"ORCA_BO_LIVE_TRANSPORT"
# The STAGING flat-bundle key's public half (staging builds only, 2026-10-12; orca-package-macos.sh
# and orca-package-linux.sh carry the same value).
STAGING_BUNDLE_KEY = "8422b8b28b82aff024427d77e6c72a38d9ea75bcb6533fe706c61006f742c0ab"


def fail(why: str) -> None:
    print(f"bundle-key-guard: {why}", file=sys.stderr)
    sys.exit(1)


def main() -> None:
    if len(sys.argv) != 3:
        fail("usage: bundle-key-guard.py <build-dir> <program>")
    build_dir, program = sys.argv[1], sys.argv[2]
    staging_flag = os.environ.get("ORCA_STAGING_BUILD", "")
    if staging_flag not in ("", "0", "1"):
        fail(f"ORCA_STAGING_BUILD is 1 or unset, not {staging_flag!r}")
    staging = staging_flag == "1"
    pinned = os.environ.get("ORCA_BUNDLE_PUBLIC_KEY", "")
    if pinned:
        if not re.fullmatch(r"[0-9a-f]{64}", pinned):
            fail("ORCA_BUNDLE_PUBLIC_KEY is not 64 lowercase hex digits")
        if pinned == TEST_BUNDLE_KEY:
            fail("ORCA_BUNDLE_PUBLIC_KEY is the TEST key")
        if pinned == STAGING_BUNDLE_KEY and not staging:
            fail("ORCA_BUNDLE_PUBLIC_KEY is the STAGING key: only a staging build (ORCA_STAGING_BUILD=1) pins it")
    try:
        with open(os.path.join(build_dir, "CMakeCache.txt"), encoding="utf-8", errors="replace") as f:
            cache = f.read().splitlines()
    except OSError as e:
        fail(f"can't read CMakeCache.txt: {e}")
    if "ORCA_BUNDLE_TRUST_TEST_KEY:BOOL=OFF" not in cache:
        fail("CMake's cache still trusts the bundle-flat TEST key (or doesn't name the option)")
    if "ORCA_SIGNIN_PREVIEW:BOOL=OFF" not in cache:
        fail("CMake's cache builds the sign-in screen's screenshot preview (or doesn't name the option)")
    if "ORCA_BO_LIVE:BOOL=OFF" not in cache:
        fail("CMake's cache lets the Brawl Online sign-in reach its real servers (or doesn't name the option)")
    try:
        with open(program, "rb") as f:
            data = f.read()
    except OSError as e:
        fail(f"can't read {program}: {e}")
    if not data:
        fail(f"{program} is empty")
    if PREVIEW_ENV in data:
        fail("the program carries the sign-in screen's screenshot preview")
    if BO_LIVE_HOST in data.lower() or BO_LIVE_MARKER in data:
        fail("the program can reach Brawl Online's real servers (built with ORCA_BO_LIVE)")
    if TEST_BUNDLE_KEY.encode() in data:
        fail("the program trusts the bundle-flat TEST key")
    if not staging and STAGING_BUNDLE_KEY.encode() in data:
        fail("the program trusts the STAGING bundle key, and this is not a staging build")
    if pinned and pinned.encode() not in data:
        fail("the program doesn't hold ORCA_BUNDLE_PUBLIC_KEY")
    print("bundle-key-guard: ok, flat bundles: " +
          ("the pinned key only" if pinned else "off (every bundle is refused)") +
          (" (STAGING build)" if staging else ""))


if __name__ == "__main__":
    main()
