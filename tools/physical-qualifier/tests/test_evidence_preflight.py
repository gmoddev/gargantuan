"""Pre-wake evidence validation must reject used or mismatched runs."""

import json
from pathlib import Path
import sys
import tempfile
import unittest


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from evidence_preflight import ValidateRun


class EvidencePreflightTests(unittest.TestCase):
    def setUp(self):
        self.Temporary = tempfile.TemporaryDirectory()
        self.Root = Path(self.Temporary.name) / "physical-qualifier"
        self.Root.mkdir()
        self.Run = self.Root / "lifecycle-1234567890abcdef"
        self.Run.mkdir()
        self.RunId = "new-physical-run-id"
        for Name, Evidence in (("coordinator.json", "coordinator-evidence"),
                               ("client.json", "client-evidence")):
            (self.Run / Name).write_text(json.dumps({
                "RunId": self.RunId, "EvidenceDir": str(self.Run / Evidence)}))

    def tearDown(self):
        self.Temporary.cleanup()

    def test_fresh_staged_run_passes_without_creating_evidence(self):
        ValidateRun(self.Run, self.RunId, self.Root)
        self.assertFalse((self.Run / "coordinator-evidence").exists())
        self.assertFalse((self.Run / "client-evidence").exists())

    def test_stale_or_used_run_rejected(self):
        (self.Run / "preflight.json").write_text("{}")
        with self.assertRaisesRegex(ValueError, "already consumed"):
            ValidateRun(self.Run, self.RunId, self.Root)
        (self.Run / "preflight.json").unlink()
        (self.Run / "client-evidence").mkdir()
        with self.assertRaisesRegex(ValueError, "already used"):
            ValidateRun(self.Run, self.RunId, self.Root)

    def test_arbitrary_root_and_mismatched_config_rejected(self):
        with self.assertRaisesRegex(ValueError, "unapproved"):
            ValidateRun(self.Run, self.RunId, self.Root / "other")
        with self.assertRaisesRegex(ValueError, "does not match"):
            ValidateRun(self.Run, "wrong-id", self.Root)

    def test_unrelated_evidence_preserved(self):
        Other = self.Root / "lifecycle-fedcba0987654321"
        Other.mkdir()
        Marker = Other / "keep.txt"
        Marker.write_text("untouched")
        ValidateRun(self.Run, self.RunId, self.Root)
        self.assertEqual(Marker.read_text(), "untouched")


if __name__ == "__main__":
    unittest.main()
