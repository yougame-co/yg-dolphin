#!/usr/bin/env python3
"""Run the train binary with the authorized read-only disc; use the loopback wrapper."""
import os
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
os.environ['ORCA_TEST_DISC'] = '/Volumes/YGWorktrees/OSCOO/ROMs/Wii/Super Smash Bros. Brawl (USA, Canada) (Rev 2).wbfs'
binary = root / 'build-orca-tests/Binaries/Tests/tests'
os.chdir(root)
os.execv(str(binary), [str(binary), '--gtest_color=no'] + sys.argv[1:])
