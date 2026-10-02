"""Mock transfer tests for fixed Farm32 two-host staging."""

import hashlib
import json
from pathlib import Path
import shutil
import sys
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import farm_outer_campaign as Outer  # noqa: E402
import farm_outer_endpoint as Endpoint  # noqa: E402
import farm_campaign_runner as Campaign  # noqa: E402
from test_farm_ticket_staging import FarmTicketStagingTests  # noqa: E402
REAL_WORKER_SANDBOX = Outer.WorkerSandbox


class MockTransport:
    def WorkerDigest(self, File):
        if File.endswith("python.exe"):
            return "a" * 64
        if File.endswith("farm_outer_endpoint.py"):
            return Outer.Digest(Outer.SOURCE / "farm_outer_endpoint.py")
        raise AssertionError(File)

    def Remote(self, Action, *Arguments):
        if Action == "prepare":
            Endpoint.Prepare(Arguments[0])
        elif Action == "verify":
            Endpoint.Verify(*Arguments)
        else:
            raise AssertionError(Action)

    def RemoteCopy(self, Source, Destination):
        shutil.copyfile(Source, Destination)

    def RemoteTree(self, Source, Destination):
        shutil.copytree(Source, Destination)

    def Fetch(self, Source, Destination, Timeout=10):
        shutil.copyfile(Source, Destination)


class OuterCampaignTests(unittest.TestCase):
    def setUp(self):
        self.Fixture = FarmTicketStagingTests(methodName="test_fresh_secrets_fixed_copy_plan_and_runner_schema")
        self.Fixture.setUp()
        self.addCleanup(self.Fixture.tearDown)
        Sandbox = mock.patch.object(Outer, "WorkerSandbox", side_effect=Outer.RemoteText)
        Sandbox.start()
        self.addCleanup(Sandbox.stop)
        self.Fixture.Spec["Host"]["HostIp"] = Outer.WORKER_IP
        self.Fixture.Spec["Host"]["Port"] = Outer.CONTROL_PORT
        self.Fixture.Seal()
        Outer.WriteNew(self.Fixture.Private / "runtime-pins.json", {
            "Format": "GargantuanFarm32RuntimePins", "Version": 1,
            "WorkerPythonPath": r"C:\Python312\python.exe", "WorkerPythonSha256": "a" * 64,
            "WorkerHelperPath": r"C:\Sandbox\farm_outer_endpoint.py",
            "WorkerHelperSha256": Outer.Digest(Outer.SOURCE / "farm_outer_endpoint.py"),
            "ClientPythonPath": sys.executable,
            "ClientPythonSha256": Outer.Digest(sys.executable),
        })
        for Role in ("SERVER", "CLIENT"):
            Path(self.Fixture.Spec["Roles"][Role]["StageRoot"]).rmdir()

    def test_stage_rebases_host_tickets_and_checks_each_destination(self):
        Roots = Outer.Stage(self.Fixture.Private, self.Fixture.SpecFile,
                            r"C:\Python312\python.exe", r"C:\Sandbox\farm_outer_endpoint.py",
                            MockTransport())
        for Role in Roots:
            Root = Path(Roots[Role])
            Endpoint.Verify(Root, Root / "stage-index.json")
            self.assertTrue((Root / ".agent-coordinator" / "agent_coordinator" / "control.py").is_file())
            Campaign.VerifyRole(json.loads((Root / "ticket.json").read_text()), Role)
        Host = json.loads((Path(Roots["SERVER"]) / "host-config.json").read_text())
        self.assertEqual(str(Path(Roots["SERVER"]) / "host-client-ticket.json"),
                         Host["Roles"]["CLIENT"]["TicketPath"])
        self.assertEqual(Outer.WORKER_IP, Host["HostIp"])
        for Role in ("SERVER", "CLIENT"):
            File = (Path(Roots["SERVER"]) / ("ticket.json" if Role == "SERVER" else
                                                  "host-client-ticket.json"))
            Campaign.VerifyHostTicket(json.loads(File.read_text()), Role, Host)
        Upstream = Path(Roots["SERVER"]) / ".agent-coordinator" / "agent_coordinator" / "control.py"
        Upstream.write_bytes(Upstream.read_bytes() + b"\n# tampered mock\n")
        with self.assertRaises(ValueError):
            Endpoint.Verify(Roots["SERVER"], Path(Roots["SERVER"]) / "stage-index.json")
        with self.assertRaises(ValueError):
            Outer.Stage(self.Fixture.Private, self.Fixture.SpecFile,
                        r"C:\Python312\python.exe", r"C:\Sandbox\farm_outer_endpoint.py",
                        MockTransport())

    def test_rejects_non_worker_host_or_modified_source(self):
        self.Fixture.Spec["Host"]["HostIp"] = "192.168.0.68"
        self.Fixture.SpecFile.write_text(json.dumps(self.Fixture.Spec), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "worker-host profile mismatch"):
            Outer.Stage(self.Fixture.Private, self.Fixture.SpecFile,
                        r"C:\Python312\python.exe", r"C:\Sandbox\farm_outer_endpoint.py",
                        MockTransport())
        self.Fixture.Spec["Host"]["HostIp"] = Outer.WORKER_IP
        self.Fixture.SpecFile.write_text(json.dumps(self.Fixture.Spec), encoding="utf-8")
        Plan = json.loads((self.Fixture.Private / "copy-plan.json").read_text())
        Plan["Files"][0]["Sha256"] = hashlib.sha256(b"wrong").hexdigest()
        (self.Fixture.Private / "copy-plan.json").write_text(json.dumps(Plan), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "fixed copy plan member changed"):
            Outer.Stage(self.Fixture.Private, self.Fixture.SpecFile,
                        r"C:\Python312\python.exe", r"C:\Sandbox\farm_outer_endpoint.py",
                        MockTransport())

    def test_preflight_ttl_is_rechecked_at_launch_boundary(self):
        for Role in ("SERVER", "CLIENT"):
            Source = Path(self.Fixture.Spec["Roles"][Role]["PreflightSource"])
            Target = self.Fixture.Private / ("server-preflight.json" if Role == "SERVER" else
                                              "client-preflight.json")
            shutil.copyfile(Source, Target)
        Outer.RequireFreshPreflights(self.Fixture.Private, self.Fixture.Identity)
        Target = self.Fixture.Private / "client-preflight.json"
        Row = json.loads(Target.read_text())
        Row["ObservedUtc"] = "2020-01-01T00:00:00Z"
        Target.write_text(json.dumps(Row), encoding="utf-8")
        with self.assertRaises(ValueError):
            Outer.RequireFreshPreflights(self.Fixture.Private, self.Fixture.Identity)

    def test_collected_worker_index_verifies_every_member(self):
        Remote = self.Fixture.Root / "mock-worker-evidence"
        Remote.mkdir()
        Member = Remote / "result.json"
        Member.write_text("bounded evidence", encoding="utf-8")
        Index = {"RunId": self.Fixture.Identity["RunId"], "Role": "Server",
                 "Files": [{"Name": Member.name, "Bytes": Member.stat().st_size,
                            "Sha256": Outer.Digest(Member)}]}
        (Remote / "evidence-sha256.json").write_text(json.dumps(Index), encoding="utf-8")
        Output = self.Fixture.Root / "copied"
        Outer.FetchIndexed(MockTransport(), str(Remote), Output,
                           "evidence-sha256.json", self.Fixture.Identity["RunId"],
                           "SERVER", 128, 16 * 1024 * 1024)
        self.assertEqual(Member.read_bytes(), (Output / Member.name).read_bytes())
        Index["Files"][0]["Sha256"] = "0" * 64
        (Remote / "evidence-sha256.json").write_text(json.dumps(Index), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "transfer.*hash mismatch"):
            Outer.FetchIndexed(MockTransport(), str(Remote), self.Fixture.Root / "changed",
                               "evidence-sha256.json", self.Fixture.Identity["RunId"],
                               "SERVER", 128, 16 * 1024 * 1024)

    def test_real_worker_paths_remain_under_task_sandbox(self):
        self.assertRaisesRegex(ValueError, "outside task sandbox", REAL_WORKER_SANDBOX,
                               r"C:\Windows\System32\something.exe")


if __name__ == "__main__":
    unittest.main()
