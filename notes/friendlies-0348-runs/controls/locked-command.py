#!/usr/bin/env python3
"""Recheck load after acquiring the shared lock, then replace this process with the job."""
import os
import sys

from load_gate import wait_for_load

wait_for_load()
os.execvp(sys.argv[1], sys.argv[1:])
