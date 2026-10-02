"""Source-only tests of the fixed Farm32 endpoint staging boundary."""

import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import farm_outer_endpoint as Endpoint  # noqa: E402


def Save(File, Value):
    File.write_text(json.dumps(Value), encoding="utf-8")
    return File


def Hash(File):
    return hashlib.sha256(File.read_bytes()).hexdigest()


class FarmOuterEndpointTests(unittest.TestCase):
    def setUp(self):
        self.Temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.Temporary.cleanup)
        self.Root = Path(self.Temporary.name) / "stage"
        Endpoint.Prepare(self.Root)
        self.Runner = self.Root / "farm_campaign_runner.py"
        self.Runner.write_text("print('fixed mock')\n", encoding="utf-8")
        self.Ticket = Save(self.Root / "ticket.json", {
            "Format": "GargantuanFarm32Campaign", "Version": 1, "Role": "CLIENT",
        })
        self.Index = Save(self.Root / "index.json", {"Format": "GargantuanFarm32StageIndex",
            "Version": 1, "Files": [{"Name": File.name, "Sha256": Hash(File)}
                                  for File in (self.Runner, self.Ticket)]})
        self.Launch = Save(self.Root / "launch.json", {
            "Format": "GargantuanFarm32FixedLaunch", "Version": 1, "Action": "role",
            "RunnerName": self.Runner.name, "RunnerSha256": Hash(self.Runner),
            "InputName": self.Ticket.name, "InputSha256": Hash(self.Ticket),
        })

    def test_private_index_hash_and_fixed_runner(self):
        Endpoint.Verify(self.Root, self.Index)
        with mock.patch.object(Endpoint, "MAX_SECONDS", 2):
            Endpoint.Run(self.Root, self.Index, self.Launch, "role")
        self.assertTrue((self.Root / "role.stdout.log").is_file())
        self.assertRaisesRegex(ValueError, "runner is not fixed", self.InvalidRunner)

    def InvalidRunner(self):
        Alternate = self.Root / "other.py"
        Alternate.write_text("print('other')\n", encoding="utf-8")
        Index = json.loads(self.Index.read_text())
        Index["Files"].append({"Name": Alternate.name, "Sha256": Hash(Alternate)})
        Save(self.Index, Index)
        Row = json.loads(self.Launch.read_text())
        Row["RunnerName"] = Alternate.name
        Row["RunnerSha256"] = Hash(Alternate)
        Save(self.Launch, Row)
        Endpoint.Run(self.Root, self.Index, self.Launch, "role")

    def test_mismatch_and_unlisted_path_fail_closed(self):
        self.Ticket.write_text("changed", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "stage member hash mismatch"):
            Endpoint.Verify(self.Root, self.Index)
        Row = json.loads(self.Index.read_text())
        Row["Files"][1]["Name"] = "../ticket.json"
        Save(self.Index, Row)
        with self.assertRaisesRegex(ValueError, "invalid stage member"):
            Endpoint.Verify(self.Root, self.Index)

    def test_fails_inherited_acl_and_reused_stage_root(self):
        with self.assertRaisesRegex(ValueError, "already exists"):
            Endpoint.Prepare(self.Root)
        if Endpoint.os.name != "nt":
            self.Root.chmod(0o755)
            with self.assertRaises(ValueError):
                Endpoint.Verify(self.Root, self.Index)


if __name__ == "__main__":
    unittest.main()
