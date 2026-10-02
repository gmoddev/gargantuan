"""Source-only campaign launch boundary tests; no packet capture or farm runs."""

import datetime
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import farm_campaign_runner as Campaign  # noqa: E402


MOCK_CAPTURE = r'''
import json
from pathlib import Path
import sys
import time
Config = json.loads(Path(sys.argv[2]).read_text())
Directory = Path(Config['CaptureRoot']) / Config['RunId']
Directory.mkdir()
(Directory / 'capture-controller-ready.json').write_text(json.dumps({
    'RunId': Config['RunId'], 'CoordinatorRunId': Config['CoordinatorRunId'],
    'Role': Config['Role'], 'ReadyUtc': __import__('datetime').datetime.now(
        __import__('datetime').timezone.utc).isoformat()}))
Deadline = time.monotonic() + 5
while not (Path(Config['RoleEvidenceRoot']) / 'evidence-sha256.json').exists():
    if time.monotonic() >= Deadline:
        sys.exit(7)
    time.sleep(.01)
(Directory / 'capture-sha256.json').write_text(json.dumps({
    'RunId': Config['RunId'], 'CoordinatorRunId': Config['CoordinatorRunId'],
    'Role': Config['Role'], 'State': 'SEALED_UNQUALIFIED'}))
'''


def Save(File, Row):
    File.write_text(json.dumps(Row), encoding="utf-8")
    return File


def Hash(File):
    return hashlib.sha256(File.read_bytes()).hexdigest()


def UtcNow():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


class CampaignTests(unittest.TestCase):
    def setUp(self):
        self.Temporary = tempfile.TemporaryDirectory()
        self.Root = Path(self.Temporary.name)
        self.RunId = str(uuid.uuid4())
        self.CoordinatorRunId = str(uuid.uuid4())
        self.SourceCommit = "a" * 40
        self.Workflow = ROOT / "workflows" / "thirty-two-client-farm-lifecycle.json"
        self.Manifest = Save(self.Root / "run-manifest.json", {
            "Format": "GargantuanPhysicalFarmEndpoint", "Version": 1,
            "RunId": self.RunId, "SourceCommit": self.SourceCommit,
            "Provider": "Local", "Nonces": [str(Number) for Number in range(32)],
        })
        self.Controller = self.Root / "farm_capture_campaign.py"
        self.Controller.write_text(MOCK_CAPTURE, encoding="utf-8")
        self.HostIp = "127.0.0.1"
        self.Port = 39451

    def tearDown(self):
        self.Temporary.cleanup()

    def Role(self, Role):
        Directory = self.Root / Role.lower()
        Directory.mkdir()
        CaptureRoot = Directory / "capture"
        CaptureRoot.mkdir()
        EvidenceParent = Directory / "evidence"
        EvidenceParent.mkdir()
        EvidenceRoot = EvidenceParent / self.RunId
        Preflight = Save(Directory / "preflight.json", {
            "Format": "GargantuanPhysicalFarmPreflight", "Version": 1,
            "RunId": self.RunId, "Role": "Server" if Role == "SERVER" else "Clients",
            "Provider": "Local", "ManifestSha256": Hash(self.Manifest),
            "ObservedUtc": UtcNow(), "Status": "INVENTORY_ONLY",
        })
        Farm = Save(Directory / "farm-config.json", {
            "Role": Role, "RunId": self.RunId, "CoordinatorRunId": self.CoordinatorRunId,
            "SourceCommit": self.SourceCommit, "ManifestSHA256": Hash(self.Manifest),
            "EvidenceRoot": str(EvidenceRoot),
        })
        Capture = Save(Directory / "capture-config.json", {
            "Format": "GargantuanFarm32CaptureCampaign", "Version": 1,
            "Role": Role, "RunId": self.RunId, "CoordinatorRunId": self.CoordinatorRunId,
            "RoleEvidenceRoot": str(EvidenceRoot), "CaptureRoot": str(CaptureRoot),
        })
        Ticket = {
            "Format": Campaign.FORMAT, "Version": 1, "CreatedUtc": UtcNow(),
            "RunId": self.RunId, "CoordinatorRunId": self.CoordinatorRunId,
            "SourceCommit": self.SourceCommit, "Role": Role,
            "EndpointId": Role, "PeerIp": "127.0.0.1", "CoordinatorHost": self.HostIp,
            "Port": self.Port, "Token": ("1" if Role == "SERVER" else "2") * 64,
            "WorkflowPath": str(self.Workflow), "WorkflowSha256": Hash(self.Workflow),
            "ManifestPath": str(self.Manifest), "ManifestSha256": Hash(self.Manifest),
            "FarmConfigPath": str(Farm), "FarmConfigSha256": Hash(Farm),
            "CaptureConfigPath": str(Capture), "CaptureConfigSha256": Hash(Capture),
            "CaptureControllerPath": str(self.Controller), "CaptureControllerSha256": Hash(self.Controller),
            "PreflightPath": str(Preflight), "PreflightSha256": Hash(Preflight),
            "JournalRoot": str(Directory / "journal"), "ResultPath": str(Directory / "result.json"),
        }
        TicketFile = Save(Directory / "ticket.json", Ticket)
        return TicketFile, Ticket, Farm, Capture, Preflight

    def Host(self, ServerTicket, ClientTicket):
        return Save(self.Root / "host-config.json", {
            "Format": Campaign.FORMAT, "Version": 1, "CreatedUtc": UtcNow(),
            "RunId": self.RunId, "CoordinatorRunId": self.CoordinatorRunId,
            "SourceCommit": self.SourceCommit,
            "WorkflowPath": str(self.Workflow), "WorkflowSha256": Hash(self.Workflow),
            "ManifestPath": str(self.Manifest), "ManifestSha256": Hash(self.Manifest),
            "HostIp": self.HostIp, "Port": self.Port,
            "Roles": {
                "SERVER": {"TicketPath": str(ServerTicket), "TicketSha256": Hash(ServerTicket)},
                "CLIENT": {"TicketPath": str(ClientTicket), "TicketSha256": Hash(ClientTicket)},
            },
            "JournalRoot": str(self.Root / "host-journal"),
            "ListeningPath": str(self.Root / "host-listening.json"),
        })

    def test_role_mock_seals_only_unqualified_receipts(self):
        TicketFile, Ticket, Farm, _, _ = self.Role("SERVER")

        def FakeJoin(Endpoint, Workflows, Catalog, Journal):
            self.assertEqual(self.CoordinatorRunId, Endpoint["RunId"])
            self.assertEqual(self.HostIp, Endpoint["CoordinatorHost"])
            self.assertEqual(self.Port, Endpoint["Port"])
            EvidenceRoot = Path(json.loads(Farm.read_text())["EvidenceRoot"])
            EvidenceRoot.mkdir()
            Save(EvidenceRoot / "evidence-sha256.json", {"RunId": self.RunId})
            Journal.Close({"Success": True})
            return 0

        with mock.patch.object(Campaign, "Join", side_effect=FakeJoin), \
                mock.patch.object(Campaign, "GetCatalog", return_value=object()):
            self.assertEqual(0, Campaign.RunRole(TicketFile))
        Result = json.loads(Path(Ticket["ResultPath"]).read_text())
        self.assertEqual("SEALED_UNQUALIFIED", Result["Status"])
        self.assertEqual(self.CoordinatorRunId, Result["CoordinatorRunId"])
        with self.assertRaisesRegex(ValueError, "stale role or capture"):
            Campaign.RunRole(TicketFile)

    def test_role_rejects_stale_preflight_and_changed_capture_pin(self):
        TicketFile, Ticket, _, _, Preflight = self.Role("CLIENT")
        Row = json.loads(Preflight.read_text())
        Row["ObservedUtc"] = "2020-01-01T00:00:00Z"
        Save(Preflight, Row)
        Ticket["PreflightSha256"] = Hash(Preflight)
        Save(TicketFile, Ticket)
        with self.assertRaisesRegex(ValueError, "stale or future preflight"):
            Campaign.RunRole(TicketFile)
        Row["ObservedUtc"] = UtcNow()
        Save(Preflight, Row)
        Ticket["PreflightSha256"] = Hash(Preflight)
        Ticket["CaptureControllerSha256"] = "0" * 64
        Save(TicketFile, Ticket)
        with self.assertRaisesRegex(ValueError, "capture controller pin changed"):
            Campaign.RunRole(TicketFile)

    def test_host_uses_only_pinned_tickets_and_exact_assignment(self):
        ServerFile, _, _, _, _ = self.Role("SERVER")
        ClientFile, _, _, _, _ = self.Role("CLIENT")
        HostFile = self.Host(ServerFile, ClientFile)

        def FakeHost(Address, Port, Assign, Journal, Listening):
            self.assertEqual(self.CoordinatorRunId, Assign.RunId)
            self.assertEqual(set(Campaign.ROLES), set(Assign.Workflow.Value["Roles"]))
            self.assertEqual("SERVER", Assign.Entries["SERVER"]["Role"])
            self.assertEqual(self.HostIp, Address)
            Listening(Port)
            Journal.Close({"Success": True, "RunId": Assign.RunId, "Results": []})
            return 0

        with mock.patch.object(Campaign, "Host", side_effect=FakeHost):
            self.assertEqual(0, Campaign.RunHost(HostFile))
        self.assertEqual("LISTENING_UNQUALIFIED",
                         json.loads((self.Root / "host-listening.json").read_text())["State"])

    def test_host_rejects_ticket_swap_and_unknown_host_field(self):
        ServerFile, _, _, _, _ = self.Role("SERVER")
        ClientFile, Client, _, _, _ = self.Role("CLIENT")
        HostFile = self.Host(ServerFile, ClientFile)
        Client["CoordinatorRunId"] = str(uuid.uuid4())
        Save(ClientFile, Client)
        HostRow = json.loads(HostFile.read_text())
        HostRow["Roles"]["CLIENT"]["TicketSha256"] = Hash(ClientFile)
        Save(HostFile, HostRow)
        with self.assertRaisesRegex(ValueError, "host and role ticket mismatch"):
            Campaign.RunHost(HostFile)
        HostRow["InjectedCommand"] = "calc.exe"
        Save(HostFile, HostRow)
        with self.assertRaisesRegex(ValueError, "invalid host config fields"):
            Campaign.RunHost(HostFile)

    def ReconcileConfig(self, DirectionsBody):
        Files = {}
        for Name in ("CoordinatorResult", "ServerRoleIndex", "ClientRoleIndex",
                     "ServerCaptureIndex", "ClientCaptureIndex"):
            Files[Name] = Save(self.Root / (Name + ".json"), {"RunId": self.RunId})
        Directions = self.Root / "farm_capture_directions.py"
        Directions.write_text(DirectionsBody, encoding="utf-8")
        Binder = self.Root / "farm_capture_campaign.py"
        Binder.write_text(
            "import json\nfrom pathlib import Path\n"
            "def BindReceipt(RunId, CoordinatorRunId, CoordinatorResult, ServerIndex, ClientIndex, "
            "ServerCapture, ClientCapture, Receipt):\n"
            "    Path(Receipt).write_text(json.dumps({'RunId': RunId, "
            "'CoordinatorRunId': CoordinatorRunId, 'State': 'SEALED_UNQUALIFIED'}))\n"
            "    return Receipt\n", encoding="utf-8")
        Config = {"Format": Campaign.FORMAT, "Version": 1,
                  "RunId": self.RunId, "CoordinatorRunId": self.CoordinatorRunId,
                  "DirectionAnalyzerPath": str(Directions), "DirectionAnalyzerSha256": Hash(Directions),
                  "CaptureBinderPath": str(Binder), "CaptureBinderSha256": Hash(Binder),
                  "OuterReceiptPath": str(self.Root / "outer-receipt.json"),
                  "DirectionReportPath": str(self.Root / "directions.json"),
                  "ResultPath": str(self.Root / "campaign-analysis.json")}
        for Name, File in Files.items():
            Config[Name + "Path"] = str(File)
            Config[Name + "Sha256"] = Hash(File)
        return Save(self.Root / "reconcile-config.json", Config)

    def test_reconcile_calls_both_pinned_capture_components_without_provider_pass(self):
        ConfigFile = self.ReconcileConfig(
            "def Analyze(Server, Client):\n"
            "    return {'RunId': '" + self.RunId + "', 'CoordinatorRunId': '" +
            self.CoordinatorRunId + "', 'Status': 'BIDIRECTIONAL_32_TUPLES'}\n")
        self.assertEqual(0, Campaign.Reconcile(ConfigFile))
        Result = json.loads((self.Root / "campaign-analysis.json").read_text())
        self.assertEqual("CAPTURE_DIRECTIONS_MEASURED", Result["Status"])
        self.assertEqual("NOT_MEASURED", Result["ProviderGate"])
        self.assertEqual("INCOMPLETE", Result["Foundation3LGate"])

    def test_reconcile_direction_failure_leaves_no_outer_receipt(self):
        ConfigFile = self.ReconcileConfig(
            "def Analyze(Server, Client):\n"
            "    raise ValueError('missing reverse direction')\n")
        with self.assertRaisesRegex(ValueError, "missing reverse direction"):
            Campaign.Reconcile(ConfigFile)
        self.assertFalse((self.Root / "outer-receipt.json").exists())


if __name__ == "__main__":
    unittest.main()
