"""Bounded, source-only Farm32 coordinator and role launch adapter.

Each endpoint receives a locally staged, one-run ticket. This module never
copies files to another host or accepts an executable command from the wire.
Capture remains an independent, pinned local controller and its receipt is
bound only after the coordinator has completed.
"""

import argparse
import datetime
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import uuid

# The pinned worker Python's ._pth does not add this verified stage directory.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from dependency import GetRoot

UPSTREAM = GetRoot()
sys.path.insert(0, str(UPSTREAM))
from agent_coordinator.control import Assignments, Host, Join  # noqa: E402
from agent_coordinator.transport import Journal  # noqa: E402
from agent_coordinator.workflow import Workflow  # noqa: E402
from farm_lifecycle import GetCatalog  # noqa: E402

FORMAT = "GargantuanFarm32Campaign"
ROLES = ("SERVER", "CLIENT")
ROLE_KEYS = ("Format", "Version", "CreatedUtc", "RunId", "CoordinatorRunId",
             "SourceCommit", "Role", "EndpointId", "PeerIp", "CoordinatorHost", "Port",
             "Token", "WorkflowPath", "WorkflowSha256", "ManifestPath", "ManifestSha256",
             "FarmConfigPath", "FarmConfigSha256", "CaptureConfigPath", "CaptureConfigSha256",
             "CaptureControllerPath", "CaptureControllerSha256", "PreflightPath", "PreflightSha256",
             "JournalRoot", "ResultPath")
NODE_ROLE_KEYS = ("NodeStagePath", "NodeStageSha256", "NodeHelperPath", "NodeHelperSha256")
RECONCILE_KEYS = ("Format", "Version", "RunId", "CoordinatorRunId",
                  "CoordinatorResultPath", "CoordinatorResultSha256",
                  "ServerRoleIndexPath", "ServerRoleIndexSha256",
                  "ClientRoleIndexPath", "ClientRoleIndexSha256",
                  "ServerCaptureIndexPath", "ServerCaptureIndexSha256",
                  "ClientCaptureIndexPath", "ClientCaptureIndexSha256",
                  "DirectionAnalyzerPath", "DirectionAnalyzerSha256",
                  "CaptureBinderPath", "CaptureBinderSha256",
                  "OuterReceiptPath", "DirectionReportPath", "ResultPath")
NODE_RECONCILE_KEYS = ("NodePowerShellPath", "NodePowerShellSha256",
                       "NodeTlsMatcherPath", "NodeTlsMatcherSha256",
                       "ServerNodeProviderReceiptPath", "ServerNodeProviderReceiptSha256",
                       "NodeStagePath", "NodeStageSha256",
                       "NodeRunReceiptPath", "NodeRunReceiptSha256",
                       "NodeTlsMatchReceiptPath")
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
COMMIT = re.compile(r"[0-9a-f]{40}\Z")
READY_SECONDS = 20
CAPTURE_FINISH_SECONDS = 800  # Controller owns bounded stop/autostop and offline export.
PREFLIGHT_AGE_SECONDS = 120
TICKET_AGE_SECONDS = 3600
MAX_JSON_BYTES = 65536


def Exact(Value, Keys, Name):
    if not isinstance(Value, dict) or set(Value) != set(Keys):
        raise ValueError("[Qualification:FarmCampaign] invalid " + Name + " fields")
    return Value


def ReadJson(File, Maximum=MAX_JSON_BYTES):
    File = Path(File).resolve(strict=True)
    if not File.is_file() or File.stat().st_size > Maximum:
        raise ValueError("[Qualification:FarmCampaign] missing or oversized JSON")
    return json.loads(File.read_text(encoding="utf-8"))


def Digest(File):
    with Path(File).open("rb") as Stream:
        return hashlib.file_digest(Stream, "sha256").hexdigest()


def Pinned(PathText, Hash, Name, BaseName=None):
    if not isinstance(PathText, str) or not isinstance(Hash, str) or not SHA256.fullmatch(Hash):
        raise ValueError("[Qualification:FarmCampaign] invalid " + Name + " pin")
    File = Path(PathText).resolve(strict=True)
    if not File.is_file() or (BaseName is not None and File.name != BaseName) or Digest(File) != Hash:
        raise ValueError("[Qualification:FarmCampaign] " + Name + " pin changed")
    return File


def CanonicalUuid(Value):
    return isinstance(Value, str) and str(uuid.UUID(Value)) == Value


def AgeSeconds(Value):
    if not isinstance(Value, str):
        raise ValueError("[Qualification:FarmCampaign] timestamp missing")
    Moment = datetime.datetime.fromisoformat(Value.replace("Z", "+00:00"))
    if Moment.tzinfo is None:
        raise ValueError("[Qualification:FarmCampaign] timestamp has no zone")
    return (datetime.datetime.now(datetime.timezone.utc) - Moment).total_seconds()


def Fresh(Value, Maximum, Name):
    Age = AgeSeconds(Value)
    if not 0 <= Age <= Maximum:
        raise ValueError("[Qualification:FarmCampaign] stale or future " + Name)


def VerifyPreflight(PathText, Hash, RunId, Role, ManifestHash, Provider):
    File = Pinned(PathText, Hash, "preflight")
    Row = ReadJson(File)
    if (not isinstance(Row, dict) or Row.get("Format") != "GargantuanPhysicalFarmPreflight" or
            Row.get("Version") != 1 or Row.get("RunId") != RunId or
            Row.get("Role") != ("Server" if Role == "SERVER" else "Clients") or
            Row.get("Provider") != Provider or Row.get("Status") != "INVENTORY_ONLY" or
            Row.get("ManifestSha256") != ManifestHash):
        raise ValueError("[Qualification:FarmCampaign] preflight identity mismatch")
    Fresh(Row.get("ObservedUtc"), PREFLIGHT_AGE_SECONDS, "preflight")
    return File


def WorkflowAndManifest(Config):
    WorkflowFile = Pinned(Config["WorkflowPath"], Config["WorkflowSha256"],
                          "workflow", "thirty-two-client-farm-lifecycle.json")
    Schema = Workflow(ReadJson(WorkflowFile))
    if (Schema.Value["SchemaId"] != "gargantuan.thirty-two-client-farm-control" or
            tuple(Schema.Value["Roles"]) != ROLES or Schema.Value["ExecutionTimeout"] != 540):
        raise ValueError("[Qualification:FarmCampaign] unexpected farm workflow")
    ManifestFile = Pinned(Config["ManifestPath"], Config["ManifestSha256"], "manifest", "run-manifest.json")
    Manifest = ReadJson(ManifestFile)
    if (not isinstance(Manifest, dict) or Manifest.get("Format") != "GargantuanPhysicalFarmEndpoint" or
            Manifest.get("Version") != 1 or Manifest.get("RunId") != Config["RunId"] or
            Manifest.get("SourceCommit") != Config["SourceCommit"] or
            Manifest.get("Provider") not in ("Local", "Node") or
            not isinstance(Manifest.get("Nonces"), list) or len(Manifest["Nonces"]) != 32):
        raise ValueError("[Qualification:FarmCampaign] manifest identity mismatch")
    return Schema, Manifest


def VerifyIdentity(Config):
    if (Config.get("Format") != FORMAT or Config.get("Version") != 1 or
            not CanonicalUuid(Config.get("RunId")) or
            not CanonicalUuid(Config.get("CoordinatorRunId")) or
            not isinstance(Config.get("SourceCommit"), str) or
            not COMMIT.fullmatch(Config["SourceCommit"])):
        raise ValueError("[Qualification:FarmCampaign] invalid run identity")
    Fresh(Config.get("CreatedUtc"), TICKET_AGE_SECONDS, "session")


def VerifyRole(Config, Role):
    Exact(Config, ROLE_KEYS + (NODE_ROLE_KEYS if "NodeStagePath" in Config else ()), "role ticket")
    VerifyIdentity(Config)
    if (Config["Role"] != Role or type(Config["Port"]) is not int or
            not 1 <= Config["Port"] <= 65535 or
            not isinstance(Config["Token"], str) or not re.fullmatch(r"[0-9a-f]{64}", Config["Token"])):
        raise ValueError("[Qualification:FarmCampaign] invalid endpoint ticket")
    Schema, Manifest = WorkflowAndManifest(Config)
    FarmFile = Pinned(Config["FarmConfigPath"], Config["FarmConfigSha256"], "farm role config")
    Farm = ReadJson(FarmFile)
    if (Farm.get("Role") != Role or Farm.get("RunId") != Config["RunId"] or
            Farm.get("CoordinatorRunId") != Config["CoordinatorRunId"] or
            Farm.get("ManifestSHA256") != Config["ManifestSha256"] or
            Farm.get("SourceCommit") != Config["SourceCommit"]):
        raise ValueError("[Qualification:FarmCampaign] farm role config mismatch")
    if (Manifest["Provider"] == "Node" and Role == "SERVER") != ("NodeStagePath" in Config):
        raise ValueError("[Qualification:FarmCampaign] Node supervision ticket mismatch")
    if "NodeStagePath" in Config:
        VerifyNodeStage(Config, Farm, Manifest)
    CaptureFile = Pinned(Config["CaptureConfigPath"], Config["CaptureConfigSha256"], "capture config")
    Capture = ReadJson(CaptureFile)
    if (Capture.get("Format") != "GargantuanFarm32CaptureCampaign" or Capture.get("Version") != 1 or
            Capture.get("Role") != Role or Capture.get("RunId") != Config["RunId"] or
            Capture.get("CoordinatorRunId") != Config["CoordinatorRunId"] or
            Path(Capture.get("RoleEvidenceRoot", "")).resolve() != Path(Farm["EvidenceRoot"]).resolve()):
        raise ValueError("[Qualification:FarmCampaign] capture config mismatch")
    CaptureRoot = Path(Capture["CaptureRoot"]).resolve()
    RoleRoot = Path(Farm["EvidenceRoot"]).resolve()
    if CaptureRoot == RoleRoot or CaptureRoot in RoleRoot.parents or RoleRoot in CaptureRoot.parents:
        raise ValueError("[Qualification:FarmCampaign] capture and role evidence overlap")
    Pinned(Config["CaptureControllerPath"], Config["CaptureControllerSha256"],
           "capture controller", "farm_capture_campaign.py")
    VerifyPreflight(Config["PreflightPath"], Config["PreflightSha256"], Config["RunId"],
                    Role, Config["ManifestSha256"], Manifest["Provider"])
    return Schema, Farm, Capture


def VerifyHostTicket(Config, Role, HostConfig):
    """The host verifies a byte-pinned ticket; endpoint-local pins stay local."""
    Exact(Config, ROLE_KEYS + (NODE_ROLE_KEYS if "NodeStagePath" in Config else ()), "host ticket")
    VerifyIdentity(Config)
    if (Config["Role"] != Role or Config["RunId"] != HostConfig["RunId"] or
            Config["CoordinatorRunId"] != HostConfig["CoordinatorRunId"] or
            Config["SourceCommit"] != HostConfig["SourceCommit"] or
            Config["ManifestSha256"] != HostConfig["ManifestSha256"] or
            Config["WorkflowSha256"] != HostConfig["WorkflowSha256"] or
            Config["CoordinatorHost"] != HostConfig["HostIp"] or
            Config["Port"] != HostConfig["Port"] or
            not isinstance(Config["EndpointId"], str) or
            not re.fullmatch(r"[A-Za-z][A-Za-z0-9.-]{0,95}", Config["EndpointId"]) or
            not isinstance(Config["Token"], str) or
            not re.fullmatch(r"[0-9a-f]{64}", Config["Token"])):
        raise ValueError("[Qualification:FarmCampaign] host and role ticket mismatch")
    Manifest = ReadJson(Pinned(HostConfig["ManifestPath"], HostConfig["ManifestSha256"], "manifest"))
    if (Manifest["Provider"] == "Node" and Role == "SERVER") != ("NodeStagePath" in Config):
        raise ValueError("[Qualification:FarmCampaign] host Node supervision ticket mismatch")


def VerifyNodeStage(Config, Farm, Manifest):
    StageFile = Pinned(Config["NodeStagePath"], Config["NodeStageSha256"],
                       "Node stage", "node-stage.json")
    Helper = Pinned(Config["NodeHelperPath"], Config["NodeHelperSha256"],
                    "Node helper", "PhysicalGameSessionFarmNode.ps1")
    PowerShell = Pinned(Farm["PowerShellPath"], Farm["PowerShellSHA256"],
                        "PowerShell", "pwsh.exe")
    Stage = ReadJson(StageFile)
    if (Stage.get("Format") != "GargantuanFarmNodeStage" or Stage.get("Version") != 1 or
            Stage.get("Status") != "STAGED_NOT_TLS_PROVEN" or
            Stage.get("RunId") != Config["RunId"] or
            Stage.get("SourceCommit") != Config["SourceCommit"] or
            Stage.get("RunManifestSha256") != Config["ManifestSha256"] or
            Stage.get("NodeEndpoint") != Manifest.get("NodeEndpoint") or
            Stage.get("NodeTokenEnvironment") != Manifest.get("NodeTokenEnvironment") or
            Stage.get("RootCertificateSha256") != str(Manifest.get("NodeRootCertificateSha256")).lower() or
            Stage.get("HelperSha256") != Config["NodeHelperSha256"]):
        raise ValueError("[Qualification:FarmCampaign] Node stage identity mismatch")
    StageRoot = StageFile.parent
    for Root in (Path(Farm["PackageRoot"]).resolve(), Path(Farm["EvidenceRoot"]).resolve(),
                 Path(Config["JournalRoot"]).resolve(), Path(Config["CaptureConfigPath"]).resolve().parent):
        if StageRoot == Root or StageRoot in Root.parents or Root in StageRoot.parents:
            raise ValueError("[Qualification:FarmCampaign] Node stage overlaps campaign inputs or evidence")
    if any((StageRoot / Name).exists() for Name in
           ("node-run.claim", "node-run.json", "node-tcp-ready.json", "stop.request")):
        raise ValueError("[Qualification:FarmCampaign] Node stage already consumed")
    return PowerShell, Helper, StageRoot


def WriteNewJson(File, Row):
    File = Path(File).resolve()
    if not File.parent.is_dir():
        raise ValueError("[Qualification:FarmCampaign] output parent missing")
    Bytes = (json.dumps(Row, indent=2, sort_keys=True) + "\n").encode("utf-8")
    with File.open("xb") as Stream:
        Stream.write(Bytes)
    return File


def StartNode(Config, Farm, Manifest):
    PowerShell, Helper, StageRoot = VerifyNodeStage(Config, Farm, Manifest)
    Flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    Process = subprocess.Popen([str(PowerShell), "-NoProfile", "-NonInteractive", "-File",
                                str(Helper), "-Mode", "Run", "-StageRoot", str(StageRoot),
                                "-StageSha256", Config["NodeStageSha256"],
                                "-MaximumRuntimeSeconds", "900"],
                               cwd=str(Helper.parent), stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL, creationflags=Flags)
    return Process, StageRoot


def AwaitNodeReady(Config, Process, StageRoot):
    Marker = StageRoot / "node-tcp-ready.json"
    Deadline = time.monotonic() + READY_SECONDS
    while not Marker.is_file():
        if Process.poll() is not None:
            raise RuntimeError("[Qualification:FarmCampaign] Node supervisor ended before TCP readiness")
        if time.monotonic() >= Deadline:
            raise TimeoutError("[Qualification:FarmCampaign] Node TCP readiness deadline")
        time.sleep(0.05)
    Row = ReadJson(Marker, 4096)
    if (Row.get("Format") != "GargantuanFarmNodeTcpReady" or Row.get("Version") != 1 or
            Row.get("RunId") != Config["RunId"] or type(Row.get("Pid")) is not int or
            Row["Pid"] <= 0 or Row.get("TlsProven") is not False or Process.poll() is not None):
        raise ValueError("[Qualification:FarmCampaign] Node TCP marker identity mismatch")
    Fresh(Row.get("ObservedUtc"), READY_SECONDS, "Node TCP readiness")


def StopNode(Config, Process, StageRoot):
    Stop = StageRoot / "stop.request"
    if not Stop.exists():
        with Stop.open("xb") as Stream:
            Stream.write(Config["RunId"].encode("ascii"))
    if Process.wait(timeout=30) != 0:
        raise RuntimeError("[Qualification:FarmCampaign] owned Node supervisor failed")
    Receipt = StageRoot / "node-run.json"
    Row = ReadJson(Receipt, 4096)
    if (Row.get("Format") != "GargantuanFarmNodeRun" or Row.get("Version") != 1 or
            Row.get("RunId") != Config["RunId"] or
            Row.get("StageSha256") != Config["NodeStageSha256"] or
            Row.get("Reason") != "STOP_REQUESTED" or Row.get("TcpReady") is not True or
            Row.get("ChildReaped") is not True):
        raise ValueError("[Qualification:FarmCampaign] owned Node run receipt failed")
    return Receipt


def RunRole(TicketPath):
    Config = ReadJson(TicketPath)
    Role = Config.get("Role")
    if Role not in ROLES:
        raise ValueError("[Qualification:FarmCampaign] invalid role")
    Schema, Farm, Capture = VerifyRole(Config, Role)
    Ready = Path(Capture["CaptureRoot"]).resolve() / Config["RunId"] / "capture-controller-ready.json"
    CaptureIndex = Ready.parent / "capture-sha256.json"
    RoleIndex = Path(Farm["EvidenceRoot"]).resolve() / "evidence-sha256.json"
    if Ready.exists() or CaptureIndex.exists() or RoleIndex.exists():
        raise ValueError("[Qualification:FarmCampaign] stale role or capture evidence")
    JournalRoot = Path(Config["JournalRoot"]).resolve()
    if JournalRoot.exists() or not JournalRoot.parent.is_dir():
        raise ValueError("[Qualification:FarmCampaign] stale journal root")
    ResultPath = Path(Config["ResultPath"]).resolve()
    if ResultPath.exists() or not ResultPath.parent.is_dir():
        raise ValueError("[Qualification:FarmCampaign] stale role result")
    for Root in (Path(Farm["EvidenceRoot"]).resolve(), Path(Capture["CaptureRoot"]).resolve()):
        if (JournalRoot == Root or JournalRoot in Root.parents or Root in JournalRoot.parents or
                ResultPath == Root or Root in ResultPath.parents):
            raise ValueError("[Qualification:FarmCampaign] campaign output overlaps sealed evidence")
    Catalog = GetCatalog(Config["FarmConfigPath"])
    Controller = Pinned(Config["CaptureControllerPath"], Config["CaptureControllerSha256"], "capture controller")
    Flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    CaptureProcess = None
    NodeProcess = None
    NodeRoot = None
    NodeReceipt = None
    Started = None
    JoinCode = 1
    try:
        if "NodeStagePath" in Config:
            Manifest = ReadJson(Pinned(Config["ManifestPath"], Config["ManifestSha256"], "manifest"))
            NodeProcess, NodeRoot = StartNode(Config, Farm, Manifest)
            AwaitNodeReady(Config, NodeProcess, NodeRoot)
        CaptureProcess = subprocess.Popen(
            [sys.executable, "-B", str(Controller), "role", Config["CaptureConfigPath"]],
            cwd=str(Controller.parent), stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, creationflags=Flags)
        Started = time.monotonic()
        Deadline = Started + READY_SECONDS
        while not Ready.is_file():
            if CaptureProcess.poll() is not None:
                raise RuntimeError("[Qualification:FarmCampaign] capture controller ended before ready")
            if NodeProcess is not None and NodeProcess.poll() is not None:
                raise RuntimeError("[Qualification:FarmCampaign] Node supervisor ended before role launch")
            if time.monotonic() >= Deadline:
                raise TimeoutError("[Qualification:FarmCampaign] capture readiness deadline")
            time.sleep(0.05)
        Marker = ReadJson(Ready, 4096)
        if (Marker.get("RunId") != Config["RunId"] or Marker.get("Role") != Role or
                Marker.get("CoordinatorRunId") != Config["CoordinatorRunId"]):
            raise ValueError("[Qualification:FarmCampaign] capture readiness identity mismatch")
        Fresh(Marker.get("ReadyUtc"), READY_SECONDS, "capture readiness")
        Endpoint = {Key: Config[Key] for Key in ("EndpointId", "PeerIp", "CoordinatorHost", "Port", "Token")}
        Endpoint["RunId"] = Config["CoordinatorRunId"]
        JoinCode = Join(Endpoint, {(Schema.Value["SchemaId"], 1): Schema},
                        Catalog, Journal(JournalRoot))
        try:
            CaptureCode = CaptureProcess.wait(timeout=max(0.1, Started + CAPTURE_FINISH_SECONDS - time.monotonic()))
        except subprocess.TimeoutExpired as Error:
            raise TimeoutError("[Qualification:FarmCampaign] capture finalization deadline") from Error
        if JoinCode or CaptureCode or not CaptureIndex.is_file() or not RoleIndex.is_file():
            raise RuntimeError("[Qualification:FarmCampaign] role, capture, or evidence index failed")
        if NodeProcess is not None:
            NodeReceipt = StopNode(Config, NodeProcess, NodeRoot)
        Result = {"Format": FORMAT, "Version": 1,
                     "RunId": Config["RunId"], "CoordinatorRunId": Config["CoordinatorRunId"],
                     "Role": Role, "Status": "SEALED_UNQUALIFIED",
                     "RoleIndexSha256": Digest(RoleIndex), "CaptureIndexSha256": Digest(CaptureIndex),
                     "JoinJournalSha256": Digest(JournalRoot / "result.json")}
        if NodeReceipt is not None:
            Result["NodeStageSha256"] = Config["NodeStageSha256"]
            Result["NodeRunReceiptSha256"] = Digest(NodeReceipt)
        WriteNewJson(Config["ResultPath"], Result)
        return 0
    finally:
        if CaptureProcess is not None and CaptureProcess.poll() is None:
            try:
                # The fixed controller owns a bounded Stop/Abort path. Let it
                # release the worker lease even after coordinator failure.
                CaptureProcess.wait(timeout=max(0.1, Started + CAPTURE_FINISH_SECONDS - time.monotonic()))
            except subprocess.TimeoutExpired:
                CaptureProcess.terminate()
                try:
                    CaptureProcess.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    CaptureProcess.kill()
                    CaptureProcess.wait(timeout=5)
        if NodeProcess is not None and NodeReceipt is None:
            StopNode(Config, NodeProcess, NodeRoot)


def RunHost(ConfigPath):
    Config = ReadJson(ConfigPath)
    Exact(Config, ("Format", "Version", "CreatedUtc", "RunId", "CoordinatorRunId",
                   "SourceCommit", "WorkflowPath", "WorkflowSha256", "ManifestPath",
                   "ManifestSha256", "HostIp", "Port", "Roles", "JournalRoot", "ListeningPath"),
          "host config")
    VerifyIdentity(Config)
    Schema, Manifest = WorkflowAndManifest(Config)
    if type(Config["Port"]) is not int or not 1 <= Config["Port"] <= 65535 or set(Config["Roles"]) != set(ROLES):
        raise ValueError("[Qualification:FarmCampaign] invalid host endpoints")
    Endpoints = {}
    for Role in ROLES:
        Entry = Exact(Config["Roles"][Role], ("TicketPath", "TicketSha256"), "host ticket pin")
        Ticket = ReadJson(Pinned(Entry["TicketPath"], Entry["TicketSha256"], "role ticket"))
        VerifyHostTicket(Ticket, Role, Config)
        Endpoints[Role] = {"EndpointId": Ticket["EndpointId"], "PeerIp": Ticket["PeerIp"],
                           "RunId": Ticket["CoordinatorRunId"], "Token": Ticket["Token"]}
    Assign = Assignments(Schema, Endpoints, {})
    Assign.RunId = Config["CoordinatorRunId"]
    Root = Path(Config["JournalRoot"]).resolve()
    if Root.exists() or not Root.parent.is_dir():
        raise ValueError("[Qualification:FarmCampaign] stale coordinator journal root")
    Marker = Path(Config["ListeningPath"]).resolve()
    if Marker.exists() or not Marker.parent.is_dir():
        raise ValueError("[Qualification:FarmCampaign] stale listening marker")

    def Listening(Port):
        if Port != Config["Port"]:
            raise ValueError("[Qualification:FarmCampaign] control port changed")
        WriteNewJson(Marker, {"RunId": Config["RunId"],
                     "CoordinatorRunId": Config["CoordinatorRunId"], "HostIp": Config["HostIp"],
                     "Port": Port, "State": "LISTENING_UNQUALIFIED"})

    return Host(Config["HostIp"], Config["Port"], Assign, Journal(Root), Listening)


def ImportPinnedModule(PathText, Hash, BaseName, ModuleName):
    Source = Pinned(PathText, Hash, ModuleName, BaseName)
    Spec = importlib.util.spec_from_file_location(ModuleName, Source)
    if Spec is None or Spec.loader is None:
        raise ValueError("[Qualification:FarmCampaign] pinned analysis module is unavailable")
    Module = importlib.util.module_from_spec(Spec)
    Spec.loader.exec_module(Module)
    return Module


def Reconcile(ConfigPath):
    """Bind immutable role/capture evidence and check 32 bidirectional tuples.

    This is an offline capture gate only. Node TLS and remaining 3L gates stay
    independent and cannot be promoted by this report.
    """
    Config = ReadJson(ConfigPath)
    Node = "NodeStagePath" in Config
    Exact(Config, RECONCILE_KEYS + (NODE_RECONCILE_KEYS if Node else ()), "reconciliation config")
    if Config["Format"] != FORMAT or Config["Version"] != 1 or not CanonicalUuid(Config["RunId"]) or \
            not CanonicalUuid(Config["CoordinatorRunId"]):
        raise ValueError("[Qualification:FarmCampaign] reconciliation identity mismatch")
    Inputs = {}
    for Name in ("CoordinatorResult", "ServerRoleIndex", "ClientRoleIndex",
                 "ServerCaptureIndex", "ClientCaptureIndex"):
        Inputs[Name] = Pinned(Config[Name + "Path"], Config[Name + "Sha256"], Name)
    if Node:
        for Name in ("ServerNodeProviderReceipt", "NodeStage", "NodeRunReceipt"):
            Inputs[Name] = Pinned(Config[Name + "Path"], Config[Name + "Sha256"], Name)
        ServerIndex = ReadJson(Inputs["ServerRoleIndex"])
        ProviderReceipt = Inputs["ServerNodeProviderReceipt"]
        Members = [Entry for Entry in ServerIndex.get("Files", [])
                   if isinstance(Entry, dict) and Entry.get("Name") == "node-provider.json"]
        if (ProviderReceipt != Inputs["ServerRoleIndex"].parent / "node-provider.json" or
                len(Members) != 1 or Members[0].get("Sha256") != Config["ServerNodeProviderReceiptSha256"] or
                Members[0].get("Bytes") != ProviderReceipt.stat().st_size):
            raise ValueError("[Qualification:FarmCampaign] Node provider receipt is not sealed by server role")
        if (Inputs["NodeStage"].name != "node-stage.json" or
                Inputs["NodeRunReceipt"] != Inputs["NodeStage"].parent / "node-run.json"):
            raise ValueError("[Qualification:FarmCampaign] Node stage/run paths differ")
    Outputs = [Path(Config[Name]).resolve() for Name in
               ("OuterReceiptPath", "DirectionReportPath", "ResultPath")]
    if Node:
        Outputs.append(Path(Config["NodeTlsMatchReceiptPath"]).resolve())
    if len(set(Outputs)) != len(Outputs) or any(File.exists() or not File.parent.is_dir() for File in Outputs):
        raise ValueError("[Qualification:FarmCampaign] stale or missing reconciliation output")
    for File in Outputs:
        if any(File == Source.parent or Source.parent in File.parents for Source in Inputs.values()):
            raise ValueError("[Qualification:FarmCampaign] reconciliation output overlaps sealed evidence")
    Directions = ImportPinnedModule(Config["DirectionAnalyzerPath"], Config["DirectionAnalyzerSha256"],
                                    "farm_capture_directions.py", "farm_capture_directions")
    Binder = ImportPinnedModule(Config["CaptureBinderPath"], Config["CaptureBinderSha256"],
                                "farm_capture_campaign.py", "farm_capture_campaign")
    Observation = Directions.Analyze(Inputs["ServerCaptureIndex"], Inputs["ClientCaptureIndex"])
    if (Observation.get("RunId") != Config["RunId"] or
            Observation.get("CoordinatorRunId") != Config["CoordinatorRunId"] or
            Observation.get("Status") != "BIDIRECTIONAL_32_TUPLES"):
        raise ValueError("[Qualification:FarmCampaign] capture direction identity mismatch")
    NodeMatch = None
    if Node:
        PowerShell = Pinned(Config["NodePowerShellPath"], Config["NodePowerShellSha256"],
                            "Node analysis PowerShell", "pwsh.exe")
        Matcher = Pinned(Config["NodeTlsMatcherPath"], Config["NodeTlsMatcherSha256"],
                         "Node TLS matcher", "PhysicalGameSessionFarmNodeTls.ps1")
        try:
            Completed = subprocess.run(
                [str(PowerShell), "-NoProfile", "-NonInteractive", "-File", str(Matcher),
                 "-ServerReceiptPath", str(Inputs["ServerNodeProviderReceipt"]),
                 "-NodeStagePath", str(Inputs["NodeStage"]),
                 "-NodeRunReceiptPath", str(Inputs["NodeRunReceipt"]),
                 "-NodeRunReceiptSha256", Config["NodeRunReceiptSha256"],
                 "-NodeStageSha256", Config["NodeStageSha256"],
                 "-OutputPath", str(Outputs[3])],
                cwd=str(Matcher.parent), capture_output=True, timeout=20,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        except subprocess.TimeoutExpired as Error:
            raise TimeoutError("[Qualification:FarmCampaign] Node TLS matcher deadline") from Error
        if Completed.returncode != 0:
            raise RuntimeError("[Qualification:FarmCampaign] pinned Node TLS matcher failed")
        NodeMatch = ReadJson(Outputs[3], 4096)
        if (NodeMatch.get("Format") != "GargantuanFarmNodeTlsLogMatch" or
                NodeMatch.get("Version") != 1 or
                NodeMatch.get("State") != "OFFLINE_LOG_MATCH_BOUND_TO_PINNED_NODE_RUN" or
                NodeMatch.get("RunId") != Config["RunId"] or
                NodeMatch.get("NodeStageSha256") != Config["NodeStageSha256"] or
                NodeMatch.get("NodeRunReceiptSha256") != Config["NodeRunReceiptSha256"]):
            raise ValueError("[Qualification:FarmCampaign] Node TLS match identity mismatch")
    Receipt = Binder.BindReceipt(Config["RunId"], Config["CoordinatorRunId"],
                                 Inputs["CoordinatorResult"], Inputs["ServerRoleIndex"],
                                 Inputs["ClientRoleIndex"], Inputs["ServerCaptureIndex"],
                                 Inputs["ClientCaptureIndex"], Outputs[0])
    Sealed = ReadJson(Receipt)
    if (Sealed.get("State") != "SEALED_UNQUALIFIED" or Sealed.get("RunId") != Config["RunId"] or
            Sealed.get("CoordinatorRunId") != Config["CoordinatorRunId"]):
        raise ValueError("[Qualification:FarmCampaign] outer receipt identity mismatch")
    WriteNewJson(Outputs[1], Observation)
    Result = {"Format": FORMAT, "Version": 1,
                 "RunId": Config["RunId"], "CoordinatorRunId": Config["CoordinatorRunId"],
                 "Status": "CAPTURE_DIRECTIONS_MEASURED",
                 "OuterReceiptSha256": Digest(Outputs[0]),
                 "DirectionReportSha256": Digest(Outputs[1]),
                 "ProviderGate": "NOT_MEASURED", "Foundation3LGate": "INCOMPLETE"}
    if NodeMatch is not None:
        Result["NodeTlsEvidence"] = NodeMatch["State"]
        Result["NodeTlsMatchReceiptSha256"] = Digest(Outputs[3])
    WriteNewJson(Outputs[2], Result)
    return 0


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("Action", choices=("host", "role", "reconcile"))
    Parser.add_argument("ConfigPath")
    Args = Parser.parse_args()
    if Args.Action == "host":
        return RunHost(Args.ConfigPath)
    if Args.Action == "role":
        return RunRole(Args.ConfigPath)
    return Reconcile(Args.ConfigPath)


if __name__ == "__main__":
    try:
        raise SystemExit(Main())
    except (ValueError, RuntimeError, TimeoutError, OSError) as Error:
        print(str(Error), file=sys.stderr)
        raise SystemExit(1)
