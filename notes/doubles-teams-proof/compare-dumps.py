#!/usr/bin/env python3
"""Compare read-only probe dumps; offsets are hexadecimal, bytes decimal."""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("before", type=Path)
parser.add_argument("after", type=Path)
args = parser.parse_args()
before, after = args.before.read_bytes(), args.after.read_bytes()
if len(before) != len(after):
    raise SystemExit("dump lengths differ")
for offset, (old, new) in enumerate(zip(before, after)):
    if old != new:
        print(f"+0x{offset:03x}: {old:3d} -> {new:3d} ({old:02x}->{new:02x})")
