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

from dependency import GetRoot

UPSTREAM = GetRoot()
sys.path.insert(0, str(UPSTREAM))
from agent_coordinator.control import Assignments, Host, Join  # noqa: E402
from agent_coordinator.transport import Journal  # noqa: E402
from agent_coordinator.workflow import Workflow  # noqa: E402
from farm_lifecycle import GetCatalog  # noqa: E402

FORMAT = "GargantuanFarm32Campaign"
ROLES = ("SERVER", "CLIENT")
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
    Exact(Config, ("Format", "Version", "CreatedUtc", "RunId", "CoordinatorRunId",
                   "SourceCommit", "Role", "EndpointId", "PeerIp", "CoordinatorHost", "Port",
                   "Token", "WorkflowPath", "WorkflowSha256", "ManifestPath", "ManifestSha256",
                   "FarmConfigPath", "FarmConfigSha256", "CaptureConfigPath", "CaptureConfigSha256",
                   "CaptureControllerPath", "CaptureControllerSha256", "PreflightPath", "PreflightSha256",
                   "JournalRoot", "ResultPath"), "role ticket")
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
    Exact(Config, ("Format", "Version", "CreatedUtc", "RunId", "CoordinatorRunId",
                   "SourceCommit", "Role", "EndpointId", "PeerIp", "CoordinatorHost", "Port",
                   "Token", "WorkflowPath", "WorkflowSha256", "ManifestPath", "ManifestSha256",
                   "FarmConfigPath", "FarmConfigSha256", "CaptureConfigPath", "CaptureConfigSha256",
                   "CaptureControllerPath", "CaptureControllerSha256", "PreflightPath", "PreflightSha256",
                   "JournalRoot", "ResultPath"), "host ticket")
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


def WriteNewJson(File, Row):
    File = Path(File).resolve()
    if not File.parent.is_dir():
        raise ValueError("[Qualification:FarmCampaign] output parent missing")
    Bytes = (json.dumps(Row, indent=2, sort_keys=True) + "\n").encode("utf-8")
    with File.open("xb") as Stream:
        Stream.write(Bytes)
    return File


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
    Process = subprocess.Popen([sys.executable, "-B", str(Controller), "role", Config["CaptureConfigPath"]],
                               cwd=str(Controller.parent), stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL, creationflags=Flags)
    Started = time.monotonic()
    JoinCode = 1
    try:
        Deadline = Started + READY_SECONDS
        while not Ready.is_file():
            if Process.poll() is not None:
                raise RuntimeError("[Qualification:FarmCampaign] capture controller ended before ready")
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
            CaptureCode = Process.wait(timeout=max(0.1, Started + CAPTURE_FINISH_SECONDS - time.monotonic()))
        except subprocess.TimeoutExpired as Error:
            raise TimeoutError("[Qualification:FarmCampaign] capture finalization deadline") from Error
        if JoinCode or CaptureCode or not CaptureIndex.is_file() or not RoleIndex.is_file():
            raise RuntimeError("[Qualification:FarmCampaign] role, capture, or evidence index failed")
        WriteNewJson(Config["ResultPath"], {"Format": FORMAT, "Version": 1,
                     "RunId": Config["RunId"], "CoordinatorRunId": Config["CoordinatorRunId"],
                     "Role": Role, "Status": "SEALED_UNQUALIFIED",
                     "RoleIndexSha256": Digest(RoleIndex), "CaptureIndexSha256": Digest(CaptureIndex),
                     "JoinJournalSha256": Digest(JournalRoot / "result.json")})
        return 0
    finally:
        if Process.poll() is None:
            try:
                # The fixed controller owns a bounded Stop/Abort path. Let it
                # release the worker lease even after coordinator failure.
                Process.wait(timeout=max(0.1, Started + CAPTURE_FINISH_SECONDS - time.monotonic()))
            except subprocess.TimeoutExpired:
                Process.terminate()
                try:
                    Process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    Process.kill()
                    Process.wait(timeout=5)


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
    Exact(Config, ("Format", "Version", "RunId", "CoordinatorRunId",
                   "CoordinatorResultPath", "CoordinatorResultSha256",
                   "ServerRoleIndexPath", "ServerRoleIndexSha256",
                   "ClientRoleIndexPath", "ClientRoleIndexSha256",
                   "ServerCaptureIndexPath", "ServerCaptureIndexSha256",
                   "ClientCaptureIndexPath", "ClientCaptureIndexSha256",
                   "DirectionAnalyzerPath", "DirectionAnalyzerSha256",
                   "CaptureBinderPath", "CaptureBinderSha256",
                   "OuterReceiptPath", "DirectionReportPath", "ResultPath"), "reconciliation config")
    if Config["Format"] != FORMAT or Config["Version"] != 1 or not CanonicalUuid(Config["RunId"]) or \
            not CanonicalUuid(Config["CoordinatorRunId"]):
        raise ValueError("[Qualification:FarmCampaign] reconciliation identity mismatch")
    Inputs = {}
    for Name in ("CoordinatorResult", "ServerRoleIndex", "ClientRoleIndex",
                 "ServerCaptureIndex", "ClientCaptureIndex"):
        Inputs[Name] = Pinned(Config[Name + "Path"], Config[Name + "Sha256"], Name)
    Outputs = [Path(Config[Name]).resolve() for Name in
               ("OuterReceiptPath", "DirectionReportPath", "ResultPath")]
    if len(set(Outputs)) != 3 or any(File.exists() or not File.parent.is_dir() for File in Outputs):
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
    Receipt = Binder.BindReceipt(Config["RunId"], Config["CoordinatorRunId"],
                                 Inputs["CoordinatorResult"], Inputs["ServerRoleIndex"],
                                 Inputs["ClientRoleIndex"], Inputs["ServerCaptureIndex"],
                                 Inputs["ClientCaptureIndex"], Outputs[0])
    Sealed = ReadJson(Receipt)
    if (Sealed.get("State") != "SEALED_UNQUALIFIED" or Sealed.get("RunId") != Config["RunId"] or
            Sealed.get("CoordinatorRunId") != Config["CoordinatorRunId"]):
        raise ValueError("[Qualification:FarmCampaign] outer receipt identity mismatch")
    WriteNewJson(Outputs[1], Observation)
    WriteNewJson(Outputs[2], {"Format": FORMAT, "Version": 1,
                 "RunId": Config["RunId"], "CoordinatorRunId": Config["CoordinatorRunId"],
                 "Status": "CAPTURE_DIRECTIONS_MEASURED",
                 "OuterReceiptSha256": Digest(Outputs[0]),
                 "DirectionReportSha256": Digest(Outputs[1]),
                 "ProviderGate": "NOT_MEASURED", "Foundation3LGate": "INCOMPLETE"})
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
