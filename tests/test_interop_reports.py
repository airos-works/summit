import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


TOOLS = Path(__file__).resolve().parents[1] / "tools" / "interop2026"


class InteropReportTests(unittest.TestCase):
    def test_overlapping_focus_areas_and_years(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            results = root / "results"
            results.mkdir()
            labels = {
                "2025/interop-2025-a": ["/shared", "/unrun"],
                "2025/interop-2025-b": ["/shared"],
                "2026/interop-2026-c": ["/shared", "/passed"],
            }
            (root / "labels.json").write_text(json.dumps(labels))
            safari = {url: {"s": "P", "c": [0, 0]} for url in ["/shared", "/unrun", "/passed"]}
            for name in ["safari-summary", "stp-summary"]:
                (root / f"{name}.json").write_text(json.dumps(safari))
            report = results / "sample.json"
            report.write_text(json.dumps({"results": [
                {"test": "/shared", "status": "FAIL"},
                {"test": "/passed", "status": "PASS"},
            ]}))
            env = dict(os.environ, INTEROP_ROOT=directory)
            score = subprocess.check_output([sys.executable, TOOLS / "score.py", report], env=env, text=True)
            tests = json.loads((results / "tests.json").read_text())
            self.assertEqual(tests["/shared"]["areas"], list(labels))
            self.assertEqual(tests["/shared"]["area"], "2026/interop-2026-c")
            self.assertEqual(tests["/unrun"]["status"], "NOT RUN")
            self.assertRegex(score, r"2025 TOTAL \(mean of areas\)\s+0\.0\s+1000\.0\s+1000\.0")
            self.assertRegex(score, r"2026 TOTAL \(mean of areas\)\s+500\.0\s+1000\.0\s+1000\.0")

            gaps = subprocess.check_output([sys.executable, TOOLS / "gaps.py", "sample"], env=env, text=True)
            first, second = gaps.split("2026 best-estimate overall")
            self.assertIn("2025 best-estimate overall 0.0", first)
            self.assertIn("+100.00 points  (2 tests)", first)
            self.assertTrue(second.startswith(" 50.0"))
            self.assertIn("+50.00 points  (1 tests)", second)
            issues = json.loads((results / "issue-tests.json").read_text())
            self.assertCountEqual(issues["other"], [["/shared", 0], ["/unrun", 0]])
