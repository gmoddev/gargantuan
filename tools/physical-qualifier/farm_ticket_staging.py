"""Create one-use Farm32 control tickets without executing on either endpoint.

``new`` creates secret run identity before the run manifest and preflights.
``seal`` consumes those fresh, independently generated inputs and writes fixed
role tickets/configs plus an explicit file-copy plan. It never invokes SSH/SCP,
starts a process, or writes into a package or evidence root.
"""

import argparse
from datetime import datetime, timezone
import hashlib
import ipaddress
import json
from pathlib import Path, PureWindowsPath
import re
import secrets
import sys
import uuid


FORMAT = "GargantuanFarm32Campaign"
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
COMMIT = re.compile(r"[0-9a-f]{40}\Z")
ENDPOINT_ID = re.compile(r"[A-Za-z][A-Za-z0-9.-]{0,95}\Z")
MAX_JSON_BYTES = 65536
ROLE_KEYS = frozenset({"EndpointId", "PeerIp", "StageRoot", "ManifestPath",
                       "PreflightPath", "FarmConfigPath", "CaptureConfigPath",
                       "TicketPath", "WorkflowPath", "CaptureControllerPath",
                       "JournalRoot", "ResultPath", "PackageRoot", "EvidenceRoot",
                       "RunRegistryRoot", "CaptureRoot", "PowerShell", "Supervisor",
                       "CaptureEngine", "ManifestSource", "PreflightSource"})
HOST_KEYS = frozenset({"HostIp", "Port", "JournalRoot", "ListeningPath"})


def Exact(Value, Keys, Name):
    if not isinstance(Value, dict) or set(Value) != set(Keys):
        raise ValueError("[Qualification:FarmTickets] invalid " + Name + " fields")
    return Value


def ReadJson(File):
    File = Path(File).resolve(strict=True)
    if not File.is_file() or File.stat().st_size > MAX_JSON_BYTES:
        raise ValueError("[Qualification:FarmTickets] missing or oversized JSON")
    return json.loads(File.read_text(encoding="utf-8"))


def Digest(File):
    with Path(File).open("rb") as Stream:
        return hashlib.file_digest(Stream, "sha256").hexdigest()


def UtcNow():
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def Fresh(Value, Seconds, Name):
    if not isinstance(Value, str):
        raise ValueError("[Qualification:FarmTickets] missing " + Name + " timestamp")
    Moment = datetime.fromisoformat(Value.replace("Z", "+00:00"))
    if Moment.tzinfo is None or not 0 <= (datetime.now(timezone.utc) - Moment).total_seconds() <= Seconds:
        raise ValueError("[Qualification:FarmTickets] stale or future " + Name)


def SourceFile(Value, Name):
    if not isinstance(Value, (str, Path)):
        raise ValueError("[Qualification:FarmTickets] invalid " + Name + " path")
    File = Path(Value).resolve(strict=True)
    if not File.is_file() or File.is_symlink() or File.stat().st_size > MAX_JSON_BYTES:
        raise ValueError("[Qualification:FarmTickets] unsafe " + Name + " source")
    return File


def WinPath(Value, Name):
    if not isinstance(Value, str) or len(Value) > 512 or not re.match(r"^[A-Za-z]:\\", Value):
        raise ValueError("[Qualification:FarmTickets] invalid absolute " + Name)
    PathValue = PureWindowsPath(Value)
    if (Value.startswith("\\\\") or any(Segment in (".", "..") for Segment in Value.split("\\")) or
            any(Character in Value[2:] for Character in (':', '"', "'", "\n", "\r", "\x00", '*', '?', '<', '>', '|'))):
        raise ValueError("[Qualification:FarmTickets] unsafe " + Name)
    return PathValue


def Contained(Child, Parent):
    Child = str(Child).casefold().rstrip("\\")
    Parent = str(Parent).casefold().rstrip("\\")
    return Child == Parent or Child.startswith(Parent + "\\")


def Pin(Value, Name, BaseName):
    Exact(Value, ("Path", "Sha256"), Name + " pin")
    File = WinPath(Value["Path"], Name)
    if File.name.lower() != BaseName.lower() or not isinstance(Value["Sha256"], str) or \
            not SHA256.fullmatch(Value["Sha256"]):
        raise ValueError("[Qualification:FarmTickets] invalid " + Name + " pin")
    return {"Path": str(File), "Sha256": Value["Sha256"]}


def WriteNew(File, Value):
    File = Path(File)
    Data = (json.dumps(Value, indent=2, sort_keys=True) + "\n").encode("utf-8")
    with File.open("xb") as Stream:
        Stream.write(Data)
    return File


def New(Root):
    Root = Path(Root).resolve()
    if Root.exists() or not Root.parent.is_dir() or Root.parent.is_symlink():
        raise ValueError("[Qualification:FarmTickets] private output root must be new")
    Root.mkdir(mode=0o700)
    WriteNew(Root / "identity.json", {"Format": FORMAT, "Version": 1,
             "CreatedUtc": UtcNow(), "RunId": str(uuid.uuid4()),
             "CoordinatorRunId": str(uuid.uuid4()),
             "ServerToken": secrets.token_hex(32), "ClientToken": secrets.token_hex(32)})
    print("[Qualification:FarmTickets] private run identity created; keep this root outside packages and evidence")


def ValidateRole(Role, Input, Identity, Manifest, ManifestHash, WorkflowHash):
    ExpectedKeys = ROLE_KEYS | ({"NodeRootCertificatePath"} if Manifest["Provider"] == "Node" else set())
    if Manifest["Provider"] == "Node" and Role == "SERVER":
        ExpectedKeys |= {"NodeStage", "NodeHelper"}
    Exact(Input, ExpectedKeys, Role + " role")
    if not ENDPOINT_ID.fullmatch(Input["EndpointId"]):
        raise ValueError("[Qualification:FarmTickets] invalid endpoint ID")
    ipaddress.IPv4Address(Input["PeerIp"])
    StageRoot = WinPath(Input["StageRoot"], "stage root")
    for Name in ("ManifestPath", "PreflightPath", "FarmConfigPath", "CaptureConfigPath",
                 "TicketPath", "WorkflowPath", "CaptureControllerPath"):
        if not Contained(WinPath(Input[Name], Name), StageRoot):
            raise ValueError("[Qualification:FarmTickets] staged file escapes endpoint root")
    Paths = [WinPath(Input[Name], Name) for Name in ("ManifestPath", "PreflightPath", "FarmConfigPath",
             "CaptureConfigPath", "TicketPath", "WorkflowPath", "CaptureControllerPath")]
    if len({str(Value).casefold() for Value in Paths}) != len(Paths):
        raise ValueError("[Qualification:FarmTickets] duplicate stage destination")
    for Name in ("JournalRoot", "ResultPath",
                 "PackageRoot", "EvidenceRoot", "RunRegistryRoot", "CaptureRoot"):
        WinPath(Input[Name], Name)
    for Name in ("PackageRoot", "EvidenceRoot", "RunRegistryRoot", "CaptureRoot"):
        if Contained(StageRoot, WinPath(Input[Name], Name)) or \
                Contained(WinPath(Input[Name], Name), StageRoot):
            raise ValueError("[Qualification:FarmTickets] staging overlaps package, evidence or capture")
    if Contained(WinPath(Input["CaptureRoot"], "capture"), WinPath(Input["EvidenceRoot"], "evidence")) or \
            Contained(WinPath(Input["EvidenceRoot"], "evidence"), WinPath(Input["CaptureRoot"], "capture")):
        raise ValueError("[Qualification:FarmTickets] capture and role evidence overlap")
    PowerShell = Pin(Input["PowerShell"], "PowerShell", "pwsh.exe")
    Supervisor = Pin(Input["Supervisor"], "supervisor", "PhysicalGameSessionFarmEndpoint.ps1")
    NodeStage = None
    NodeHelper = None
    if Manifest["Provider"] == "Node" and Role == "SERVER":
        NodeStage = Pin(Input["NodeStage"], "Node stage", "node-stage.json")
        NodeHelper = Pin(Input["NodeHelper"], "Node helper", "PhysicalGameSessionFarmNode.ps1")
        NodeRoot = PureWindowsPath(NodeStage["Path"]).parent
        for Name in ("StageRoot", "PackageRoot", "EvidenceRoot", "RunRegistryRoot", "CaptureRoot"):
            Boundary = WinPath(Input[Name], Name)
            if Contained(NodeRoot, Boundary) or Contained(Boundary, NodeRoot):
                raise ValueError("[Qualification:FarmTickets] Node stage overlaps campaign roots")
    EngineKeys = ("Service", "Hook") if Role == "SERVER" else ("CaptureScript", "Dumpcap")
    Engine = Exact(Input["CaptureEngine"], EngineKeys, "capture engine")
    Expected = {"Service": "AgentCoordinator.CaptureFarm32Service.exe", "Hook": "CaptureFarm32.ps1",
                "CaptureScript": "DumpcapFarm32Capture.ps1", "Dumpcap": "dumpcap.exe"}
    Pins = {Name: Pin(Engine[Name], Name, Expected[Name]) for Name in EngineKeys}
    ManifestFile = SourceFile(Input["ManifestSource"], "manifest")
    if Digest(ManifestFile) != ManifestHash or ReadJson(ManifestFile) != Manifest:
        raise ValueError("[Qualification:FarmTickets] role manifest bytes differ")
    PreflightFile = SourceFile(Input["PreflightSource"], "preflight")
    Preflight = ReadJson(PreflightFile)
    if (Preflight.get("Format") != "GargantuanPhysicalFarmPreflight" or Preflight.get("Version") != 1 or
            Preflight.get("RunId") != Identity["RunId"] or
            Preflight.get("Role") != ("Server" if Role == "SERVER" else "Clients") or
            Preflight.get("Provider") != Manifest["Provider"] or
            Preflight.get("ManifestSha256") != Digest(ManifestFile) or
            Preflight.get("Status") != "INVENTORY_ONLY"):
        raise ValueError("[Qualification:FarmTickets] preflight identity mismatch")
    Fresh(Preflight.get("ObservedUtc"), 120, "preflight")
    Farm = {"Role": Role, "CoordinatorRunId": Identity["CoordinatorRunId"],
            "RunId": Identity["RunId"], "SourceCommit": Manifest["SourceCommit"],
            "PowerShellPath": PowerShell["Path"], "PowerShellSHA256": PowerShell["Sha256"],
            "SupervisorPath": Supervisor["Path"], "SupervisorSHA256": Supervisor["Sha256"],
            "ManifestPath": Input["ManifestPath"], "ManifestSHA256": Digest(ManifestFile),
            "PackageRoot": Input["PackageRoot"], "EvidenceRoot": Input["EvidenceRoot"],
            "RunRegistryRoot": Input["RunRegistryRoot"]}
    if Manifest["Provider"] == "Node":
        Farm["NodeRootCertificatePath"] = str(WinPath(Input["NodeRootCertificatePath"], "Node root"))
    Capture = {"Format": "GargantuanFarm32CaptureCampaign", "Version": 1,
               "RunId": Identity["RunId"], "CoordinatorRunId": Identity["CoordinatorRunId"],
               "Role": Role, "CaptureRoot": Input["CaptureRoot"],
               "RoleEvidenceRoot": Input["EvidenceRoot"],
               "PowerShellPath": PowerShell["Path"], "PowerShellSha256": PowerShell["Sha256"]}
    for Name, Pinned in Pins.items():
        Capture[Name + "Path"] = Pinned["Path"]
        Capture[Name + "Sha256"] = Pinned["Sha256"]
    Ticket = {"Format": FORMAT, "Version": 1, "CreatedUtc": Identity["CreatedUtc"],
              "RunId": Identity["RunId"], "CoordinatorRunId": Identity["CoordinatorRunId"],
              "SourceCommit": Manifest["SourceCommit"], "Role": Role,
              "EndpointId": Input["EndpointId"], "PeerIp": Input["PeerIp"],
              "Token": Identity["ServerToken" if Role == "SERVER" else "ClientToken"],
              "WorkflowPath": Input["WorkflowPath"], "WorkflowSha256": WorkflowHash,
              "ManifestPath": Input["ManifestPath"], "ManifestSha256": Digest(ManifestFile),
              "FarmConfigPath": Input["FarmConfigPath"],
              "CaptureConfigPath": Input["CaptureConfigPath"],
              "CaptureControllerPath": Input["CaptureControllerPath"],
              "CaptureControllerSha256": Digest(Path(__file__).with_name("farm_capture_campaign.py")),
              "PreflightPath": Input["PreflightPath"], "PreflightSha256": Digest(PreflightFile),
              "JournalRoot": Input["JournalRoot"], "ResultPath": Input["ResultPath"]}
    if NodeStage is not None:
        Ticket.update({"NodeStagePath": NodeStage["Path"], "NodeStageSha256": NodeStage["Sha256"],
                       "NodeHelperPath": NodeHelper["Path"], "NodeHelperSha256": NodeHelper["Sha256"]})
    return Farm, Capture, Ticket, ManifestFile, PreflightFile


def Seal(Root, SpecPath):
    Root = Path(Root).resolve(strict=True)
    Identity = Exact(ReadJson(Root / "identity.json"),
                     ("Format", "Version", "CreatedUtc", "RunId", "CoordinatorRunId",
                      "ServerToken", "ClientToken"), "identity")
    if Identity["Format"] != FORMAT or Identity["Version"] != 1 or \
            any(str(uuid.UUID(Identity[Name])) != Identity[Name] for Name in ("RunId", "CoordinatorRunId")) or \
            Identity["RunId"] == Identity["CoordinatorRunId"] or \
            any(not SHA256.fullmatch(Identity[Name]) for Name in ("ServerToken", "ClientToken")) or \
            Identity["ServerToken"] == Identity["ClientToken"]:
        raise ValueError("[Qualification:FarmTickets] invalid run identity")
    Fresh(Identity["CreatedUtc"], 3600, "identity")
    Spec = Exact(ReadJson(SpecPath), ("Format", "Version", "ManifestSource", "Host", "Roles"), "spec")
    if Spec["Format"] != "GargantuanFarm32TicketSpec" or Spec["Version"] != 1:
        raise ValueError("[Qualification:FarmTickets] unsupported spec")
    Host = Exact(Spec["Host"], HOST_KEYS, "host")
    Roles = Exact(Spec["Roles"], ("SERVER", "CLIENT"), "roles")
    ipaddress.IPv4Address(Host["HostIp"])
    if type(Host["Port"]) is not int or not 1 <= Host["Port"] <= 65535:
        raise ValueError("[Qualification:FarmTickets] invalid control port")
    for Name in ("JournalRoot", "ListeningPath"):
        WinPath(Host[Name], Name)
    ManifestFile = SourceFile(Spec["ManifestSource"], "manifest")
    Manifest = ReadJson(ManifestFile)
    if (Manifest.get("Format") != "GargantuanPhysicalFarmEndpoint" or Manifest.get("Version") != 1 or
            Manifest.get("RunId") != Identity["RunId"] or not COMMIT.fullmatch(Manifest.get("SourceCommit", "")) or
            Manifest.get("Provider") not in ("Local", "Node") or
            not isinstance(Manifest.get("Nonces"), list) or len(Manifest["Nonces"]) != 32):
        raise ValueError("[Qualification:FarmTickets] manifest identity mismatch")
    WorkflowFile = SourceFile(Path(__file__).parent / "workflows" / "thirty-two-client-farm-lifecycle.json",
                              "workflow")
    WorkflowHash = Digest(WorkflowFile)
    Built = {}
    for Role in ("SERVER", "CLIENT"):
        Built[Role] = ValidateRole(Role, Roles[Role], Identity, Manifest, Digest(ManifestFile), WorkflowHash)
        Built[Role][2]["CoordinatorHost"] = Host["HostIp"]
        Built[Role][2]["Port"] = Host["Port"]
    if Roles["SERVER"]["EndpointId"] == Roles["CLIENT"]["EndpointId"]:
        raise ValueError("[Qualification:FarmTickets] duplicate endpoint identity")
    Destinations = []
    for Role in ("SERVER", "CLIENT"):
        Destinations += [str(WinPath(Roles[Role][Name], Name)).casefold() for Name in
                         ("ManifestPath", "PreflightPath", "FarmConfigPath", "CaptureConfigPath", "TicketPath")]
    if len(set(Destinations)) != len(Destinations):
        raise ValueError("[Qualification:FarmTickets] duplicate cross-role destination")
    PrivatePath = WinPath(str(Root), "private root")
    for Role in ("SERVER", "CLIENT"):
        for Name in ("PackageRoot", "EvidenceRoot", "RunRegistryRoot", "CaptureRoot"):
            Boundary = WinPath(Roles[Role][Name], Name)
            if Contained(PrivatePath, Boundary) or Contained(Boundary, PrivatePath):
                raise ValueError("[Qualification:FarmTickets] private tickets overlap application roots")
    if any((Root / Name).exists() for Name in ("SERVER", "CLIENT", "host-config.json", "copy-plan.json")):
        raise ValueError("[Qualification:FarmTickets] private run already sealed")
    Plan = {"Format": "GargantuanFarm32CopyPlan", "Version": 1,
            "RunId": Identity["RunId"], "CoordinatorRunId": Identity["CoordinatorRunId"],
            "State": "FILES_ONLY_UNEXECUTED", "Files": []}

    def Add(Role, Name, Source, Destination):
        Plan["Files"].append({"Endpoint": Role, "Name": Name, "Source": str(Source),
                              "Destination": str(WinPath(Destination, Name)), "Sha256": Digest(Source)})

    for Role in ("SERVER", "CLIENT"):
        Directory = Root / Role
        Directory.mkdir(mode=0o700)
        Config = Roles[Role]
        Farm, Capture, Ticket, RoleManifest, Preflight = Built[Role]
        FarmFile = WriteNew(Directory / "farm-config.json", Farm)
        CaptureFile = WriteNew(Directory / "capture-config.json", Capture)
        Ticket["FarmConfigSha256"] = Digest(FarmFile)
        Ticket["CaptureConfigSha256"] = Digest(CaptureFile)
        TicketFile = WriteNew(Directory / "ticket.json", Ticket)
        for Name, Source, Destination in (("manifest", RoleManifest, Config["ManifestPath"]),
                                          ("preflight", Preflight, Config["PreflightPath"]),
                                          ("farm-config", FarmFile, Config["FarmConfigPath"]),
                                          ("capture-config", CaptureFile, Config["CaptureConfigPath"]),
                                          ("ticket", TicketFile, Config["TicketPath"]),
                                          ("workflow", WorkflowFile, Config["WorkflowPath"]),
                                          ("capture-controller", Path(__file__).with_name("farm_capture_campaign.py"),
                                           Config["CaptureControllerPath"])):
            Add(Role, Name, Source, Destination)
    HostConfig = {"Format": FORMAT, "Version": 1, "CreatedUtc": Identity["CreatedUtc"],
                  "RunId": Identity["RunId"], "CoordinatorRunId": Identity["CoordinatorRunId"],
                  "SourceCommit": Manifest["SourceCommit"], "WorkflowPath": str(WorkflowFile),
                  "WorkflowSha256": WorkflowHash, "ManifestPath": str(ManifestFile),
                  "ManifestSha256": Digest(ManifestFile), "HostIp": Host["HostIp"],
                  "Port": Host["Port"], "Roles": {}, "JournalRoot": Host["JournalRoot"],
                  "ListeningPath": Host["ListeningPath"]}
    for Role in ("SERVER", "CLIENT"):
        TicketFile = Root / Role / "ticket.json"
        HostConfig["Roles"][Role] = {"TicketPath": str(TicketFile), "TicketSha256": Digest(TicketFile)}
    WriteNew(Root / "host-config.json", HostConfig)
    WriteNew(Root / "copy-plan.json", Plan)
    print("[Qualification:FarmTickets] fixed file-copy plan sealed; no endpoint operation performed")


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("Action", choices=("new", "seal"))
    Parser.add_argument("PrivateRoot")
    Parser.add_argument("SpecPath", nargs="?")
    Args = Parser.parse_args()
    if Args.Action == "new" and Args.SpecPath is None:
        New(Args.PrivateRoot)
    elif Args.Action == "seal" and Args.SpecPath is not None:
        Seal(Args.PrivateRoot, Args.SpecPath)
    else:
        Parser.error("new takes only a private root; seal requires a spec path")


if __name__ == "__main__":
    try:
        Main()
    except (ValueError, OSError, KeyError, TypeError, json.JSONDecodeError) as Error:
        print("[Qualification:FarmTickets] " + str(Error), file=sys.stderr)
        raise SystemExit(1)
