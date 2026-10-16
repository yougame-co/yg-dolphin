#!/usr/bin/env python3
"""Recheck load after acquiring the shared lock, then replace this process with the job."""
import os
import subprocess
import sys

subprocess.run(['uptime'], check=True)
roles = sys.argv[sys.argv.index('--roles') + 1].split(',') if '--roles' in sys.argv else []
limit = 14 if 0 < len(roles) <= 3 else 12
if os.getloadavg()[0] >= limit:
    print(f'Load is at least {limit}; releasing the lock to leave headroom below 18.', flush=True)
    raise SystemExit(75)
os.execvp(sys.argv[1], sys.argv[1:])
