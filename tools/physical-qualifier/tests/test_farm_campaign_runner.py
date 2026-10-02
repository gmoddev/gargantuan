"""Source-only campaign launch boundary tests; no packet capture or farm runs."""

import ctypes
import datetime
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import farm_campaign_runner as Campaign  # noqa: E402
from private_ticket_acl import Harden  # noqa: E402


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

    @unittest.skipUnless(sys.platform == "win32", "Windows 8.3 path alias")
    def test_pinned_file_resolves_existing_short_path_alias(self):
        PowerShell = self.Root / "pwsh.exe"
        PowerShell.write_bytes(b"pinned test PowerShell")
        Buffer = ctypes.create_unicode_buffer(1024)
        Length = ctypes.windll.kernel32.GetShortPathNameW(str(PowerShell), Buffer, len(Buffer))
        if not Length or Length >= len(Buffer) or \
                Buffer.value.casefold() == str(PowerShell.resolve(strict=True)).casefold():
            self.skipTest("volume has no distinct 8.3 alias")
        self.assertEqual(PowerShell.resolve(strict=True),
                         Campaign.Pinned(Buffer.value, Hash(PowerShell), "PowerShell", "pwsh.exe"))

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

    def NodeRole(self):
        TicketFile, Ticket, FarmFile, _, PreflightFile = self.Role("SERVER")
        Manifest = json.loads(self.Manifest.read_text())
        Manifest.update({"Provider": "Node", "NodeEndpoint": "127.0.0.1:50051",
                         "NodeRootCertificateSha256": "b" * 64,
                         "NodeTokenEnvironment": "GARGANTUAN_ENGINE_ADAPTER_TOKEN"})
        Save(self.Manifest, Manifest)
        Preflight = json.loads(PreflightFile.read_text())
        Preflight.update({"Provider": "Node", "ManifestSha256": Hash(self.Manifest)})
        Save(PreflightFile, Preflight)
        Farm = json.loads(FarmFile.read_text())
        Farm["ManifestSHA256"] = Hash(self.Manifest)
        PowerShell = self.Root / "pwsh.exe"
        PowerShell.write_bytes(b"pinned test PowerShell")
        Farm["PowerShellPath"] = str(PowerShell)
        Farm["PowerShellSHA256"] = Hash(PowerShell)
        Package = self.Root / "package"
        Package.mkdir()
        Farm["PackageRoot"] = str(Package)
        Save(FarmFile, Farm)
        Helper = self.Root / "PhysicalGameSessionFarmNode.ps1"
        Helper.write_bytes(b"pinned Node helper")
        StageRoot = self.Root / "node-stage"
        StageRoot.mkdir()
        TokenRoot = self.Root / self.RunId
        TokenRoot.mkdir()
        Harden(TokenRoot)
        TokenFile = TokenRoot / "node-token.secret"
        TokenFile.write_text("f" * 64, encoding="ascii")
        StageFile = Save(StageRoot / "node-stage.json", {
            "Format": "GargantuanFarmNodeStage", "Version": 1,
            "Status": "STAGED_NOT_TLS_PROVEN", "RunId": self.RunId,
            "SourceCommit": self.SourceCommit, "RunManifestSha256": Hash(self.Manifest),
            "NodeEndpoint": Manifest["NodeEndpoint"],
            "NodeTokenEnvironment": Manifest["NodeTokenEnvironment"],
            "NodeTokenFilePath": str(TokenFile), "NodeTokenFileSha256": Hash(TokenFile),
            "RootCertificateSha256": Manifest["NodeRootCertificateSha256"],
            "HelperSha256": Hash(Helper),
        })
        Ticket.update({"ManifestSha256": Hash(self.Manifest),
                       "PreflightSha256": Hash(PreflightFile),
                       "FarmConfigSha256": Hash(FarmFile),
                       "NodeStagePath": str(StageFile), "NodeStageSha256": Hash(StageFile),
                       "NodeHelperPath": str(Helper), "NodeHelperSha256": Hash(Helper)})
        Save(TicketFile, Ticket)
        return TicketFile, Ticket, FarmFile, StageRoot

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

    def test_node_server_requires_pinned_stage_and_stops_owned_child(self):
        TicketFile, Ticket, FarmFile, StageRoot = self.NodeRole()
        Campaign.VerifyRole(Ticket, "SERVER")
        Events = []
        NodeProcess = mock.Mock()
        NodeProcess.poll.return_value = None

        def FakeStart(Config, Farm, Manifest):
            Events.append("node-start")
            self.assertEqual("f" * 64, os.environ[Manifest["NodeTokenEnvironment"]])
            return NodeProcess, StageRoot

        def FakeJoin(Endpoint, Workflows, Catalog, Journal):
            Events.append("join")
            self.assertEqual("f" * 64, os.environ["GARGANTUAN_ENGINE_ADAPTER_TOKEN"])
            EvidenceRoot = Path(json.loads(FarmFile.read_text())["EvidenceRoot"])
            EvidenceRoot.mkdir()
            Save(EvidenceRoot / "evidence-sha256.json", {"RunId": self.RunId})
            Journal.Close({"Success": True})
            return 0

        def FakeStop(Config, Process, Root):
            Events.append("node-stop")
            self.assertIs(NodeProcess, Process)
            self.assertTrue((Path(json.loads(FarmFile.read_text())["EvidenceRoot"]) /
                             "evidence-sha256.json").is_file())
            return Save(Root / "node-run.json", {"RunId": self.RunId})

        with mock.patch.object(Campaign, "StartNode", side_effect=FakeStart), \
                mock.patch.object(Campaign, "AwaitNodeReady", side_effect=lambda *Args: Events.append("node-ready")), \
                mock.patch.object(Campaign, "StopNode", side_effect=FakeStop), \
                mock.patch.object(Campaign, "Join", side_effect=FakeJoin), \
                mock.patch.object(Campaign, "GetCatalog", return_value=object()):
            self.assertEqual(0, Campaign.RunRole(TicketFile))
        self.assertEqual(["node-start", "node-ready", "join", "node-stop"], Events)
        self.assertNotEqual("f" * 64, os.environ.get("GARGANTUAN_ENGINE_ADAPTER_TOKEN"))
        Result = json.loads(Path(Ticket["ResultPath"]).read_text())
        self.assertEqual(Hash(StageRoot / "node-run.json"), Result["NodeRunReceiptSha256"])
        self.assertEqual(Ticket["NodeStageSha256"], Result["NodeStageSha256"])

    def test_node_stage_swap_and_client_supervision_are_denied(self):
        TicketFile, Ticket, _, StageRoot = self.NodeRole()
        Stage = json.loads((StageRoot / "node-stage.json").read_text())
        Stage["RunId"] = str(uuid.uuid4())
        Save(StageRoot / "node-stage.json", Stage)
        Ticket["NodeStageSha256"] = Hash(StageRoot / "node-stage.json")
        Save(TicketFile, Ticket)
        with self.assertRaisesRegex(ValueError, "Node stage identity mismatch"):
            Campaign.VerifyRole(Ticket, "SERVER")
        ClientTicket, Client, _, _, _ = self.Role("CLIENT")
        Client.update({Key: Ticket[Key] for Key in Campaign.NODE_ROLE_KEYS})
        Save(ClientTicket, Client)
        with self.assertRaisesRegex(ValueError, "Node supervision ticket mismatch"):
            Campaign.VerifyRole(Client, "CLIENT")

    def test_node_token_tamper_fails_before_child_and_restores_environment(self):
        TicketFile, _, _, StageRoot = self.NodeRole()
        Stage = json.loads((StageRoot / "node-stage.json").read_text())
        Path(Stage["NodeTokenFilePath"]).write_text("e" * 64, encoding="ascii")
        Previous = os.environ.get("GARGANTUAN_ENGINE_ADAPTER_TOKEN")
        with mock.patch.object(Campaign, "GetCatalog", return_value=object()), \
                mock.patch.object(Campaign, "StartNode") as Start:
            with self.assertRaisesRegex(ValueError, "Node token file pin mismatch"):
                Campaign.RunRole(TicketFile)
            Start.assert_not_called()
        self.assertEqual(Previous, os.environ.get("GARGANTUAN_ENGINE_ADAPTER_TOKEN"))

    def test_node_child_command_marker_and_run_receipt_are_bounded(self):
        _, Ticket, FarmFile, StageRoot = self.NodeRole()
        Farm = json.loads(FarmFile.read_text())
        Child = mock.Mock()
        Child.poll.return_value = None
        Child.wait.return_value = 0
        with mock.patch.object(Campaign.subprocess, "Popen", return_value=Child) as Spawn:
            Process, Root = Campaign.StartNode(Ticket, Farm, json.loads(self.Manifest.read_text()))
        self.assertIs(Child, Process)
        self.assertEqual(StageRoot.resolve(strict=True), Root)
        Args = Spawn.call_args.args[0]
        self.assertEqual(str(Path(Farm["PowerShellPath"]).resolve(strict=True)), Args[0])
        self.assertIn(Ticket["NodeStageSha256"], Args)
        self.assertIn("900", Args)
        Save(StageRoot / "node-tcp-ready.json", {
            "Format": "GargantuanFarmNodeTcpReady", "Version": 1,
            "RunId": self.RunId, "Pid": 101, "TlsProven": False,
            "ObservedUtc": UtcNow(),
        })
        Campaign.AwaitNodeReady(Ticket, Child, StageRoot)
        Marker = StageRoot / "node-tcp-ready.json"
        BadMarker = json.loads(Marker.read_text())
        BadMarker["TlsProven"] = True
        Save(Marker, BadMarker)
        with self.assertRaisesRegex(ValueError, "TCP marker identity mismatch"):
            Campaign.AwaitNodeReady(Ticket, Child, StageRoot)
        Receipt = Save(StageRoot / "node-run.json", {
            "Format": "GargantuanFarmNodeRun", "Version": 1,
            "RunId": self.RunId, "StageSha256": Ticket["NodeStageSha256"],
            "Reason": "STOP_REQUESTED", "TcpReady": True, "ChildReaped": True,
        })
        self.assertEqual(Receipt.resolve(strict=True), Campaign.StopNode(Ticket, Child, StageRoot))
        self.assertEqual(self.RunId, (StageRoot / "stop.request").read_text())
        Bad = json.loads(Receipt.read_text())
        Bad["Reason"] = "CHILD_EXITED"
        Save(Receipt, Bad)
        with self.assertRaisesRegex(ValueError, "owned Node run receipt failed"):
            Campaign.StopNode(Ticket, Child, StageRoot)

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
        Sealed = self.Root / "sealed"
        Sealed.mkdir()
        Analysis = self.Root / "analysis"
        Analysis.mkdir()
        Files = {}
        for Name in ("CoordinatorResult", "ServerRoleIndex", "ClientRoleIndex",
                     "ServerCaptureIndex", "ClientCaptureIndex"):
            Files[Name] = Save(Sealed / (Name + ".json"), {"RunId": self.RunId})
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
                  "OuterReceiptPath": str(Analysis / "outer-receipt.json"),
                  "DirectionReportPath": str(Analysis / "directions.json"),
                  "ResultPath": str(Analysis / "campaign-analysis.json")}
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
        Result = json.loads((self.Root / "analysis" / "campaign-analysis.json").read_text())
        self.assertEqual("CAPTURE_DIRECTIONS_MEASURED", Result["Status"])
        self.assertEqual("NOT_MEASURED", Result["ProviderGate"])
        self.assertEqual("INCOMPLETE", Result["Foundation3LGate"])

    def test_reconcile_direction_failure_leaves_no_outer_receipt(self):
        ConfigFile = self.ReconcileConfig(
            "def Analyze(Server, Client):\n"
            "    raise ValueError('missing reverse direction')\n")
        with self.assertRaisesRegex(ValueError, "missing reverse direction"):
            Campaign.Reconcile(ConfigFile)
        self.assertFalse((self.Root / "analysis" / "outer-receipt.json").exists())

    def test_node_reconcile_binds_sealed_server_receipt_and_independent_tls_match(self):
        ConfigFile = self.ReconcileConfig(
            "def Analyze(Server, Client):\n"
            "    return {'RunId': '" + self.RunId + "', 'CoordinatorRunId': '" +
            self.CoordinatorRunId + "', 'Status': 'BIDIRECTIONAL_32_TUPLES'}\n")
        Config = json.loads(ConfigFile.read_text())
        ServerIndex = Path(Config["ServerRoleIndexPath"])
        Provider = Save(ServerIndex.parent / "node-provider.json", {"RunId": self.RunId})
        Save(ServerIndex, {"RunId": self.RunId, "Role": "SERVER", "Files": [
            {"Name": "node-provider.json", "Bytes": Provider.stat().st_size, "Sha256": Hash(Provider)}]})
        NodeRoot = self.Root / "node-stage"
        NodeRoot.mkdir()
        Stage = Save(NodeRoot / "node-stage.json", {"RunId": self.RunId})
        Run = Save(NodeRoot / "node-run.json", {"RunId": self.RunId})
        PowerShell = self.Root / "pwsh.exe"
        PowerShell.write_bytes(b"pinned PowerShell")
        Matcher = self.Root / "PhysicalGameSessionFarmNodeTls.ps1"
        Matcher.write_bytes(b"pinned TLS matcher")
        Config.update({
            "ServerRoleIndexSha256": Hash(ServerIndex),
            "ServerNodeProviderReceiptPath": str(Provider),
            "ServerNodeProviderReceiptSha256": Hash(Provider),
            "NodeStagePath": str(Stage), "NodeStageSha256": Hash(Stage),
            "NodeRunReceiptPath": str(Run), "NodeRunReceiptSha256": Hash(Run),
            "NodePowerShellPath": str(PowerShell), "NodePowerShellSha256": Hash(PowerShell),
            "NodeTlsMatcherPath": str(Matcher), "NodeTlsMatcherSha256": Hash(Matcher),
            "NodeTlsMatchReceiptPath": str(self.Root / "analysis" / "node-tls-match.json"),
        })
        Save(ConfigFile, Config)

        def FakeMatch(Args, **Kwargs):
            self.assertEqual(str(PowerShell.resolve(strict=True)), Args[0])
            self.assertIn(Hash(Stage), Args)
            self.assertIn(Hash(Run), Args)
            Save(Path(Config["NodeTlsMatchReceiptPath"]), {
                "Format": "GargantuanFarmNodeTlsLogMatch", "Version": 1,
                "State": "OFFLINE_LOG_MATCH_BOUND_TO_PINNED_NODE_RUN",
                "RunId": self.RunId, "NodeStageSha256": Hash(Stage),
                "NodeRunReceiptSha256": Hash(Run),
            })
            return mock.Mock(returncode=0)

        with mock.patch.object(Campaign.subprocess, "run", side_effect=FakeMatch):
            self.assertEqual(0, Campaign.Reconcile(ConfigFile))
        Result = json.loads(Path(Config["ResultPath"]).read_text())
        self.assertEqual("NOT_MEASURED", Result["ProviderGate"])
        self.assertEqual("OFFLINE_LOG_MATCH_BOUND_TO_PINNED_NODE_RUN", Result["NodeTlsEvidence"])
        self.assertEqual(Hash(Path(Config["NodeTlsMatchReceiptPath"])),
                         Result["NodeTlsMatchReceiptSha256"])

        Path(Config["ResultPath"]).unlink()
        Path(Config["OuterReceiptPath"]).unlink()
        Path(Config["DirectionReportPath"]).unlink()
        Path(Config["NodeTlsMatchReceiptPath"]).unlink()
        Save(ServerIndex, {"RunId": self.RunId, "Role": "SERVER", "Files": []})
        Config["ServerRoleIndexSha256"] = Hash(ServerIndex)
        Save(ConfigFile, Config)
        with self.assertRaisesRegex(ValueError, "not sealed by server role"):
            Campaign.Reconcile(ConfigFile)

    def test_reconcile_refuses_output_inside_sealed_capture_root(self):
        ConfigFile = self.ReconcileConfig(
            "def Analyze(Server, Client):\n"
            "    raise AssertionError('must not analyze unsafe output')\n")
        Config = json.loads(ConfigFile.read_text())
        Config["OuterReceiptPath"] = str(self.Root / "sealed" / "mutated-capture-root.json")
        Save(ConfigFile, Config)
        with self.assertRaisesRegex(ValueError, "overlaps sealed evidence"):
            Campaign.Reconcile(ConfigFile)


if __name__ == "__main__":
    unittest.main()
