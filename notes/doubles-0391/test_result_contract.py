"""Check the ranked doubles handoff and its live engine source anchors."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
DOC = Path(__file__).with_name('result-contract.md')


class ResultContract(unittest.TestCase):
    def test_wire_contract_and_source_anchors(self):
        self.assertTrue(DOC.is_file(), 'missing ranked doubles result contract')
        text = DOC.read_text()
        for word in ('rdoubles', 'qdoubles', '2v2-ranked', 'game-report', '$final',
                     'detail.tw', 'premade', 'desync', 'disconnect', 'per-seat',
                     'per-team', 'host', 'record_team_match_report'):
            self.assertIn(word, text.lower())
        anchors = re.findall(r'`(Source/[^`:\s]+):(\d+)` — `([^`]+)`', text)
        self.assertGreaterEqual(len(anchors), 12)
        for path, line, fragment in anchors:
            with self.subTest(path=path, line=line):
                self.assertIn(fragment, (ROOT / path).read_text().splitlines()[int(line) - 1])


if __name__ == '__main__':
    unittest.main()
