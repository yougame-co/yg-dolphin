#!/usr/bin/env python3
# Copyright 2026 YouGame
# SPDX-License-Identifier: GPL-2.0-or-later
"""Runner regressions using isolated fake Orca children; no disc or external service."""
import importlib.util
import io
from pathlib import Path
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("doubles_rooms", Path(__file__).with_name("doubles-rooms.py"))
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


class RunnerTest(unittest.TestCase):
    def test_slow_second_ticket_cannot_swap_role_ports(self):
        registered = threading.Event()

        class StopAfterThirdSpawn(Exception):
            pass

        class Child:
            def __init__(self, registration):
                self.stdin = io.StringIO()
                self.stdout = iter(())
                self.registration = registration

            def poll(self):
                return None

            def wait(self, timeout=None):
                self.registration.join()
                return 0

        def spawn(command, **kwargs):
            role = int(Path(command[command.index("-u") + 1]).name.split("-")[1])
            if role == 2:
                self.assertTrue(registered.is_set(), "third role started before the second owned its seat")
                raise StopAfterThirdSpawn()
            log = kwargs["stderr"]

            def register():
                if role == 1:
                    time.sleep(0.2)  # Model a slow ticket response while the next role is ready.
                log.write(f"Orca room: in room test on seat {role + 1}, joining\n")
                log.flush()
                if role == 1:
                    registered.set()

            registration = threading.Thread(target=register)
            registration.start()
            return Child(registration)

        args = SimpleNamespace(site="http://127.0.0.1:1", room="test", hold_expiry=False,
                               roles="0,1,2", input_role=[], blackout_role=[], events=None,
                               defer_role=[], refused_role=[], transport="direct", probe=None,
                               input=None, profile=None, mod_dir=None, binary="fake-orca",
                               disc="unused", keep_logs=None)
        with patch.object(fixture.subprocess, "Popen", side_effect=spawn):
            with self.assertRaises(StopAfterThirdSpawn):
                fixture.run(args)


if __name__ == "__main__":
    unittest.main()
