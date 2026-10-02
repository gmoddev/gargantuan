"""Deterministic fixed-file ticket staging tests; no host or capture is started."""

from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import farm_campaign_runner as Campaign  # noqa: E402
import farm_ticket_staging as Staging  # noqa: E402


def Save(File, Row):
    File.parent.mkdir(parents=True, exist_ok=True)
    File.write_text(json.dumps(Row), encoding="utf-8")
    return File


def Hash(File):
    return hashlib.sha256(File.read_bytes()).hexdigest()


def Pin(File):
    File.parent.mkdir(parents=True, exist_ok=True)
    File.write_bytes(b"fixed mock file " + File.name.encode("ascii"))
    return {"Path": str(File), "Sha256": Hash(File)}


class FarmTicketStagingTests(unittest.TestCase):
    def setUp(self):
        self.Temporary = tempfile.TemporaryDirectory()
        self.Root = Path(self.Temporary.name)
        self.Private = self.Root / "private"
        Staging.New(self.Private)
        self.Identity = json.loads((self.Private / "identity.json").read_text())
        self.Manifest = Save(self.Root / "source" / "run-manifest.json", {
            "Format": "GargantuanPhysicalFarmEndpoint", "Version": 1,
            "RunId": self.Identity["RunId"], "SourceCommit": "a" * 40,
            "Provider": "Local", "Nonces": [str(123456789 << 32 | (Slot + 1)) for Slot in range(32)],
        })
        self.Spec = {"Format": "GargantuanFarm32TicketSpec", "Version": 1,
                     "ManifestSource": str(self.Manifest),
                     "Host": {"HostIp": "127.0.0.1", "Port": 39451,
                              "JournalRoot": str(self.Root / "host-journal"),
                              "ListeningPath": str(self.Root / "host-listening.json")},
                     "Roles": {Role: self.Role(Role) for Role in ("SERVER", "CLIENT")}}
        self.SpecFile = Save(self.Root / "spec.json", self.Spec)

    def tearDown(self):
        self.Temporary.cleanup()

    def Role(self, Role):
        Base = self.Root / Role.lower()
        Stage = Base / "stage"
        Stage.mkdir(parents=True)
        (Base / "capture").mkdir()
        (Base / "evidence").mkdir()
        (Base / "package").mkdir()
        (Base / "registry").mkdir()
        Preflight = Save(Base / "source-preflight.json", {
            "Format": "GargantuanPhysicalFarmPreflight", "Version": 1,
            "RunId": self.Identity["RunId"], "Role": "Server" if Role == "SERVER" else "Clients",
            "Provider": "Local", "ManifestSha256": Hash(self.Manifest),
            "ObservedUtc": datetime.now(timezone.utc).isoformat(), "Status": "INVENTORY_ONLY",
        })
        Binaries = Base / "installed"
        Engine = ({"Service": Pin(Binaries / "AgentCoordinator.CaptureFarm32Service.exe"),
                   "Hook": Pin(Binaries / "CaptureFarm32.ps1")} if Role == "SERVER" else
                  {"CaptureScript": Pin(Binaries / "DumpcapFarm32Capture.ps1"),
                   "Dumpcap": Pin(Binaries / "dumpcap.exe")})
        return {"EndpointId": Role, "PeerIp": "127.0.0.1", "StageRoot": str(Stage),
                "ManifestSource": str(self.Manifest), "PreflightSource": str(Preflight),
                "ManifestPath": str(Stage / "run-manifest.json"),
                "PreflightPath": str(Stage / "preflight.json"),
                "FarmConfigPath": str(Stage / "farm-config.json"),
                "CaptureConfigPath": str(Stage / "capture-config.json"),
                "TicketPath": str(Stage / "ticket.json"),
                "WorkflowPath": str(Stage / "thirty-two-client-farm-lifecycle.json"),
                "CaptureControllerPath": str(Stage / "farm_capture_campaign.py"),
                "JournalRoot": str(Base / "journal"), "ResultPath": str(Base / "result.json"),
                "PackageRoot": str(Base / "package"),
                "EvidenceRoot": str(Base / "evidence" / self.Identity["RunId"]),
                "RunRegistryRoot": str(Base / "registry"), "CaptureRoot": str(Base / "capture"),
                "PowerShell": Pin(Binaries / "pwsh.exe"),
                "Supervisor": Pin(Binaries / "PhysicalGameSessionFarmEndpoint.ps1"),
                "CaptureEngine": Engine}

    def Seal(self):
        Save(self.SpecFile, self.Spec)
        Staging.Seal(self.Private, self.SpecFile)
        return json.loads((self.Private / "copy-plan.json").read_text())

    def test_fresh_secrets_fixed_copy_plan_and_runner_schema(self):
        Plan = self.Seal()
        self.assertEqual("FILES_ONLY_UNEXECUTED", Plan["State"])
        self.assertEqual(14, len(Plan["Files"]))
        self.assertEqual(64, len(self.Identity["ServerToken"]))
        self.assertNotEqual(self.Identity["ServerToken"], self.Identity["ClientToken"])
        self.assertNotEqual(self.Identity["RunId"], self.Identity["CoordinatorRunId"])
        self.assertNotIn(self.Identity["ServerToken"], (self.Private / "copy-plan.json").read_text())
        for Entry in Plan["Files"]:
            Source = Path(Entry["Source"])
            Destination = Path(Entry["Destination"])
            self.assertEqual(Hash(Source), Entry["Sha256"])
            Destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(Source, Destination)
        Host = json.loads((self.Private / "host-config.json").read_text())
        Campaign.WorkflowAndManifest(Host)
        for Role in ("SERVER", "CLIENT"):
            TicketFile = self.Private / Role / "ticket.json"
            Ticket = json.loads(TicketFile.read_text())
            Campaign.VerifyHostTicket(Ticket, Role, Host)
            Campaign.VerifyRole(Ticket, Role)
            self.assertEqual(Hash(TicketFile), Host["Roles"][Role]["TicketSha256"])
            self.assertEqual(self.Identity["ServerToken" if Role == "SERVER" else "ClientToken"],
                             Ticket["Token"])
        with self.assertRaisesRegex(ValueError, "already sealed"):
            Staging.Seal(self.Private, self.SpecFile)

    def test_rejects_stale_or_swapped_preflight_before_writing_tickets(self):
        File = Path(self.Spec["Roles"]["SERVER"]["PreflightSource"])
        Row = json.loads(File.read_text())
        Row["ObservedUtc"] = "2020-01-01T00:00:00Z"
        Save(File, Row)
        with self.assertRaisesRegex(ValueError, "stale or future preflight"):
            Staging.Seal(self.Private, self.SpecFile)
        self.assertFalse((self.Private / "SERVER").exists())
        Row["ObservedUtc"] = datetime.now(timezone.utc).isoformat()
        Row["RunId"] = "00000000-0000-4000-8000-000000000000"
        Save(File, Row)
        with self.assertRaisesRegex(ValueError, "preflight identity mismatch"):
            Staging.Seal(self.Private, self.SpecFile)

    def test_rejects_path_escape_and_command_field(self):
        Role = self.Spec["Roles"]["SERVER"]
        Role["TicketPath"] = str(self.Root / "outside" / "ticket.json")
        Save(self.SpecFile, self.Spec)
        with self.assertRaisesRegex(ValueError, "escapes endpoint root"):
            Staging.Seal(self.Private, self.SpecFile)
        Role["TicketPath"] = str(self.Root / "server" / "stage" / "ticket.json")
        Role["Command"] = "calc.exe"
        Save(self.SpecFile, self.Spec)
        with self.assertRaisesRegex(ValueError, "invalid SERVER role fields"):
            Staging.Seal(self.Private, self.SpecFile)

    def test_rejects_wrong_manifest_and_duplicate_endpoint(self):
        self.Spec["Roles"]["CLIENT"]["EndpointId"] = "SERVER"
        Save(self.SpecFile, self.Spec)
        with self.assertRaisesRegex(ValueError, "duplicate endpoint identity"):
            Staging.Seal(self.Private, self.SpecFile)
        self.Spec["Roles"]["CLIENT"]["EndpointId"] = "CLIENT"
        Manifest = json.loads(self.Manifest.read_text())
        Manifest["RunId"] = "00000000-0000-4000-8000-000000000000"
        Save(self.Manifest, Manifest)
        Save(self.SpecFile, self.Spec)
        with self.assertRaisesRegex(ValueError, "manifest identity mismatch"):
            Staging.Seal(self.Private, self.SpecFile)

    def test_node_provider_requires_cert_pin_on_both_roles(self):
        Manifest = json.loads(self.Manifest.read_text())
        Manifest.update({"Provider": "Node", "NodeEndpoint": "127.0.0.1:50051",
                         "NodeRootCertificateSha256": "b" * 64,
                         "NodeTokenEnvironment": "GARGANTUAN_ENGINE_ADAPTER_TOKEN"})
        Save(self.Manifest, Manifest)
        for Role in ("SERVER", "CLIENT"):
            Preflight = Path(self.Spec["Roles"][Role]["PreflightSource"])
            Row = json.loads(Preflight.read_text())
            Row.update({"Provider": "Node", "ManifestSha256": Hash(self.Manifest)})
            Save(Preflight, Row)
        with self.assertRaisesRegex(ValueError, "invalid SERVER role fields"):
            self.Seal()
        self.Spec["Roles"]["SERVER"]["NodeRootCertificatePath"] = str(self.Root / "server" / "root-ca.pem")
        with self.assertRaisesRegex(ValueError, "invalid SERVER role fields"):
            self.Seal()
        self.Spec["Roles"]["SERVER"]["NodeStage"] = Pin(
            self.Root / "server" / "node-stage" / "node-stage.json")
        self.Spec["Roles"]["SERVER"]["NodeHelper"] = Pin(
            self.Root / "server" / "installed" / "PhysicalGameSessionFarmNode.ps1")
        with self.assertRaisesRegex(ValueError, "invalid CLIENT role fields"):
            self.Seal()
        self.Spec["Roles"]["CLIENT"]["NodeRootCertificatePath"] = str(self.Root / "client" / "root-ca.pem")
        Plan = self.Seal()
        self.assertEqual(14, len(Plan["Files"]))
        ServerFarm = json.loads((self.Private / "SERVER" / "farm-config.json").read_text())
        ClientFarm = json.loads((self.Private / "CLIENT" / "farm-config.json").read_text())
        self.assertIn("NodeRootCertificatePath", ServerFarm)
        self.assertIn("NodeRootCertificatePath", ClientFarm)
        ServerTicket = json.loads((self.Private / "SERVER" / "ticket.json").read_text())
        ClientTicket = json.loads((self.Private / "CLIENT" / "ticket.json").read_text())
        self.assertEqual(self.Spec["Roles"]["SERVER"]["NodeStage"]["Sha256"],
                         ServerTicket["NodeStageSha256"])
        self.assertEqual(self.Spec["Roles"]["SERVER"]["NodeHelper"]["Sha256"],
                         ServerTicket["NodeHelperSha256"])
        self.assertNotIn("NodeStagePath", ClientTicket)

    def test_role_manifest_must_be_byte_identical(self):
        Alternate = self.Root / "client" / "alternate-manifest.json"
        Alternate.write_text(json.dumps(json.loads(self.Manifest.read_text()), indent=2), encoding="utf-8")
        self.Spec["Roles"]["CLIENT"]["ManifestSource"] = str(Alternate)
        Save(self.SpecFile, self.Spec)
        with self.assertRaisesRegex(ValueError, "role manifest bytes differ"):
            Staging.Seal(self.Private, self.SpecFile)


if __name__ == "__main__":
    unittest.main()
