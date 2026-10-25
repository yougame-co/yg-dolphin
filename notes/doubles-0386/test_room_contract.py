#!/usr/bin/env python3
# Copyright 2026 YouGame
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the doubles handoff covers every requested wire field and cites live source lines."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
DOC = Path(__file__).with_name("room-contract.md")


class RoomContract(unittest.TestCase):
    def test_room_contract_covers_wire_and_has_current_citations(self):
        self.assertTrue(DOC.is_file(), "missing doubles-lead room contract")
        text = DOC.read_text()
        for heading, fields in {
            "Ticket request": ["queue", "doubles", "players", "mode", "lobby", "ls",
                               "maxLocalPlayers", "compatibility", "ranked"],
            "Welcome": ["queue", "doubles", "size", "team", "mode", "localIndex"],
            "Ranked refusal": ["Room: ranked doubles needs a newer YG Dolphin", "room_mismatch"],
            "Results and reports": ["per seat", "team", "game-report", "finish", "orca result",
                                    "unrated", "not in this build"],
        }.items():
            match = re.search(r"^## " + heading + r"\n(.*?)(?=^## |\Z)", text, re.M | re.S)
            self.assertIsNotNone(match, heading)
            section = match[1]
            for field in fields:
                self.assertIn(field, section, (heading, field))
            citations = re.findall(r"`(Source/[^`:\s]+):(\d+)` — `([^`]+)`", section)
            self.assertTrue(citations, "each answer needs anchored code evidence: " + heading)
            for path, number, fragment in citations:
                with self.subTest(path=path, line=number):
                    source = (ROOT / path).read_text().splitlines()
                    self.assertIn(fragment, source[int(number) - 1],
                                  "citation moved or no longer supports its source fragment")


if __name__ == "__main__":
    unittest.main()
