#!/usr/bin/env python3
"""Read retained local evidence; this script never launches an engine."""
import argparse
import json
from pathlib import Path
import re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("case", choices=("pair12", "pair13", "no-teams"))
args = parser.parse_args()
directory = Path(__file__).resolve().parent / args.case
expected = [0, 0, 1, 1] if args.case == "pair12" else [0, 1, 0, 1]
teamless = args.case == "no-teams"
if teamless:
    expected = [int.from_bytes((directory / f"peer-0-probe-scSelctCharacter-1-1460-area{port}.bin")
                              .read_bytes()[0x1C0:0x1C4], "big") for port in range(4)]
    assert expected[0] == expected[2] and expected[1] == expected[3] and expected[0] != expected[1]
    assert all(0 <= colour <= 2 for colour in expected), expected
summary = []
for peer in range(4):
    log = (directory / f"peer-{peer}.log").read_text()
    pins = re.findall(r"Orca: doubles teams pinned from the room: ports ([^\n]+)", log)
    assert bool(pins) != teamless, (peer, "pin lines", pins)
    assert "desync detected" not in log.lower(), peer
    starts = list(map(int, re.findall(r"a game began \(the fight from frame (\d+)\)", log)))
    ends = re.findall(r"a game ended at frame (\d+): ([^\n]+)", log)
    assert len(starts) >= 2 and len(ends) >= 2, (peer, starts, ends)
    if teamless:
        # Existing report policy rejects an unassigned doubles map. The native colours and
        # result records still follow the host; this task changes only that report's reason log.
        assert all(result == "void (teams)" for _, result in ends), (peer, ends)
    else:
        assert all("port " in result and " won," in result for _, result in ends), (peer, ends)
    prefix = f"peer-{peer}-probe-"
    pairing = (directory / f"{prefix}scSelctCharacter-1-1400-pairing.bin").read_bytes()
    task_before = (directory / f"{prefix}scSelctCharacter-1-1400-task.bin").read_bytes()
    task_after = (directory / f"{prefix}scSelctCharacter-1-1460-task.bin").read_bytes()
    assert task_before[0x5C8] == 0 and task_after[0x5C8] == 1, (peer, "native toggle")
    if teamless:
        assert pairing[:4] != b"YGT1", (peer, "unexpected pairing initialization")
        for path in directory.glob(f"{prefix}*-pairing.bin"):
            assert path.read_bytes() == pairing, (peer, "pairing storage changed", path.name)
    else:
        assert pairing[:4] == b"YGT1" and pairing[4] == sum(
            team << port for port, team in enumerate(expected)), (peer, "synced pairing")
    before = []
    after = []
    for port in range(4):
        a = (directory / f"{prefix}scSelctCharacter-1-1400-area{port}.bin").read_bytes()
        b = (directory / f"{prefix}scSelctCharacter-1-1460-area{port}.bin").read_bytes()
        before.append(int.from_bytes(a[0x1C0:0x1C4], "big"))
        after.append(int.from_bytes(b[0x1C0:0x1C4], "big"))
    assert before == [0, 0, 0, 0], (peer, "colours while off", before)
    assert after == expected, (peer, "CSS colours", after)
    for visit in (1, 2):
        select = (directory / f"{prefix}scSelStage-{visit}-100-select.bin").read_bytes()
        assert select[0x33] == 1, (peer, "Team battle mode", select[0x33])
        colours = [select[0xB8 + port * 0x5C + 0x0B] for port in range(4)]
        assert colours == expected, (peer, "SSS colours", colours)
        result = (directory / f"{prefix}scVsResult-{visit}-1-result.bin").read_bytes()
        colours = [result[0x24 + port * 0x2AC + 6] for port in range(4)]
        assert colours == expected, (peer, "result colours", colours)
        stocks = [result[0x24 + port * 0x2AC + 0x0A] for port in range(4)]
        expected_stocks = [4, 0, 4, 0] if teamless else [4 if team == 0 else 0 for team in expected]
        assert stocks == expected_stocks, (peer, "stocks", stocks)
    summary.append(dict(peer=peer, pins=pins, starts=starts, ends=ends,
                        css_before=before, css_after=after, result_teams=expected))
assert all(item["starts"] == summary[0]["starts"] and item["ends"] == summary[0]["ends"]
           for item in summary), "engines disagree on start/end frames or reports"
wrapper = (directory / "wrapper.log").read_text()
assert "PASS: all machines agree" in wrapper, "fixture checksum verification did not pass"
print(json.dumps(summary, indent=2))
print(f"PASS {args.case}: four engines, identical frames/reports, native colours {expected}; "
      f"{'no pins' if teamless else 'pin on every engine'}")
