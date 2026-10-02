"""Source-only tests of the fixed Farm32 endpoint staging boundary."""

import hashlib
import json
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest
import uuid
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
        self.RunId = str(uuid.uuid4())
        self.Runner = self.Root / "farm_campaign_runner.py"
        self.Runner.write_text("print('fixed mock')\n", encoding="utf-8")
        self.Ticket = Save(self.Root / "ticket.json", {
            "Format": "GargantuanFarm32Campaign", "Version": 1, "Role": "CLIENT",
            "RunId": self.RunId,
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

    def RunInThread(self):
        Errors = []
        def Target():
            try:
                Endpoint.Run(self.Root, self.Index, self.Launch, "role")
            except (ValueError, RuntimeError, TimeoutError, OSError) as Error:
                Errors.append(Error)
        Thread = threading.Thread(target=Target)
        Thread.start()
        return Thread, Errors

    def WaitForLog(self):
        Deadline = time.monotonic() + 5
        while not (self.Root / "role.stdout.log").is_file():
            if time.monotonic() >= Deadline:
                self.fail("bounded mock child did not start")
            time.sleep(0.01)

    def SetSleepingRunner(self):
        self.Runner.write_text("import time\nprint('started', flush=True)\ntime.sleep(30)\n",
                               encoding="utf-8")
        Index = json.loads(self.Index.read_text())
        Index["Files"][0]["Sha256"] = Hash(self.Runner)
        Save(self.Index, Index)
        Launch = json.loads(self.Launch.read_text())
        Launch["RunnerSha256"] = Hash(self.Runner)
        Save(self.Launch, Launch)

    def test_run_bound_abort_reaps_owned_child(self):
        self.SetSleepingRunner()
        Thread, Errors = self.RunInThread()
        self.WaitForLog()
        Endpoint.Abort(self.Root, self.Index, "role")
        Thread.join(timeout=12)
        self.assertFalse(Thread.is_alive())
        self.assertEqual(1, len(Errors))
        self.assertIn("abort requested", str(Errors[0]))
        Terminal = json.loads((self.Root / "role.terminal.json").read_text())
        self.assertEqual(self.RunId, Terminal["RunId"])
        self.assertEqual("ABORTED", Terminal["Outcome"])
        self.assertTrue(Terminal["ChildTreeReaped"])

    def test_run_timeout_reaps_owned_child(self):
        self.SetSleepingRunner()
        with mock.patch.object(Endpoint, "MAX_SECONDS", 0.05):
            Thread, Errors = self.RunInThread()
            Thread.join(timeout=12)
        self.assertFalse(Thread.is_alive())
        self.assertEqual(1, len(Errors))
        self.assertIn("time or log bound", str(Errors[0]))
        Terminal = json.loads((self.Root / "role.terminal.json").read_text())
        self.assertEqual("BOUND_EXCEEDED", Terminal["Outcome"])
        self.assertTrue(Terminal["ChildTreeReaped"])

    def test_failed_runner_without_live_tree_proof_fails_closed(self):
        self.Runner.write_text("raise RuntimeError('mock failure')\n", encoding="utf-8")
        Index = json.loads(self.Index.read_text())
        Index["Files"][0]["Sha256"] = Hash(self.Runner)
        Save(self.Index, Index)
        Launch = json.loads(self.Launch.read_text())
        Launch["RunnerSha256"] = Hash(self.Runner)
        Save(self.Launch, Launch)
        with self.assertRaisesRegex(RuntimeError, "child tree was not reaped"):
            Endpoint.Run(self.Root, self.Index, self.Launch, "role")
        Terminal = json.loads((self.Root / "role.terminal.json").read_text())
        self.assertEqual("FAILED", Terminal["Outcome"])
        self.assertFalse(Terminal["ChildTreeReaped"])


if __name__ == "__main__":
    unittest.main()
