"""Fixed two-host staging and control for one already-packaged Farm32 run.

The input spec is sealed by farm_ticket_staging after fresh role preflights. This
layer copies only its 14 fixed members, stages the pinned local coordinator
library, checks private endpoint roots and exact hashes, proves the worker's
normal-LAN control path, and starts the fixed host/role entrypoints. It does
not grant a physical PASS; offline reconciliation remains separate.
"""

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path, PureWindowsPath
import re
import shutil
import socket
import subprocess
import sys
import time

from private_ticket_acl import AssertPrivate
import farm_ticket_staging as Tickets
from dependency import GetRoot


SOURCE = Path(__file__).resolve().parent
WORKER_ALIAS = "dockerbox"
WORKER_IP = "192.168.0.108"
CLIENT_IP = "192.168.0.68"
CONTROL_PORT = 39451
SHA = re.compile(r"[0-9a-f]{64}\Z")
SAFE_REMOTE = re.compile(r"^[A-Za-z]:\\[A-Za-z0-9._\\-]{1,350}\Z")
SAFE_ARGUMENT = re.compile(r"[A-Za-z0-9._:\\-]{1,512}\Z")
ROLE_FILES = ("farm_campaign_runner.py", "farm_lifecycle.py", "dependency.py",
              "upstream.lock.json")


def Digest(File):
    with Path(File).open("rb") as Stream:
        return hashlib.file_digest(Stream, "sha256").hexdigest()


def ReadJson(File):
    File = Path(File).resolve(strict=True)
    if not File.is_file() or File.stat().st_size > 65536:
        raise ValueError("[Qualification:FarmOuter] invalid bounded JSON input")
    return json.loads(File.read_text(encoding="utf-8"))


def WriteNew(File, Row):
    File = Path(File)
    with File.open("xb") as Stream:
        Stream.write((json.dumps(Row, indent=2, sort_keys=True) + "\n").encode("utf-8"))
    return File


def RemoteText(Value):
    if not isinstance(Value, str) or not SAFE_REMOTE.fullmatch(Value) or ".." in Value.split("\\"):
        raise ValueError("[Qualification:FarmOuter] unsafe fixed worker path")
    return Value


def WorkerSandbox(Value):
    Value = RemoteText(Value)
    if not Value.lower().startswith("c:\\sandbox\\codex\\"):
        raise ValueError("[Qualification:FarmOuter] worker path is outside task sandbox")
    return Value


def WindowsScpPath(Value):
    return WORKER_ALIAS + ":" + RemoteText(Value).replace("\\", "/")


def RemoteArgument(Value):
    if not isinstance(Value, str) or not SAFE_ARGUMENT.fullmatch(Value):
        raise ValueError("[Qualification:FarmOuter] unsafe remote argument")
    return Value


def Hidden():
    return subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0


def Checked(Arguments, Timeout=20):
    return subprocess.run(Arguments, check=True, capture_output=True, text=True,
                          timeout=Timeout, creationflags=Hidden())


class Transport:
    def __init__(self, WorkerPython, WorkerHelper):
        self.WorkerPython = RemoteText(WorkerPython)
        self.WorkerHelper = RemoteText(WorkerHelper)

    def Remote(self, *Arguments, Timeout=20):
        return Checked(["ssh", "-o", "BatchMode=yes", WORKER_ALIAS,
                        self.WorkerPython, "-B", self.WorkerHelper,
                        *(RemoteArgument(Item) for Item in Arguments)], Timeout)

    def RemoteStart(self, *Arguments):
        return subprocess.Popen(["ssh", "-o", "BatchMode=yes", WORKER_ALIAS,
                                 self.WorkerPython, "-B", self.WorkerHelper,
                                 *(RemoteArgument(Item) for Item in Arguments)],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                creationflags=Hidden())

    def RemoteCopy(self, SourceFile, Destination):
        Checked(["scp", "-q", str(SourceFile), WindowsScpPath(Destination)], 20)

    def RemoteTree(self, SourceDir, Destination):
        Checked(["scp", "-q", "-r", str(SourceDir), WindowsScpPath(Destination)], 45)

    def Fetch(self, Source, Destination, Timeout=10):
        Checked(["scp", "-q", WindowsScpPath(Source), str(Destination)], Timeout)

    def WorkerPowerShell(self, Executable, Script, *Arguments, Timeout=120):
        Executable = str(Tickets.WinPath(Executable, "worker PowerShell"))
        Script = RemoteText(Script)
        Args = ["-NoProfile", "-NonInteractive", "-File", Script,
                *(RemoteArgument(Item) for Item in Arguments)]
        Invocation = "& '" + Executable + "' " + " ".join("'" + Item + "'" for Item in Args)
        Encoded = base64.b64encode(("$ErrorActionPreference='Stop';" + Invocation +
                                    ";exit $LASTEXITCODE").encode("utf-16le")).decode("ascii")
        return Checked(["ssh", "-o", "BatchMode=yes", WORKER_ALIAS, "pwsh.exe",
                        "-NoProfile", "-NonInteractive", "-EncodedCommand", Encoded], Timeout)

    def WorkerDigest(self, File):
        File = str(Tickets.WinPath(File, "worker hash target"))
        Script = ("$ErrorActionPreference='Stop';"
                  "(Get-FileHash -LiteralPath '" + File +
                  "' -Algorithm SHA256).Hash.ToLowerInvariant()")
        Encoded = base64.b64encode(Script.encode("utf-16le")).decode("ascii")
        Result = Checked(["ssh", "-o", "BatchMode=yes", WORKER_ALIAS, "pwsh.exe",
                          "-NoProfile", "-NonInteractive", "-EncodedCommand", Encoded], 20)
        Value = Result.stdout.strip().lower()
        if not SHA.fullmatch(Value):
            raise ValueError("[Qualification:FarmOuter] worker hash proof unavailable")
        return Value

    def MakeWorkerToolRoot(self, Root):
        Root = WorkerSandbox(Root)
        Script = ("$ErrorActionPreference='Stop';"
                  "$Root='" + Root + "';"
                  "if(Test-Path -LiteralPath $Root){throw 'stale worker tool root'};"
                  "$Parent=Split-Path -Parent $Root;"
                  "if(-not(Test-Path -LiteralPath $Parent -PathType Container)){throw 'missing worker tool parent'};"
                  "[void](New-Item -ItemType Directory -Path $Root -ErrorAction Stop);"
                  "$Sid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value;"
                  "& icacls.exe $Root /grant:r ('*'+$Sid+':(OI)(CI)F') '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F' | Out-Null;"
                  "if($LASTEXITCODE -ne 0){throw 'worker tool ACL grant failed'};"
                  "& icacls.exe $Root /inheritance:r | Out-Null;"
                  "if($LASTEXITCODE -ne 0){throw 'worker tool ACL inheritance failed'}")
        Encoded = base64.b64encode(Script.encode("utf-16le")).decode("ascii")
        Checked(["ssh", "-o", "BatchMode=yes", WORKER_ALIAS, "pwsh.exe",
                 "-NoProfile", "-NonInteractive", "-EncodedCommand", Encoded], 20)


def ExpandRun(Value, RunId):
    if isinstance(Value, str):
        return Value.replace("{RunId}", RunId)
    if isinstance(Value, list):
        return [ExpandRun(Item, RunId) for Item in Value]
    if isinstance(Value, dict):
        return {Name: ExpandRun(Item, RunId) for Name, Item in Value.items()}
    return Value


def PrepareInputs(ConfigPath, TransportInstance=None):
    """Create fresh one-run manifest and role-local inventory, then seal tickets.

    Both package deployment manifests are checked by the canonical manifest
    creator on the worker. The client preflight independently checks its local
    Player package. No capture or GameSession process is started here.
    """
    Config = ReadJson(ConfigPath)
    Required = {"Format", "Version", "Provider", "SourceCommit", "PrivateRoot",
                "TemplatePath", "WorkerToolRoot", "WorkerPlayerPackageRoot",
                "WorkerPython", "WorkerPythonSha256", "WorkerHelper", "Node"}
    if (not isinstance(Config, dict) or set(Config) != Required or
            Config["Format"] != "GargantuanFarm32OuterPreparation" or Config["Version"] != 1 or
            Config["Provider"] not in ("Local", "Node") or
            not re.fullmatch(r"[0-9a-f]{40}", Config["SourceCommit"])):
        raise ValueError("[Qualification:FarmOuter] invalid preparation config")
    WorkerToolRoot = WorkerSandbox(Config["WorkerToolRoot"])
    TransportInstance = TransportInstance or Transport(Config["WorkerPython"], Config["WorkerHelper"])
    ExpectedHelper = str(PureWindowsPath(WorkerToolRoot) / "farm_outer_endpoint.py")
    if (Config["WorkerHelper"] != ExpectedHelper or
            not isinstance(Config["WorkerPythonSha256"], str) or
            not SHA.fullmatch(Config["WorkerPythonSha256"]) or
            TransportInstance.WorkerDigest(Config["WorkerPython"]) != Config["WorkerPythonSha256"]):
        raise ValueError("[Qualification:FarmOuter] worker runtime pin mismatch")
    TransportInstance.MakeWorkerToolRoot(WorkerToolRoot)
    for Name in ("farm_outer_endpoint.py", "private_ticket_acl.py"):
        Destination = str(PureWindowsPath(WorkerToolRoot) / Name)
        TransportInstance.RemoteCopy(SOURCE / Name, Destination)
        if TransportInstance.WorkerDigest(Destination) != Digest(SOURCE / Name):
            raise ValueError("[Qualification:FarmOuter] worker helper source hash mismatch")
    Tickets.New(Config["PrivateRoot"])
    Private = Path(Config["PrivateRoot"]).resolve(strict=True)
    Identity = ReadJson(Private / "identity.json")
    WriteNew(Private / "runtime-pins.json", {"Format": "GargantuanFarm32RuntimePins",
        "Version": 1, "WorkerPythonPath": Config["WorkerPython"],
        "WorkerPythonSha256": Config["WorkerPythonSha256"],
        "WorkerHelperPath": Config["WorkerHelper"],
        "WorkerHelperSha256": Digest(SOURCE / "farm_outer_endpoint.py"),
        "ClientPythonPath": sys.executable, "ClientPythonSha256": Digest(sys.executable)})
    RunId = Identity["RunId"]
    Spec = ExpandRun(ReadJson(Config["TemplatePath"]), RunId)
    if (Spec.get("Format") != "GargantuanFarm32TicketSpec" or Spec.get("Version") != 1 or
            Spec.get("Host", {}).get("HostIp") != WORKER_IP or
            Spec.get("Host", {}).get("Port") != CONTROL_PORT):
        raise ValueError("[Qualification:FarmOuter] worker-host ticket template mismatch")
    WorkerPowerShell = Spec["Roles"]["SERVER"]["PowerShell"]
    if TransportInstance.WorkerDigest(WorkerPowerShell["Path"]) != WorkerPowerShell["Sha256"]:
        raise ValueError("[Qualification:FarmOuter] worker PowerShell pin mismatch")
    WorkerRunRoot = str(PureWindowsPath(WorkerToolRoot) / RunId)
    TransportInstance.Remote("prepare", RemoteText(WorkerRunRoot))
    for Name in ("NewPhysicalGameSessionFarmManifest.ps1",
                 "PhysicalGameSessionFarmPreflight.ps1",
                 "PhysicalGameSessionFarmEndpoint.ps1"):
        SourceFile = SOURCE.parent.parent / "tests" / Name
        Destination = str(PureWindowsPath(WorkerRunRoot) / Name)
        TransportInstance.RemoteCopy(SourceFile, Destination)
        if TransportInstance.Remote("digest", RemoteText(Destination)).stdout.strip() != Digest(SourceFile):
            raise ValueError("[Qualification:FarmOuter] worker preparation source hash mismatch")
    ManifestScript = str(PureWindowsPath(WorkerRunRoot) / "NewPhysicalGameSessionFarmManifest.ps1")
    ManifestRoot = str(PureWindowsPath(WorkerRunRoot) / "manifest")
    ManifestArgs = ["-ServerPackageRoot", WorkerSandbox(Spec["Roles"]["SERVER"]["PackageRoot"]),
                    "-PlayerPackageRoot", WorkerSandbox(Config["WorkerPlayerPackageRoot"]),
                    "-OutputRoot", RemoteText(ManifestRoot), "-RunId", RunId,
                    "-Endpoint", "10.253.3.2:39450", "-Provider", Config["Provider"],
                    "-ScaleWorkload", "-ClientFrames", "9000", "-ServerTicks", "10000"]
    if Config["Provider"] == "Node":
        Node = Config["Node"]
        if not isinstance(Node, dict) or set(Node) != {"Endpoint", "RootCertificatePath",
                                                      "TokenEnvironment", "HelperPath", "StageRoot",
                                                      "DescriptorPath", "DescriptorSha256", "ExecutablePath",
                                                      "ExecutableSha256", "SourceCommit", "CertificatePath",
                                                      "PrivateKeyPath"}:
            raise ValueError("[Qualification:FarmOuter] invalid Node preparation")
        ManifestArgs += ["-NodeEndpoint", Node["Endpoint"],
                         "-NodeRootCertificatePath", WorkerSandbox(Node["RootCertificatePath"]),
                         "-NodeTokenEnvironment", Node["TokenEnvironment"]]
    elif Config["Node"] is not None:
        raise ValueError("[Qualification:FarmOuter] Local campaign has Node preparation")
    TransportInstance.WorkerPowerShell(WorkerPowerShell["Path"], ManifestScript,
                                       *ManifestArgs, Timeout=180)
    RemoteManifest = str(PureWindowsPath(ManifestRoot) / "Server" / "run-manifest.json")
    RemoteClientManifest = str(PureWindowsPath(ManifestRoot) / "Clients" / "run-manifest.json")
    LocalManifest = Private / "run-manifest.json"
    TransportInstance.Fetch(RemoteManifest, LocalManifest)
    ManifestHash = Digest(LocalManifest)
    if (TransportInstance.Remote("digest", RemoteText(RemoteClientManifest)).stdout.strip() != ManifestHash or
            ReadJson(LocalManifest).get("RunId") != RunId or
            ReadJson(LocalManifest).get("SourceCommit") != Config["SourceCommit"] or
            ReadJson(LocalManifest).get("ScaleWorkload") is not True or
            ReadJson(LocalManifest).get("ClientFrames", 0) < 9000 or
            ReadJson(LocalManifest).get("ServerTicks", 0) <= ReadJson(LocalManifest).get("ClientFrames", 0) + 600):
        raise ValueError("[Qualification:FarmOuter] generated package manifest mismatch")
    if Config["Provider"] == "Node":
        Node = Config["Node"]
        Helper = RemoteText(Node["HelperPath"])
        if (WorkerSandbox(Helper) != Helper or
                TransportInstance.WorkerDigest(Helper) != Digest(
                    SOURCE.parent.parent / "tests" / "PhysicalGameSessionFarmNode.ps1")):
            raise ValueError("[Qualification:FarmOuter] Node helper source pin changed")
        if not SHA.fullmatch(Node["DescriptorSha256"]) or not SHA.fullmatch(Node["ExecutableSha256"]):
            raise ValueError("[Qualification:FarmOuter] invalid Node source pins")
        TransportInstance.WorkerPowerShell(WorkerPowerShell["Path"], Helper,
            "-Mode", "Prepare", "-StageRoot",
            WorkerSandbox(ExpandRun(Node["StageRoot"], RunId)), "-RunManifestPath", RemoteManifest,
            "-RunManifestSha256", ManifestHash, "-ServerPackageRoot",
            WorkerSandbox(Spec["Roles"]["SERVER"]["PackageRoot"]), "-DescriptorPath",
            WorkerSandbox(Node["DescriptorPath"]), "-DescriptorSha256", Node["DescriptorSha256"],
            "-NodeExecutablePath", WorkerSandbox(Node["ExecutablePath"]),
            "-NodeExecutableSha256", Node["ExecutableSha256"],
            "-NodeSourceCommit", Node["SourceCommit"], "-CertificatePath",
            WorkerSandbox(Node["CertificatePath"]), "-PrivateKeyPath",
            WorkerSandbox(Node["PrivateKeyPath"]), "-RootCertificatePath",
            WorkerSandbox(Node["RootCertificatePath"]), Timeout=40)
        StagePath = str(PureWindowsPath(ExpandRun(Node["StageRoot"], RunId)) / "node-stage.json")
        StageHash = TransportInstance.Remote("digest", RemoteText(StagePath)).stdout.strip()
        if not SHA.fullmatch(StageHash):
            raise ValueError("[Qualification:FarmOuter] Node stage hash unavailable")
        Spec["Roles"]["SERVER"]["NodeStage"] = {"Path": StagePath, "Sha256": StageHash}
        Spec["Roles"]["SERVER"]["NodeHelper"] = {"Path": Helper,
            "Sha256": TransportInstance.Remote("digest", Helper).stdout.strip()}
    PreflightScript = str(PureWindowsPath(WorkerRunRoot) / "PhysicalGameSessionFarmPreflight.ps1")
    ServerPreflightRemote = str(PureWindowsPath(WorkerRunRoot) / "server-preflight.json")
    Server = Spec["Roles"]["SERVER"]
    ServerArgs = ["-Role", "Server", "-ManifestPath", RemoteManifest,
                  "-ManifestSha256", ManifestHash, "-PackageRoot", Server["PackageRoot"],
                  "-EvidenceRoot", Server["EvidenceRoot"],
                  "-RunRegistryRoot", Server["RunRegistryRoot"],
                  "-ReportPath", ServerPreflightRemote]
    if Config["Provider"] == "Node":
        ServerArgs += ["-NodeRootCertificatePath", Spec["Roles"]["SERVER"]["NodeRootCertificatePath"]]
    TransportInstance.WorkerPowerShell(WorkerPowerShell["Path"], PreflightScript,
                                       *ServerArgs, Timeout=45)
    ServerPreflight = Private / "server-preflight.json"
    TransportInstance.Fetch(ServerPreflightRemote, ServerPreflight)
    Client = Spec["Roles"]["CLIENT"]
    ClientPreflight = Private / "client-preflight.json"
    PowerShell = Path(Client["PowerShell"]["Path"])
    if Digest(PowerShell) != Client["PowerShell"]["Sha256"]:
        raise ValueError("[Qualification:FarmOuter] client PowerShell pin changed")
    Checked([str(PowerShell), "-NoProfile", "-NonInteractive", "-File",
             str(SOURCE.parent.parent / "tests" / "PhysicalGameSessionFarmPreflight.ps1"),
             "-Role", "Clients", "-ManifestPath", str(LocalManifest),
             "-ManifestSha256", ManifestHash, "-PackageRoot", Client["PackageRoot"],
             "-EvidenceRoot", Client["EvidenceRoot"],
             "-RunRegistryRoot", Client["RunRegistryRoot"],
             "-ReportPath", str(ClientPreflight)], 45)
    Spec["ManifestSource"] = str(LocalManifest)
    Server["ManifestSource"] = str(LocalManifest)
    Client["ManifestSource"] = str(LocalManifest)
    Server["PreflightSource"] = str(ServerPreflight)
    Client["PreflightSource"] = str(ClientPreflight)
    SealedSpec = WriteNew(Private / "sealed-spec.json", Spec)
    Tickets.Seal(Private, SealedSpec)
    return Private, SealedSpec


def VerifyRuntimePins(PrivateRoot, WorkerPython, WorkerHelper, TransportInstance):
    Pins = ReadJson(Path(PrivateRoot) / "runtime-pins.json")
    if (set(Pins) != {"Format", "Version", "WorkerPythonPath", "WorkerPythonSha256",
                      "WorkerHelperPath", "WorkerHelperSha256",
                      "ClientPythonPath", "ClientPythonSha256"} or
            Pins["Format"] != "GargantuanFarm32RuntimePins" or Pins["Version"] != 1 or
            Pins["WorkerPythonPath"] != WorkerPython or Pins["WorkerHelperPath"] != WorkerHelper or
            Pins["ClientPythonPath"] != sys.executable or
            Pins["ClientPythonSha256"] != Digest(sys.executable) or
            Pins["WorkerHelperSha256"] != Digest(SOURCE / "farm_outer_endpoint.py") or
            TransportInstance.WorkerDigest(WorkerPython) != Pins["WorkerPythonSha256"] or
            TransportInstance.WorkerDigest(WorkerHelper) != Pins["WorkerHelperSha256"]):
        raise ValueError("[Qualification:FarmOuter] worker launch runtime pin changed")


def Stage(PrivateRoot, SpecPath, WorkerPython, WorkerHelper, TransportInstance=None):
    """Stage a freshly sealed ticket plan; never infer paths from remote data."""
    AssertPrivate(PrivateRoot)
    GetRoot()  # Every upstream source member must match the repository lock.
    PrivateRoot = Path(PrivateRoot).resolve(strict=True)
    Spec = ReadJson(SpecPath)
    Plan = ReadJson(PrivateRoot / "copy-plan.json")
    Identity = ReadJson(PrivateRoot / "identity.json")
    if (Plan.get("Format") != "GargantuanFarm32CopyPlan" or Plan.get("Version") != 1 or
            Plan.get("State") != "FILES_ONLY_UNEXECUTED" or
            Plan.get("RunId") != Identity.get("RunId") or
            len(Plan.get("Files", [])) != 14 or
            Spec.get("Host", {}).get("HostIp") != WORKER_IP or
            Spec.get("Host", {}).get("Port") != CONTROL_PORT):
        raise ValueError("[Qualification:FarmOuter] sealed plan or worker-host profile mismatch")
    TransportInstance = TransportInstance or Transport(WorkerPython, WorkerHelper)
    VerifyRuntimePins(PrivateRoot, WorkerPython, WorkerHelper, TransportInstance)
    Roots = {Role: Spec["Roles"][Role]["StageRoot"] for Role in ("SERVER", "CLIENT")}
    WorkerSandbox(Roots["SERVER"])
    for Role, Root in Roots.items():
        if Role == "SERVER":
            TransportInstance.Remote("prepare", RemoteText(Root))
        else:
            from farm_outer_endpoint import Prepare
            Prepare(Root)
    Entries = {Role: [] for Role in Roots}

    def Copy(Role, SourceFile, Destination, Name=None):
        Root = PureWindowsPath(Roots[Role])
        Target = PureWindowsPath(Destination)
        if Target.parent != Root or Target.name in {Entry["Name"] for Entry in Entries[Role]}:
            raise ValueError("[Qualification:FarmOuter] copy destination is not a unique stage member")
        SourceFile = Path(SourceFile).resolve(strict=True)
        if Role == "SERVER":
            TransportInstance.RemoteCopy(SourceFile, Destination)
        else:
            TargetPath = Path(Destination)
            with SourceFile.open("rb") as SourceStream, TargetPath.open("xb") as TargetStream:
                shutil.copyfileobj(SourceStream, TargetStream)
        Entries[Role].append({"Name": Target.name, "Sha256": Digest(SourceFile)})

    for Row in Plan["Files"]:
        if not isinstance(Row, dict) or set(Row) != {"Endpoint", "Name", "Source", "Destination", "Sha256"} or \
                Row["Endpoint"] not in Roots or not SHA.fullmatch(Row["Sha256"]) or \
                Digest(Row["Source"]) != Row["Sha256"]:
            raise ValueError("[Qualification:FarmOuter] fixed copy plan member changed")
        Copy(Row["Endpoint"], Row["Source"], Row["Destination"])
    for Role in Roots:
        for Name in ROLE_FILES:
            Copy(Role, SOURCE / Name, str(PureWindowsPath(Roots[Role]) / Name))
        if Role == "SERVER":
            TransportInstance.RemoteTree(SOURCE / ".agent-coordinator",
                                         str(PureWindowsPath(Roots[Role]) / ".agent-coordinator"))
        else:
            shutil.copytree(SOURCE / ".agent-coordinator", Path(Roots[Role]) / ".agent-coordinator")
    Host = ReadJson(PrivateRoot / "host-config.json")
    if Host["RunId"] != Identity["RunId"] or Host["HostIp"] != WORKER_IP:
        raise ValueError("[Qualification:FarmOuter] host identity mismatch")
    Host["Roles"]["SERVER"]["TicketPath"] = Spec["Roles"]["SERVER"]["TicketPath"]
    Host["Roles"]["CLIENT"]["TicketPath"] = str(PureWindowsPath(Roots["SERVER"]) / "host-client-ticket.json")
    Host["WorkflowPath"] = Spec["Roles"]["SERVER"]["WorkflowPath"]
    Host["ManifestPath"] = Spec["Roles"]["SERVER"]["ManifestPath"]
    LocalHost = WriteNew(PrivateRoot / "worker-host-config.json", Host)
    Copy("SERVER", PrivateRoot / "CLIENT" / "ticket.json", Host["Roles"]["CLIENT"]["TicketPath"])
    Copy("SERVER", LocalHost, str(PureWindowsPath(Roots["SERVER"]) / "host-config.json"))
    for Role in Roots:
        Root = PureWindowsPath(Roots[Role])
        LaunchFiles = (("role", "ticket.json"), ("host", "host-config.json")) if Role == "SERVER" else \
                      (("role", "ticket.json"),)
        for Action, InputName in LaunchFiles:
            InputFile = PrivateRoot / Role / "ticket.json" if InputName == "ticket.json" else LocalHost
            Launch = {"Format": "GargantuanFarm32FixedLaunch", "Version": 1,
                      "Action": Action, "RunnerName": "farm_campaign_runner.py",
                      "RunnerSha256": Digest(SOURCE / "farm_campaign_runner.py"),
                      "InputName": InputName, "InputSha256": Digest(InputFile)}
            File = WriteNew(PrivateRoot / (Role.lower() + "-" + Action + "-launch.json"), Launch)
            Copy(Role, File, str(Root / (Action + "-launch.json")))
        Index = WriteNew(PrivateRoot / (Role.lower() + "-stage-index.json"),
                         {"Format": "GargantuanFarm32StageIndex", "Version": 1,
                          "Files": Entries[Role]})
        Destination = str(Root / "stage-index.json")
        if Role == "SERVER":
            TransportInstance.RemoteCopy(Index, Destination)
            TransportInstance.Remote("verify", RemoteText(Roots[Role]), RemoteText(Destination))
        else:
            shutil.copyfile(Index, Path(Destination))
            from farm_outer_endpoint import Verify
            Verify(Roots[Role], Destination)
    return Roots


def ControlProbe(TransportInstance, RunId):
    Process = TransportInstance.RemoteStart("probe", WORKER_IP, str(CONTROL_PORT), RunId)
    try:
        Deadline = time.monotonic() + 10
        while True:
            try:
                with socket.create_connection((WORKER_IP, CONTROL_PORT), timeout=0.5) as Connection:
                    Connection.sendall(RunId.encode("ascii"))
                    if Connection.recv(128) != RunId.encode("ascii"):
                        raise ValueError("[Qualification:FarmOuter] control probe challenge mismatch")
                    break
            except (ConnectionRefusedError, TimeoutError):
                if Process.poll() is not None or time.monotonic() >= Deadline:
                    raise TimeoutError("[Qualification:FarmOuter] worker control listener unreachable")
                time.sleep(0.05)
        if Process.wait(timeout=3) != 0:
            raise RuntimeError("[Qualification:FarmOuter] worker control probe failed")
    finally:
        if Process.poll() is None:
            Process.terminate()


def RequireFreshPreflights(PrivateRoot, Identity):
    for Role in ("SERVER", "CLIENT"):
        Ticket = ReadJson(Path(PrivateRoot) / Role / "ticket.json")
        Preflight = Path(PrivateRoot) / ("server-preflight.json" if Role == "SERVER" else
                                          "client-preflight.json")
        if (Ticket.get("RunId") != Identity["RunId"] or
                Ticket.get("PreflightSha256") != Digest(Preflight)):
            raise ValueError("[Qualification:FarmOuter] role preflight pin changed")
        Row = ReadJson(Preflight)
        if Row.get("RunId") != Identity["RunId"] or Row.get("Status") != "INVENTORY_ONLY":
            raise ValueError("[Qualification:FarmOuter] role preflight identity changed")
        Tickets.Fresh(Row.get("ObservedUtc"), 120, "role preflight")


def Launch(PrivateRoot, SpecPath, WorkerPython, WorkerHelper, TransportInstance=None):
    """Start a sealed worker host and two roles; never start before control proof."""
    AssertPrivate(PrivateRoot)
    PrivateRoot = Path(PrivateRoot).resolve(strict=True)
    Spec = ReadJson(SpecPath)
    Identity = ReadJson(PrivateRoot / "identity.json")
    Roots = {Role: Spec["Roles"][Role]["StageRoot"] for Role in ("SERVER", "CLIENT")}
    TransportInstance = TransportInstance or Transport(WorkerPython, WorkerHelper)
    VerifyRuntimePins(PrivateRoot, WorkerPython, WorkerHelper, TransportInstance)
    RequireFreshPreflights(PrivateRoot, Identity)
    for Role in Roots:
        Index = str(PureWindowsPath(Roots[Role]) / "stage-index.json")
        if Role == "SERVER":
            TransportInstance.Remote("verify", RemoteText(Roots[Role]), RemoteText(Index))
        else:
            from farm_outer_endpoint import Verify
            Verify(Roots[Role], Index)
    BarrierStart = time.monotonic()
    ControlProbe(TransportInstance, Identity["RunId"])
    ProbeElapsed = time.monotonic() - BarrierStart
    Root = RemoteText(Roots["SERVER"])
    Index = RemoteText(str(PureWindowsPath(Root) / "stage-index.json"))
    HostConfig = RemoteText(str(PureWindowsPath(Root) / "host-launch.json"))
    Host = TransportInstance.RemoteStart("host", Root, Index, HostConfig)
    Roles = []
    try:
        Marker = Spec["Host"]["ListeningPath"]
        LocalMarker = PrivateRoot / "worker-listening.json"
        Deadline = time.monotonic() + 20
        while not LocalMarker.is_file():
            if Host.poll() is not None or time.monotonic() >= Deadline:
                raise TimeoutError("[Qualification:FarmOuter] same-run host listener missing")
            try:
                TransportInstance.Fetch(Marker, LocalMarker)
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
                time.sleep(0.05)
        Seen = ReadJson(LocalMarker)
        if (Seen.get("RunId") != Identity["RunId"] or
                Seen.get("CoordinatorRunId") != Identity["CoordinatorRunId"] or
                Seen.get("HostIp") != WORKER_IP or Seen.get("Port") != CONTROL_PORT or
                Seen.get("State") != "LISTENING_UNQUALIFIED" or Host.poll() is not None):
            raise ValueError("[Qualification:FarmOuter] host marker identity mismatch")
        BarrierElapsed = time.monotonic() - BarrierStart
        if BarrierElapsed >= 60:
            raise TimeoutError("[Qualification:FarmOuter] control barrier exceeded 60 seconds")
        RequireFreshPreflights(PrivateRoot, Identity)
        WriteNew(PrivateRoot / "control-barrier.json", {
            "Format": "GargantuanFarm32ControlBarrier", "Version": 1,
            "RunId": Identity["RunId"], "CoordinatorRunId": Identity["CoordinatorRunId"],
            "HostIp": WORKER_IP, "Port": CONTROL_PORT,
            "ProbeElapsedMilliseconds": round(ProbeElapsed * 1000),
            "HostReadyElapsedMilliseconds": round(BarrierElapsed * 1000),
            "State": "CONTROL_ONLY_LISTENING_UNQUALIFIED"})
        Barrier = time.monotonic()
        ServerLaunch = RemoteText(str(PureWindowsPath(Root) / "role-launch.json"))
        Roles.append(TransportInstance.RemoteStart("role", Root, Index, ServerLaunch))
        ClientRoot = Path(Roots["CLIENT"]).resolve(strict=True)
        # A local hidden child keeps the main controller free to observe both roles.
        Roles.append(subprocess.Popen([sys.executable, "-B", str(SOURCE / "farm_outer_endpoint.py"),
                                       "role", str(ClientRoot), str(ClientRoot / "stage-index.json"),
                                       str(ClientRoot / "role-launch.json")],
                                      cwd=str(SOURCE), stdout=subprocess.DEVNULL,
                                      stderr=subprocess.DEVNULL, creationflags=Hidden()))
        if time.monotonic() - Barrier >= 60:
            raise TimeoutError("[Qualification:FarmOuter] role launch barrier exceeded")
        Deadline = time.monotonic() + 850
        for Process in Roles:
            Remaining = max(0.1, Deadline - time.monotonic())
            if Process.wait(timeout=Remaining) != 0:
                raise RuntimeError("[Qualification:FarmOuter] role failed; preserve evidence")
        if Host.wait(timeout=max(0.1, Deadline - time.monotonic())) != 0:
            raise RuntimeError("[Qualification:FarmOuter] coordinator failed; preserve evidence")
    finally:
        for Process in [*Roles, Host]:
            if Process.poll() is None:
                Process.terminate()
    return 0


def FetchIndexed(TransportInstance, RemoteRoot, LocalRoot, IndexName, RunId, Role,
                 MaximumMembers, MaximumMemberBytes):
    """Copy a sealed worker directory and independently verify every member."""
    LocalRoot = Path(LocalRoot)
    if LocalRoot.exists() or not LocalRoot.parent.is_dir():
        raise ValueError("[Qualification:FarmOuter] stale collection directory")
    LocalRoot.mkdir(mode=0o700)
    Index = LocalRoot / IndexName
    TransportInstance.Fetch(str(PureWindowsPath(RemoteRoot) / IndexName), Index)
    Row = ReadJson(Index)
    ExpectedRole = Role if IndexName == "capture-sha256.json" else \
                   ("Server" if Role == "SERVER" else "Clients")
    if (Row.get("RunId") != RunId or Row.get("Role") != ExpectedRole or
            not isinstance(Row.get("Files"), list) or
            not 1 <= len(Row["Files"]) <= MaximumMembers):
        raise ValueError("[Qualification:FarmOuter] indexed worker evidence identity mismatch")
    Seen = set()
    for Member in Row["Files"]:
        if (not isinstance(Member, dict) or
                set(Member) != {"Name", "Bytes", "Sha256"} or
                not isinstance(Member["Name"], str) or
                not re.fullmatch(r"[A-Za-z0-9._-]{1,96}", Member["Name"]) or
                Member["Name"] in Seen or Member["Name"] == IndexName or
                type(Member["Bytes"]) is not int or
                not 0 <= Member["Bytes"] <= MaximumMemberBytes or
                not isinstance(Member["Sha256"], str) or
                not SHA.fullmatch(Member["Sha256"])):
            raise ValueError("[Qualification:FarmOuter] indexed worker evidence member invalid")
        Seen.add(Member["Name"])
        Destination = LocalRoot / Member["Name"]
        TransportInstance.Fetch(str(PureWindowsPath(RemoteRoot) / Member["Name"]),
                                Destination, Timeout=180)
        if Destination.stat().st_size != Member["Bytes"] or Digest(Destination) != Member["Sha256"]:
            raise ValueError("[Qualification:FarmOuter] transferred worker evidence hash mismatch")
    return Index


def Collect(PrivateRoot, SpecPath, WorkerPython, WorkerHelper, TransportInstance=None):
    """Bind typed role/capture results; Node log match is an independent gate."""
    AssertPrivate(PrivateRoot)
    PrivateRoot = Path(PrivateRoot).resolve(strict=True)
    Spec = ReadJson(SpecPath)
    Identity = ReadJson(PrivateRoot / "identity.json")
    Manifest = ReadJson(PrivateRoot / "run-manifest.json")
    if Manifest.get("RunId") != Identity["RunId"] or Manifest.get("Provider") not in ("Local", "Node"):
        raise ValueError("[Qualification:FarmOuter] collection identity mismatch")
    TransportInstance = TransportInstance or Transport(WorkerPython, WorkerHelper)
    VerifyRuntimePins(PrivateRoot, WorkerPython, WorkerHelper, TransportInstance)
    Server = Spec["Roles"]["SERVER"]
    Client = Spec["Roles"]["CLIENT"]
    Collection = PrivateRoot / "collection"
    Collection.mkdir(mode=0o700)
    ServerRole = FetchIndexed(TransportInstance, Server["EvidenceRoot"],
                              Collection / "server-role", "evidence-sha256.json",
                              Identity["RunId"], "SERVER", 128, 16 * 1024 * 1024)
    ServerCapture = FetchIndexed(TransportInstance,
        str(PureWindowsPath(Server["CaptureRoot"]) / Identity["RunId"]),
        Collection / "server-capture", "capture-sha256.json",
        Identity["RunId"], "SERVER", 20, 960 * 1024 * 1024)
    ClientRole = Path(Client["EvidenceRoot"]) / "evidence-sha256.json"
    ClientCapture = Path(Client["CaptureRoot"]) / Identity["RunId"] / "capture-sha256.json"
    if not ClientRole.is_file() or not ClientCapture.is_file():
        raise ValueError("[Qualification:FarmOuter] client evidence is missing")
    Coordinator = Collection / "coordinator-result.json"
    TransportInstance.Fetch(str(PureWindowsPath(Spec["Host"]["JournalRoot"]) / "result.json"),
                            Coordinator)
    if ReadJson(Coordinator).get("RunId") != Identity["CoordinatorRunId"]:
        raise ValueError("[Qualification:FarmOuter] coordinator result identity mismatch")
    Analysis = PrivateRoot / "analysis"
    Analysis.mkdir(mode=0o700)
    Reconcile = {"Format": "GargantuanFarm32Campaign", "Version": 1,
        "RunId": Identity["RunId"], "CoordinatorRunId": Identity["CoordinatorRunId"],
        "CoordinatorResultPath": str(Coordinator), "CoordinatorResultSha256": Digest(Coordinator),
        "ServerRoleIndexPath": str(ServerRole), "ServerRoleIndexSha256": Digest(ServerRole),
        "ClientRoleIndexPath": str(ClientRole), "ClientRoleIndexSha256": Digest(ClientRole),
        "ServerCaptureIndexPath": str(ServerCapture),
        "ServerCaptureIndexSha256": Digest(ServerCapture),
        "ClientCaptureIndexPath": str(ClientCapture),
        "ClientCaptureIndexSha256": Digest(ClientCapture),
        "DirectionAnalyzerPath": str(SOURCE / "farm_capture_directions.py"),
        "DirectionAnalyzerSha256": Digest(SOURCE / "farm_capture_directions.py"),
        "CaptureBinderPath": str(SOURCE / "farm_capture_campaign.py"),
        "CaptureBinderSha256": Digest(SOURCE / "farm_capture_campaign.py"),
        "OuterReceiptPath": str(Analysis / "outer-receipt.json"),
        "DirectionReportPath": str(Analysis / "directions.json"),
        "ResultPath": str(Analysis / "reconcile-result.json")}
    if Manifest["Provider"] == "Node":
        ServerTicket = ReadJson(PrivateRoot / "SERVER" / "ticket.json")
        StagePath = Path(ServerTicket["NodeStagePath"])
        NodeRoot = StagePath.parent
        if (StagePath.name != "node-stage.json" or NodeRoot.exists() or
                str(NodeRoot).lower().find(Identity["RunId"]) < 0 or
                not str(NodeRoot).lower().startswith("c:\\sandbox\\codex\\artifacts\\")):
            raise ValueError("[Qualification:FarmOuter] unsafe Node receipt mirror root")
        NodeRoot.mkdir(parents=True)
        RunPath = NodeRoot / "node-run.json"
        TransportInstance.Fetch(ServerTicket["NodeStagePath"], StagePath)
        if Digest(StagePath) != ServerTicket["NodeStageSha256"]:
            raise ValueError("[Qualification:FarmOuter] Node stage transfer changed")
        TransportInstance.Fetch(str(PureWindowsPath(ServerTicket["NodeStagePath"]).parent /
                                    "node-run.json"), RunPath)
        NodeRun = ReadJson(RunPath)
        ServerResult = ReadJson(ServerRole.parent / "result.json")
        if (ServerResult.get("NodeStageSha256") != ServerTicket["NodeStageSha256"] or
                ServerResult.get("NodeRunReceiptSha256") != Digest(RunPath)):
            raise ValueError("[Qualification:FarmOuter] Node run is not bound by server role")
        for Name in ("Stdout", "Stderr"):
            RemoteLog = NodeRun.get(Name + "Path")
            if not isinstance(RemoteLog, str) or \
                    Path(RemoteLog) != NodeRoot / ("node." + Name.lower() + ".log") or \
                    type(NodeRun.get(Name + "Bytes")) is not int or \
                    not 0 <= NodeRun[Name + "Bytes"] <= 8 * 1024 * 1024 or \
                    not isinstance(NodeRun.get(Name + "Sha256"), str) or \
                    not SHA.fullmatch(NodeRun[Name + "Sha256"]):
                raise ValueError("[Qualification:FarmOuter] Node log receipt path or bound invalid")
            TransportInstance.Fetch(RemoteLog, Path(RemoteLog), Timeout=30)
            if (Path(RemoteLog).stat().st_size != NodeRun[Name + "Bytes"] or
                    Digest(RemoteLog) != NodeRun[Name + "Sha256"]):
                raise ValueError("[Qualification:FarmOuter] Node log transfer changed")
        Provider = ServerRole.parent / "node-provider.json"
        if not Provider.is_file():
            raise ValueError("[Qualification:FarmOuter] server Node provider receipt absent")
        ClientPowerShell = Client["PowerShell"]
        Reconcile.update({
            "NodePowerShellPath": ClientPowerShell["Path"],
            "NodePowerShellSha256": ClientPowerShell["Sha256"],
            "NodeTlsMatcherPath": str(SOURCE.parent.parent / "tests" /
                                       "PhysicalGameSessionFarmNodeTls.ps1"),
            "NodeTlsMatcherSha256": Digest(SOURCE.parent.parent / "tests" /
                                            "PhysicalGameSessionFarmNodeTls.ps1"),
            "ServerNodeProviderReceiptPath": str(Provider),
            "ServerNodeProviderReceiptSha256": Digest(Provider),
            "NodeStagePath": str(StagePath), "NodeStageSha256": Digest(StagePath),
            "NodeRunReceiptPath": str(RunPath), "NodeRunReceiptSha256": Digest(RunPath),
            "NodeTlsMatchReceiptPath": str(Analysis / "node-tls-match.json")})
    ConfigFile = WriteNew(PrivateRoot / "reconcile-config.json", Reconcile)
    from farm_campaign_runner import Reconcile as Bind
    Bind(ConfigFile)
    Result = ReadJson(Analysis / "reconcile-result.json")
    if Result.get("Status") != "CAPTURE_DIRECTIONS_MEASURED" or \
            Result.get("Foundation3LGate") != "INCOMPLETE" or \
            Result.get("ProviderGate") != "NOT_MEASURED":
        raise ValueError("[Qualification:FarmOuter] offline result classification changed")
    return Analysis / "reconcile-result.json"


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("Action", choices=("prepare", "stage", "launch", "collect"))
    Parser.add_argument("Path")
    Parser.add_argument("SpecPath", nargs="?")
    Parser.add_argument("WorkerPython", nargs="?")
    Parser.add_argument("WorkerHelper", nargs="?")
    Args = Parser.parse_args()
    if Args.Action == "prepare" and Args.SpecPath is None:
        PrepareInputs(Args.Path)
    elif Args.Action == "stage" and all((Args.SpecPath, Args.WorkerPython, Args.WorkerHelper)):
        Stage(Args.Path, Args.SpecPath, Args.WorkerPython, Args.WorkerHelper)
    elif Args.Action == "launch" and all((Args.SpecPath, Args.WorkerPython, Args.WorkerHelper)):
        Launch(Args.Path, Args.SpecPath, Args.WorkerPython, Args.WorkerHelper)
    elif Args.Action == "collect" and all((Args.SpecPath, Args.WorkerPython, Args.WorkerHelper)):
        Collect(Args.Path, Args.SpecPath, Args.WorkerPython, Args.WorkerHelper)
    else:
        Parser.error("prepare needs a preparation config; stage/launch need private root, spec and worker runtime")


if __name__ == "__main__":
    try:
        raise SystemExit(Main())
    except (ValueError, RuntimeError, TimeoutError, OSError, subprocess.SubprocessError) as Error:
        print(str(Error), file=sys.stderr)
        raise SystemExit(1)
