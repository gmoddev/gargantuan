"""Source-only Farm32 capture lifetime tests; no real capture is started."""

from datetime import datetime, timedelta, timezone
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import uuid


MODULE_PATH = Path(__file__).resolve().parents[1] / "farm_capture_campaign.py"
SPEC = importlib.util.spec_from_file_location("farm_capture_campaign", MODULE_PATH)
campaign = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(campaign)


def WriteJson(File, Data):
    File.write_text(json.dumps(Data), encoding="utf-8")


class FixedClock:
    def __init__(self):
        self.Value = 0

    def __call__(self):
        return self.Value

    def Sleep(self, Seconds):
        self.Value += Seconds


class CaptureCampaignTests(unittest.TestCase):
    def setUp(self):
        self.Temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.Temp.cleanup)
        self.Root = Path(self.Temp.name)
        self.CaptureRoot = self.Root / "capture"
        self.CaptureRoot.mkdir()
        self.RoleRoot = self.Root / "role"
        self.RoleRoot.mkdir()
        self.RunId = str(uuid.uuid4())
        self.CoordinatorRunId = str(uuid.uuid4())
        self.Capture = self.CaptureRoot / self.RunId
        self.Clock = FixedClock()

    def Config(self, Role):
        return {"RunId": self.RunId, "CoordinatorRunId": self.CoordinatorRunId,
                "Role": Role, "CaptureRoot": self.CaptureRoot,
                "CaptureDirectory": self.Capture, "RoleEvidenceRoot": self.RoleRoot,
                "PowerShellPath": self.Root / "pwsh.exe",
                "ServicePath": self.Root / "AgentCoordinator.CaptureFarm32Service.exe",
                "HookPath": self.Root / "CaptureFarm32.ps1",
                "CaptureScriptPath": self.Root / "DumpcapFarm32Capture.ps1",
                "DumpcapPath": self.Root / "dumpcap.exe", "DumpcapSha256": "a" * 64}

    def RoleEvidence(self, Role):
        Result = {"RunId": self.RunId, "Role": "Server" if Role == "SERVER" else "Clients",
                  "Status": "PASS"}
        Now = datetime.now(timezone.utc)
        Result["StartedUtc"] = Now.isoformat()
        Result["CompletedUtc"] = Now.isoformat()
        ResultFile = self.RoleRoot / "result.json"
        WriteJson(ResultFile, Result)
        Index = {"RunId": self.RunId, "Role": Result["Role"],
                 "Files": [{"Name": "result.json", "Bytes": ResultFile.stat().st_size,
                            "Sha256": campaign.Digest(ResultFile)}]}
        WriteJson(self.RoleRoot / "evidence-sha256.json", Index)
        return self.RoleRoot / "evidence-sha256.json"

    def test_worker_stop_precedes_offline_finalize_and_seals_separate_root(self):
        Config = self.Config("SERVER")
        Calls = []

        def Runner(Command, **_):
            Calls.append([str(Item) for Item in Command])
            if Command[0] == str(Config["ServicePath"]):
                Operation = Command[1]
                if Operation == "start":
                    (self.Capture / "farm32-capture-active.txt").write_text("active")
                return subprocess.CompletedProcess(Command, 0, json.dumps({
                    "Success": True, "Operation": Operation, "RunId": self.RunId,
                    "State": {"start": "running", "stop": "stopped", "status": "idle"}[Operation]}), "")
            self.assertEqual(Command[-1], "Finalize")
            (self.Capture / "farm32-worker-capture.etl").write_bytes(b"etl")
            (self.Capture / "farm32-worker-capture.pcapng").write_bytes(b"\x0a\x0d\x0d\x0a" + b"pcap")
            (self.Capture / "farm32-capture-summary.txt").write_text("Total Events Lost 0\n")
            return subprocess.CompletedProcess(Command, 0, "exported", "")

        Controller = campaign.FarmCaptureController(Config, Runner=Runner,
                                                     Clock=self.Clock, Sleep=self.Clock.Sleep)
        Ready = Controller.Start()
        Index = self.RoleEvidence("SERVER")
        self.assertTrue(Ready.is_file())
        Sealed = Controller.Finish()
        Manifest = campaign.ReadJson(Sealed)
        self.assertEqual(Manifest["State"], "SEALED_UNQUALIFIED")
        self.assertEqual(Manifest["RoleIndexSha256"], campaign.Digest(Index))
        self.assertEqual([Row[1] for Row in Calls[:3]], ["start", "stop", "status"])
        self.assertEqual(Calls[3][-1], "Finalize")

    def test_client_capture_is_owned_until_autostop(self):
        Config = self.Config("CLIENT")

        class Child:
            def poll(self):
                return None

            def wait(self, timeout):
                self.Test.assertLessEqual(timeout, campaign.CLIENT_AUTOSTOP_SECONDS)
                return 0

        def Spawner(Command, **_):
            self.assertEqual(Command[0], str(Config["PowerShellPath"]))
            (self.Capture / "farm32-client-capture.pcapng").write_bytes(b"\x0a\x0d\x0d\x0a" + b"x" * 24)
            Child.Test = self
            return Child()

        Controller = campaign.FarmCaptureController(Config, Spawner=Spawner,
                                                     Clock=self.Clock, Sleep=self.Clock.Sleep)
        Controller.Start()
        self.RoleEvidence("CLIENT")
        Manifest = campaign.ReadJson(Controller.Finish())
        self.assertEqual(Manifest["Role"], "CLIENT")
        self.assertEqual(Manifest["State"], "SEALED_UNQUALIFIED")

    def test_client_capture_clean_early_exit_cannot_be_timestamped_after_role(self):
        Config = self.Config("CLIENT")
        self.Capture.mkdir()
        Controller = campaign.FarmCaptureController(Config, Clock=self.Clock, Sleep=self.Clock.Sleep)
        Controller.Started = self.Clock()

        class Child:
            def poll(self):
                return 0

        Controller.Child = Child()
        with self.assertRaisesRegex(RuntimeError, "ended before role completion"):
            Controller.AwaitRole()
        self.assertLess(self.Clock.Value, 1)
        self.RoleEvidence("CLIENT")
        with self.assertRaisesRegex(RuntimeError, "ended before role completion"):
            Controller.Finish()
        self.assertFalse((self.Capture / "capture-sha256.json").exists())

    def test_role_deadline_is_less_than_service_lease(self):
        Config = self.Config("SERVER")
        self.Capture.mkdir()
        Controller = campaign.FarmCaptureController(Config, Clock=self.Clock, Sleep=self.Clock.Sleep)
        Controller.Started = self.Clock()
        with self.assertRaises(TimeoutError):
            Controller.AwaitRole()
        self.assertAlmostEqual(self.Clock.Value, campaign.ROLE_DEADLINE_SECONDS, places=6)
        self.assertLess(campaign.ROLE_DEADLINE_SECONDS + campaign.STOP_TIMEOUT_SECONDS, 600)

    def test_outer_receipt_binds_both_role_and_capture_indices(self):
        Coordinator = self.Root / "coordinator.json"
        WriteJson(Coordinator, {"RunId": self.CoordinatorRunId, "Success": True})
        ServerRole = self.Root / "server-role.json"
        ClientRole = self.Root / "client-role.json"
        ServerCapture = self.Root / "server-capture.json"
        ClientCapture = self.Root / "client-capture.json"
        for Role, RoleFile, CaptureFile in (("SERVER", ServerRole, ServerCapture),
                                            ("CLIENT", ClientRole, ClientCapture)):
            WriteJson(RoleFile, {"RunId": self.RunId, "Role": "Server" if Role == "SERVER" else "Clients"})
            WriteJson(CaptureFile, {"RunId": self.RunId, "CoordinatorRunId": self.CoordinatorRunId,
                                    "Role": Role, "RoleIndexSha256": campaign.Digest(RoleFile),
                                    "State": "SEALED_UNQUALIFIED"})
        Receipt = self.Root / "outer.json"
        campaign.BindReceipt(self.RunId, self.CoordinatorRunId, Coordinator,
                             ServerRole, ClientRole, ServerCapture, ClientCapture, Receipt)
        Bound = campaign.ReadJson(Receipt)
        self.assertEqual(Bound["CoordinatorResultSha256"], campaign.Digest(Coordinator))
        self.assertEqual(Bound["Roles"]["SERVER"]["CaptureIndexSha256"], campaign.Digest(ServerCapture))
        WriteJson(ClientCapture, {"RunId": self.RunId, "State": "PASS"})
        with self.assertRaises(ValueError):
            campaign.BindReceipt(self.RunId, self.CoordinatorRunId, Coordinator,
                                 ServerRole, ClientRole, ServerCapture, ClientCapture,
                                 self.Root / "untrusted.json")

    def test_fixed_config_rejects_changed_pin_and_overlapping_roots(self):
        Config = {"Format": "GargantuanFarm32CaptureCampaign", "Version": 1,
                  "RunId": self.RunId, "CoordinatorRunId": self.CoordinatorRunId,
                  "Role": "SERVER", "CaptureRoot": str(self.CaptureRoot),
                  "RoleEvidenceRoot": str(self.RoleRoot)}
        for Key, Name in (("PowerShellPath", "pwsh.exe"),
                          ("ServicePath", "AgentCoordinator.CaptureFarm32Service.exe"),
                          ("HookPath", "CaptureFarm32.ps1")):
            File = self.Root / Name
            File.write_bytes(Name.encode("ascii"))
            Config[Key] = str(File)
            Config[Key.removesuffix("Path") + "Sha256"] = campaign.Digest(File)
        ConfigFile = self.Root / "config.json"
        WriteJson(ConfigFile, Config)
        self.assertEqual(campaign.Configured(ConfigFile)["CaptureDirectory"],
                         self.CaptureRoot.resolve(strict=True) / self.RunId)
        (self.Root / "CaptureFarm32.ps1").write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "pin changed"):
            campaign.Configured(ConfigFile)
        Config["HookSha256"] = campaign.Digest(self.Root / "CaptureFarm32.ps1")
        Config["RoleEvidenceRoot"] = str(self.CaptureRoot / "role")
        WriteJson(ConfigFile, Config)
        with self.assertRaisesRegex(ValueError, "overlap"):
            campaign.Configured(ConfigFile)


if __name__ == "__main__":
    unittest.main()
