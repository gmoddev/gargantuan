"""Fixed two-host staging and control for one already-packaged Farm32 run.

The input spec is sealed by farm_ticket_staging after fresh role preflights. This
layer copies only its fixed declared members, stages the pinned local coordinator
library, checks private endpoint roots and exact hashes, proves the worker's
normal-LAN control path, and starts the fixed host/role entrypoints. It does
not grant a physical PASS; offline reconciliation remains separate.
"""

import argparse
import base64
from datetime import datetime, timedelta, timezone
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
import uuid

from private_ticket_acl import AssertPrivate
import farm_ticket_staging as Tickets
from dependency import GetRoot


SOURCE = Path(__file__).resolve().parent
WORKER_ALIAS = "dockerbox"
WORKER_IP = "192.168.0.108"
CLIENT_IP = "192.168.0.68"
CONTROL_PORT = 39451
CAPTURE_MEMBER_MAX_BYTES = 15 * 1024 * 1024 * 1024
CAPTURE_TRANSFER_SECONDS = 1800  # Per immutable ETL/pcap; independent of live capture.
OUTER_FINISH_SECONDS = 3050
SHA = re.compile(r"[0-9a-f]{64}\Z")
SAFE_REMOTE = re.compile(r"^[A-Za-z]:\\[A-Za-z0-9._\\-]{1,350}\Z")
SAFE_ARGUMENT = re.compile(r"[A-Za-z0-9._:\\-]{1,512}\Z")
ROLE_FILES = ("farm_campaign_runner.py", "farm_lifecycle.py", "private_ticket_acl.py", "dependency.py",
              "upstream.lock.json", "farm_capture_directions.py")


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
                                    ";$Succeeded=$?;"
                                    "if(-not $Succeeded -or $LASTEXITCODE -isnot [int]){exit 1};"
                                    "exit [int]$LASTEXITCODE").encode("utf-16le")).decode("ascii")
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

    def AddControlFirewall(self, RunId):
        if not re.fullmatch(r"[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}", RunId):
            raise ValueError("[Qualification:FarmOuter] invalid control firewall run identity")
        Name = "Codex-Gargantuan-Farm32-Control-" + RunId
        Script = ("$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue';"
                  "$Name='" + Name + "';$Program='" + self.WorkerPython + "';"
                  "$Added=$false;try{"
                  "if(Get-NetFirewallRule -Name $Name -ErrorAction SilentlyContinue){"
                  "throw 'stale task-owned control rule'};"
                  "New-NetFirewallRule -Name $Name -DisplayName $Name -Direction Inbound "
                  "-Action Allow -Protocol TCP -LocalPort 39451 -LocalAddress 192.168.0.108 "
                  "-RemoteAddress 192.168.0.68 -Program $Program -Profile Private "
                  "-Enabled True | Out-Null;$Added=$true;"
                  "if(-not(Get-NetFirewallRule -Name $Name -ErrorAction Stop)){"
                  "throw 'control rule did not persist'}"
                  "}catch{if($Added){Get-NetFirewallRule -Name $Name -ErrorAction SilentlyContinue|"
                  "Remove-NetFirewallRule -ErrorAction SilentlyContinue};throw}")
        Encoded = base64.b64encode(Script.encode("utf-16le")).decode("ascii")
        Checked(["ssh", "-o", "BatchMode=yes", WORKER_ALIAS, "pwsh.exe",
                 "-NoProfile", "-NonInteractive", "-EncodedCommand", Encoded], 20)

    def RemoveControlFirewall(self, RunId):
        if not re.fullmatch(r"[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}", RunId):
            raise ValueError("[Qualification:FarmOuter] invalid control firewall run identity")
        Name = "Codex-Gargantuan-Farm32-Control-" + RunId
        Script = ("$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue';"
                  "$Name='" + Name + "';$Rule=Get-NetFirewallRule -Name $Name -ErrorAction Stop;"
                  "$Rule|Remove-NetFirewallRule -ErrorAction Stop;"
                  "if(Get-NetFirewallRule -Name $Name -ErrorAction SilentlyContinue){"
                  "throw 'task-owned control rule remained'}")
        Encoded = base64.b64encode(Script.encode("utf-16le")).decode("ascii")
        Checked(["ssh", "-o", "BatchMode=yes", WORKER_ALIAS, "pwsh.exe",
                 "-NoProfile", "-NonInteractive", "-EncodedCommand", Encoded], 20)

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


def WorkloadProfile(RecoveryWorkload):
    if type(RecoveryWorkload) is not bool:
        raise ValueError("[Qualification:FarmOuter] invalid recovery workload selection")
    # PlayerHost tests its frame ceiling before its 16,667-us cadence sleep.
    # Cover the entire existing role-local wall-clock budget, including that
    # first unslept frame; do not terminate a healthy run at the old 150/300 s
    # frame limits while its 300/420 s supervisor budget is still available.
    RunMicroseconds = (420000 if RecoveryWorkload else 300000) * 1000
    ClientFrames = (RunMicroseconds + 16666) // 16667 + 1
    ServerTicks = ClientFrames + 1000  # Existing bounded setup/terminal allowance.
    Arguments = ["-ScaleWorkload", "-ClientFrames", str(ClientFrames),
                 "-ServerTicks", str(ServerTicks)]
    if RecoveryWorkload:
        Arguments.append("-RecoveryWorkload")
    return ClientFrames, ServerTicks, Arguments


def PrepareRuntimeProjections(TransportInstance, WorkerPowerShell, WorkerRunRoot, Spec, Manifest):
    """Prepare exact native closures once, before inventory preflights or ARM."""
    HelperName = "NewPhysicalGameSessionFarmProjection.ps1"
    Helper = SOURCE.parent.parent / "tests" / HelperName
    RemoteHelper = str(PureWindowsPath(WorkerRunRoot) / HelperName)
    Client = Spec["Roles"]["CLIENT"]
    PowerShell = Path(Client["PowerShell"]["Path"])
    if Digest(PowerShell) != Client["PowerShell"]["Sha256"]:
        raise ValueError("[Qualification:FarmOuter] client projection PowerShell pin changed")
    for Role, Label, Prefix in (("SERVER", "Server", "Server"), ("CLIENT", "Clients", "Player")):
        Input = Spec["Roles"][Role]
        Args = ["-PackageRoot", Input["PackageRoot"], "-RunRegistryRoot", Input["RunRegistryRoot"],
                "-RunId", Manifest["RunId"], "-Role", Label,
                "-DeploymentSha256", Manifest[Prefix + "DeploymentSha256"],
                "-SourceCommit", Manifest["SourceCommit"]]
        if Role == "SERVER":
            TransportInstance.WorkerPowerShell(WorkerPowerShell, RemoteHelper, *Args, Timeout=180)
        else:
            Checked([str(PowerShell), "-NoProfile", "-NonInteractive", "-File", str(Helper), *Args], 180)


def PrepareInputsOnce(ConfigPath, TransportInstance=None, AttemptId=None):
    """Create fresh one-run manifest and role-local inventory, then seal tickets.

    Both package deployment manifests are checked by the canonical manifest
    creator on the worker. The client preflight independently checks its local
    Player package. No capture or GameSession process is started here.
    """
    Config = ReadJson(ConfigPath)
    Required = {"Format", "Version", "Provider", "RecoveryWorkload", "SourceCommit", "PrivateRoot",
                "TemplatePath", "WorkerToolRoot", "WorkerPlayerPackageRoot",
                "WorkerPython", "WorkerPythonSha256", "WorkerHelper", "Node"}
    if (not isinstance(Config, dict) or set(Config) != Required or
            Config["Format"] != "GargantuanFarm32OuterPreparation" or Config["Version"] != 1 or
            Config["Provider"] not in ("Local", "Node") or
            type(Config["RecoveryWorkload"]) is not bool or
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
    if Config["Provider"] == "Node":
        WriteNew(Private / "node-preparation-attempt.json", {
            "Format": "GargantuanFarmNodePreparationAttempt", "Version": 1,
            "RunId": Identity["RunId"], "AttemptId": AttemptId})
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
    for Role in ("SERVER", "CLIENT"):
        Timeout = Spec["Roles"][Role].get("RunTimeoutMilliseconds")
        if Config["RecoveryWorkload"]:
            if type(Timeout) is not int or Timeout != 420000:
                raise ValueError("[Qualification:FarmOuter] recovery role timeout mismatch")
        elif Timeout is not None and (type(Timeout) is not int or Timeout != 300000):
            raise ValueError("[Qualification:FarmOuter] baseline role timeout mismatch")
    WorkerPowerShell = Spec["Roles"]["SERVER"]["PowerShell"]
    if TransportInstance.WorkerDigest(WorkerPowerShell["Path"]) != WorkerPowerShell["Sha256"]:
        raise ValueError("[Qualification:FarmOuter] worker PowerShell pin mismatch")
    WorkerRunRoot = str(PureWindowsPath(WorkerToolRoot) / RunId)
    TransportInstance.Remote("prepare", RemoteText(WorkerRunRoot))
    NodeTokenPath = str(PureWindowsPath(WorkerRunRoot) / "node-token.secret")
    NodeTokenSha256 = None
    for Name in ("NewPhysicalGameSessionFarmManifest.ps1",
                 "NewPhysicalGameSessionFarmProjection.ps1",
                 "NewPhysicalGameSessionFarmNodeTls.ps1",
                 "PhysicalGameSessionFarmPreflight.ps1",
                 "PhysicalGameSessionFarmEndpoint.ps1"):
        SourceFile = SOURCE.parent.parent / "tests" / Name
        Destination = str(PureWindowsPath(WorkerRunRoot) / Name)
        TransportInstance.RemoteCopy(SourceFile, Destination)
        if TransportInstance.Remote("digest", RemoteText(Destination)).stdout.strip() != Digest(SourceFile):
            raise ValueError("[Qualification:FarmOuter] worker preparation source hash mismatch")
    ManifestScript = str(PureWindowsPath(WorkerRunRoot) / "NewPhysicalGameSessionFarmManifest.ps1")
    ManifestRoot = str(PureWindowsPath(WorkerRunRoot) / "manifest")
    ClientFrames, ServerTicks, WorkloadArguments = WorkloadProfile(Config["RecoveryWorkload"])
    ManifestArgs = ["-ServerPackageRoot", WorkerSandbox(Spec["Roles"]["SERVER"]["PackageRoot"]),
                    "-PlayerPackageRoot", WorkerSandbox(Config["WorkerPlayerPackageRoot"]),
                    "-OutputRoot", RemoteText(ManifestRoot), "-RunId", RunId,
                    "-Endpoint", "10.253.3.2:39450", "-Provider", Config["Provider"],
                    *WorkloadArguments]
    if Config["Provider"] == "Node":
        Node = Config["Node"]
        BaseKeys = {"Endpoint", "TokenEnvironment", "HelperPath", "StageRoot",
                    "DescriptorPath", "DescriptorSha256", "ExecutablePath",
                    "ExecutableSha256", "SourceCommit", "GoExecutablePath",
                    "GoExecutableSha256"}
        OldCertificateKeys = {"RootCertificatePath", "CertificatePath", "PrivateKeyPath"}
        if not isinstance(Node, dict) or set(Node) not in (BaseKeys, BaseKeys | OldCertificateKeys):
            raise ValueError("[Qualification:FarmOuter] invalid Node preparation")
        NodeTlsRoot = str(PureWindowsPath(WorkerRunRoot) / "node-root-ca.pem")
        NodeTlsCertificate = str(PureWindowsPath(WorkerRunRoot) / "node-cert.pem")
        NodeTlsKey = str(PureWindowsPath(WorkerRunRoot) / "node-key.pem")
        if OldCertificateKeys <= set(Node) and any(
                WorkerSandbox(ExpandRun(Node[Name], RunId)) != Expected for Name, Expected in (
                    ("RootCertificatePath", NodeTlsRoot), ("CertificatePath", NodeTlsCertificate),
                    ("PrivateKeyPath", NodeTlsKey))):
            raise ValueError("[Qualification:FarmOuter] Node certificate inputs must use the one-run root")
        Generator = str(PureWindowsPath(WorkerRunRoot) / "NewPhysicalGameSessionFarmNodeTls.ps1")
        TransportInstance.WorkerPowerShell(WorkerPowerShell["Path"], Generator,
                                           "-RunRoot", WorkerRunRoot, "-RunId", RunId, Timeout=45)
        NodeTlsReceipt = Private / "node-tls-stage.json"
        RemoteNodeTlsReceipt = str(PureWindowsPath(WorkerRunRoot) / "node-tls-stage.json")
        NodeTlsReceiptSha256 = TransportInstance.Remote("digest", RemoteText(RemoteNodeTlsReceipt)).stdout.strip()
        if not SHA.fullmatch(NodeTlsReceiptSha256):
            raise ValueError("[Qualification:FarmOuter] Node TLS stage pin unavailable")
        TransportInstance.Fetch(RemoteNodeTlsReceipt, NodeTlsReceipt)
        if Digest(NodeTlsReceipt) != NodeTlsReceiptSha256:
            raise ValueError("[Qualification:FarmOuter] Node TLS stage transfer changed")
        NodeTls = ReadJson(NodeTlsReceipt)
        ExpectedPaths = {"RootCertificatePath": NodeTlsRoot,
                         "CertificatePath": NodeTlsCertificate, "PrivateKeyPath": NodeTlsKey}
        if (NodeTls.get("Format") != "GargantuanFarmNodeTlsStage" or NodeTls.get("Version") != 1 or
                NodeTls.get("RunId") != RunId or NodeTls.get("Status") != "GENERATED_NOT_TLS_PROVEN" or
                any(NodeTls.get(Name) != Path for Name, Path in ExpectedPaths.items()) or
                any(not isinstance(NodeTls.get(Name + "Sha256"), str) or
                    not SHA.fullmatch(NodeTls[Name + "Sha256"]) or
                    TransportInstance.Remote("digest", RemoteText(Path)).stdout.strip() !=
                    NodeTls[Name + "Sha256"] for Name, Path in ExpectedPaths.items()) or
                datetime.fromisoformat(NodeTls["NotAfterUtc"]) <=
                datetime.now(timezone.utc) + timedelta(minutes=20)):
            raise ValueError("[Qualification:FarmOuter] generated Node TLS identity pin mismatch")
        LocalNodeRoot = Private / "node-root-ca.pem"
        TransportInstance.Fetch(NodeTlsRoot, LocalNodeRoot)
        if Digest(LocalNodeRoot) != NodeTls["RootCertificateSha256"]:
            raise ValueError("[Qualification:FarmOuter] client Node trust root copy changed")
        LocalNodeCertificate = Private / "node-cert.pem"
        TransportInstance.Fetch(NodeTlsCertificate, LocalNodeCertificate)
        if Digest(LocalNodeCertificate) != NodeTls["CertificateSha256"]:
            raise ValueError("[Qualification:FarmOuter] public Node certificate copy changed")
        Spec["Roles"]["SERVER"]["NodeRootCertificatePath"] = NodeTlsRoot
        Spec["Roles"]["CLIENT"]["NodeRootCertificatePath"] = str(LocalNodeRoot)
        ManifestArgs += ["-NodeEndpoint", Node["Endpoint"],
                         "-NodeRootCertificatePath", NodeTlsRoot,
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
            ReadJson(LocalManifest).get("RecoveryWorkload", False) is not Config["RecoveryWorkload"] or
            ReadJson(LocalManifest).get("ClientFrames") != ClientFrames or
            ReadJson(LocalManifest).get("ServerTicks") != ServerTicks):
        raise ValueError("[Qualification:FarmOuter] generated package manifest mismatch")
    if Config["Provider"] == "Node":
        Node = Config["Node"]
        Helper = RemoteText(Node["HelperPath"])
        if (WorkerSandbox(Helper) != Helper or
                TransportInstance.WorkerDigest(Helper) != Digest(
                    SOURCE.parent.parent / "tests" / "PhysicalGameSessionFarmNode.ps1")):
            raise ValueError("[Qualification:FarmOuter] Node helper source pin changed")
        if (not SHA.fullmatch(Node["DescriptorSha256"]) or
                not SHA.fullmatch(Node["ExecutableSha256"]) or
                not SHA.fullmatch(Node["GoExecutableSha256"]) or
                TransportInstance.WorkerDigest(WorkerSandbox(Node["GoExecutablePath"])) !=
                Node["GoExecutableSha256"].lower()):
            raise ValueError("[Qualification:FarmOuter] invalid Node source pins")
        TransportInstance.Remote("new-node-token", RemoteText(WorkerRunRoot))
        NodeTokenSha256 = TransportInstance.Remote("digest", RemoteText(NodeTokenPath)).stdout.strip()
        if not SHA.fullmatch(NodeTokenSha256):
            raise ValueError("[Qualification:FarmOuter] Node token pin unavailable")
        TransportInstance.WorkerPowerShell(WorkerPowerShell["Path"], Helper,
            "-Mode", "Prepare", "-StageRoot",
            WorkerSandbox(ExpandRun(Node["StageRoot"], RunId)), "-RunManifestPath", RemoteManifest,
            "-RunManifestSha256", ManifestHash, "-ServerPackageRoot",
            WorkerSandbox(Spec["Roles"]["SERVER"]["PackageRoot"]), "-DescriptorPath",
            WorkerSandbox(Node["DescriptorPath"]), "-DescriptorSha256", Node["DescriptorSha256"],
            "-NodeExecutablePath", WorkerSandbox(Node["ExecutablePath"]),
            "-NodeExecutableSha256", Node["ExecutableSha256"],
            "-NodeSourceCommit", Node["SourceCommit"],
            "-GoExecutablePath", WorkerSandbox(Node["GoExecutablePath"]),
            "-GoExecutableSha256", Node["GoExecutableSha256"], "-CertificatePath",
            NodeTlsCertificate, "-PrivateKeyPath",
            NodeTlsKey, "-RootCertificatePath",
            NodeTlsRoot, "-NodeTokenFilePath",
            NodeTokenPath, "-NodeTokenFileSha256", NodeTokenSha256, Timeout=40)
        StagePath = str(PureWindowsPath(ExpandRun(Node["StageRoot"], RunId)) / "node-stage.json")
        StageHash = TransportInstance.Remote("digest", RemoteText(StagePath)).stdout.strip()
        if not SHA.fullmatch(StageHash):
            raise ValueError("[Qualification:FarmOuter] Node stage hash unavailable")
        Spec["Roles"]["SERVER"]["NodeStage"] = {"Path": StagePath, "Sha256": StageHash}
        Spec["Roles"]["SERVER"]["NodeHelper"] = {"Path": Helper,
            "Sha256": TransportInstance.Remote("digest", Helper).stdout.strip()}
    PrepareRuntimeProjections(TransportInstance, WorkerPowerShell["Path"], WorkerRunRoot, Spec, ReadJson(LocalManifest))
    PreflightScript = str(PureWindowsPath(WorkerRunRoot) / "PhysicalGameSessionFarmPreflight.ps1")
    ServerPreflightRemote = str(PureWindowsPath(WorkerRunRoot) / "server-preflight.json")
    Server = Spec["Roles"]["SERVER"]
    ServerArgs = ["-Role", "Server", "-ManifestPath", RemoteManifest,
                  "-ManifestSha256", ManifestHash, "-PackageRoot", Server["PackageRoot"],
                  "-EvidenceRoot", Server["EvidenceRoot"],
                  "-RunRegistryRoot", Server["RunRegistryRoot"],
                  "-ReportPath", ServerPreflightRemote]
    if Config["Provider"] == "Node":
        ServerArgs += ["-NodeRootCertificatePath", Spec["Roles"]["SERVER"]["NodeRootCertificatePath"],
                       "-NodeTokenFilePath", NodeTokenPath,
                       "-NodeTokenFileSha256", NodeTokenSha256]
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


def PrepareInputs(ConfigPath, TransportInstance=None):
    AttemptId = uuid.uuid4().hex
    try:
        return PrepareInputsOnce(ConfigPath, TransportInstance, AttemptId)
    except Exception:
        try:
            Config = ReadJson(ConfigPath)
            if Config.get("Provider") == "Node":
                Private = Path(Config["PrivateRoot"])
                IdentityFile = Private / "identity.json"
                if IdentityFile.is_file():
                    RunId = ReadJson(IdentityFile)["RunId"]
                    Marker = Private / "node-preparation-attempt.json"
                    if Marker.is_file():
                        if ReadJson(Marker) != {
                                "Format": "GargantuanFarmNodePreparationAttempt", "Version": 1,
                                "RunId": RunId, "AttemptId": AttemptId}:
                            raise ValueError("[Qualification:FarmOuter] failed preparation is not this attempt")
                        WorkerToolRoot = WorkerSandbox(Config["WorkerToolRoot"])
                        Helper = str(PureWindowsPath(WorkerToolRoot) / "farm_outer_endpoint.py")
                        WorkerTransport = TransportInstance or Transport(Config["WorkerPython"], Helper)
                        if Config["WorkerHelper"] != Helper or \
                                WorkerTransport.WorkerDigest(Helper) != Digest(SOURCE / "farm_outer_endpoint.py"):
                            raise ValueError("[Qualification:FarmOuter] worker cleanup helper pin changed")
                        RunRoot = str(PureWindowsPath(WorkerToolRoot) / RunId)
                        try:
                            WorkerTransport.Remote("retire-node-token", RemoteText(RunRoot))
                        finally:
                            WorkerTransport.Remote("retire-node-tls", RemoteText(RunRoot))
        except Exception as CleanupError:
            raise RuntimeError("[Qualification:FarmOuter] preparation failed; Node secret retirement not proven") \
                from CleanupError
        raise


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
    # CaptureRoot is a fresh parent owned by staging; the capture controller
    # owns its RunId child and the native farm owns RoleEvidenceRoot later.
    for Role in Roots:
        CaptureRoot = Tickets.WinPath(Spec["Roles"][Role]["CaptureRoot"], "capture root")
        CaptureConfig = ReadJson(PrivateRoot / Role / "capture-config.json")
        if (CaptureConfig.get("RunId") != Identity["RunId"] or
                CaptureConfig.get("CoordinatorRunId") != Identity["CoordinatorRunId"] or
                CaptureConfig.get("Role") != Role or
                Tickets.WinPath(CaptureConfig.get("CaptureRoot"), "sealed capture root") != CaptureRoot or
                Tickets.WinPath(CaptureConfig.get("RoleEvidenceRoot"), "sealed role root") !=
                Tickets.WinPath(Spec["Roles"][Role]["EvidenceRoot"], "role root")):
            raise ValueError("[Qualification:FarmOuter] capture preparation spec/config mismatch")
        PrivateBoundary = Tickets.WinPath(str(PrivateRoot), "private root")
        if Tickets.Contained(CaptureRoot, PrivateBoundary) or Tickets.Contained(PrivateBoundary, CaptureRoot):
            raise ValueError("[Qualification:FarmOuter] capture preparation overlaps private root")
    for Role in Roots:
        Index = str(PureWindowsPath(Roots[Role]) / "stage-index.json")
        if Role == "SERVER":
            TransportInstance.Remote("prepare-capture", RemoteText(Roots[Role]), RemoteText(Index))
        else:
            from farm_outer_endpoint import PrepareCapture
            PrepareCapture(Roots[Role], Index)
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
    FirewallAdded = False
    RuntimeVerified = False
    try:
        VerifyRuntimePins(PrivateRoot, WorkerPython, WorkerHelper, TransportInstance)
        RuntimeVerified = True
        RequireFreshPreflights(PrivateRoot, Identity)
        for Role in Roots:
            Index = str(PureWindowsPath(Roots[Role]) / "stage-index.json")
            if Role == "SERVER":
                TransportInstance.Remote("verify", RemoteText(Roots[Role]), RemoteText(Index))
            else:
                from farm_outer_endpoint import Verify
                Verify(Roots[Role], Index)
        TransportInstance.AddControlFirewall(Identity["RunId"])
        FirewallAdded = True
        return LaunchPrepared(TransportInstance, Roots, Spec, Identity, PrivateRoot)
    finally:
        try:
            if RuntimeVerified and "NodeStage" in Spec["Roles"]["SERVER"]:
                WorkerRunRoot = str(PureWindowsPath(WorkerHelper).parent / Identity["RunId"])
                try:
                    TransportInstance.Remote("retire-node-token", RemoteText(WorkerRunRoot))
                finally:
                    TransportInstance.Remote("retire-node-tls", RemoteText(WorkerRunRoot))
        finally:
            if FirewallAdded:
                TransportInstance.RemoveControlFirewall(Identity["RunId"])


def LaunchPrepared(TransportInstance, Roots, Spec, Identity, PrivateRoot):
    BarrierStart = time.monotonic()
    ControlProbe(TransportInstance, Identity["RunId"])
    ProbeElapsed = time.monotonic() - BarrierStart
    Root = RemoteText(Roots["SERVER"])
    Index = RemoteText(str(PureWindowsPath(Root) / "stage-index.json"))
    HostConfig = RemoteText(str(PureWindowsPath(Root) / "host-launch.json"))
    Host = TransportInstance.RemoteStart("host", Root, Index, HostConfig)
    Launched = [("SERVER", "host", Host)]
    Success = False
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
        Launched.append(("SERVER", "role",
                         TransportInstance.RemoteStart("role", Root, Index, ServerLaunch)))
        ClientRoot = Path(Roots["CLIENT"]).resolve(strict=True)
        # A local hidden child keeps the main controller free to observe both roles.
        Launched.append(("CLIENT", "role", subprocess.Popen([sys.executable, "-B",
                                       str(SOURCE / "farm_outer_endpoint.py"),
                                       "role", str(ClientRoot), str(ClientRoot / "stage-index.json"),
                                       str(ClientRoot / "role-launch.json")],
                                      cwd=str(SOURCE), stdout=subprocess.DEVNULL,
                                      stderr=subprocess.DEVNULL, creationflags=Hidden())))
        if time.monotonic() - Barrier >= 60:
            raise TimeoutError("[Qualification:FarmOuter] role launch barrier exceeded")
        Deadline = time.monotonic() + OUTER_FINISH_SECONDS
        for _, _, Process in Launched[1:]:
            Remaining = max(0.1, Deadline - time.monotonic())
            if Process.wait(timeout=Remaining) != 0:
                raise RuntimeError("[Qualification:FarmOuter] role failed; preserve evidence")
        if Host.wait(timeout=max(0.1, Deadline - time.monotonic())) != 0:
            raise RuntimeError("[Qualification:FarmOuter] coordinator failed; preserve evidence")
        Success = True
    finally:
        ReapLaunch(TransportInstance, Roots, Identity["RunId"], PrivateRoot, Launched, Success)
    return 0


def ReapLaunch(TransportInstance, Roots, RunId, PrivateRoot, Launched, Success):
    """Abort only run-owned wrappers and require endpoint child-reap receipts."""
    Errors = []
    if not Success:
        for Role, Action, _ in Launched:
            Root = Roots[Role]
            Index = str(PureWindowsPath(Root) / "stage-index.json")
            try:
                if Role == "SERVER":
                    TransportInstance.Remote("abort", RemoteText(Root), RemoteText(Index), Action)
                else:
                    from farm_outer_endpoint import Abort
                    Abort(Root, Index, Action)
            except (ValueError, OSError, subprocess.SubprocessError) as Error:
                Errors.append("abort request failed for " + Role + "/" + Action + ": " + str(Error))
    for Role, Action, Process in Launched:
        try:
            Process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            Process.terminate()  # Wrapper only; the endpoint lease remains bounded.
            Errors.append("owned " + Role + "/" + Action + " wrapper did not reap")
        Root = Roots[Role]
        RemoteReceipt = str(PureWindowsPath(Root) / (Action + ".terminal.json"))
        try:
            if Role == "SERVER":
                Receipt = Path(PrivateRoot) / ("worker-" + Action + "-terminal.json")
                TransportInstance.Fetch(RemoteReceipt, Receipt)
            else:
                Receipt = Path(Root) / (Action + ".terminal.json")
            Row = ReadJson(Receipt)
            if (Row.get("Format") != "GargantuanFarm32Terminal" or Row.get("Version") != 1 or
                    Row.get("RunId") != RunId or Row.get("Action") != Action or
                    Row.get("ChildTreeReaped") is not True or
                    (Success and Row.get("Outcome") != "COMPLETED")):
                raise ValueError("terminal child-reap identity mismatch")
        except (ValueError, OSError, subprocess.SubprocessError) as Error:
            Errors.append("terminal receipt missing or invalid for " + Role + "/" + Action +
                          ": " + str(Error))
    if Errors:
        raise RuntimeError("[Qualification:FarmOuter] cleanup unproven: " + "; ".join(Errors))


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
                (IndexName == "capture-sha256.json" and Member["Bytes"] >= CAPTURE_MEMBER_MAX_BYTES) or
                not 0 <= Member["Bytes"] <= (335544832 if
                    Member["Name"] == "publication-service.bin" and Role == "SERVER" else
                    32 * 1024 * 1024 if Member["Name"] == "admission-fairness.tsv" and Role == "SERVER" else
                    32 * 1024 * 1024 if Role == "SERVER" and IndexName == "evidence-sha256.json" and
                    Member["Name"] in ("recovery-gameplay.tsv", "recovery-structural.tsv", "recovery-mixed.tsv") else
                    MaximumMemberBytes) or
                not isinstance(Member["Sha256"], str) or
                not SHA.fullmatch(Member["Sha256"])):
            raise ValueError("[Qualification:FarmOuter] indexed worker evidence member invalid")
        Seen.add(Member["Name"])
        Destination = LocalRoot / Member["Name"]
        TransportInstance.Fetch(str(PureWindowsPath(RemoteRoot) / Member["Name"]),
                                Destination, Timeout=CAPTURE_TRANSFER_SECONDS if IndexName == "capture-sha256.json" else 180)
        if Destination.stat().st_size != Member["Bytes"] or Digest(Destination) != Member["Sha256"]:
            raise ValueError("[Qualification:FarmOuter] transferred worker evidence hash mismatch")
    return Index


def FetchNodeResources(TransportInstance, NodeRun, NodeRoot):
    """Copy only the pinned bounded provider-process CSV; no directory walk."""
    Fields = {"ResourceContract", "ResourcePath", "ResourceBytes", "ResourceSamples", "ResourceSha256"}
    Present = Fields.intersection(NodeRun)
    if not Present:
        return  # Historical receipts do not gain resource coverage.
    if (Present != Fields or NodeRun["ResourceContract"] != "node_process_resources_v1" or
            NodeRun["ResourcePath"] != str(NodeRoot / "node-resources.csv") or
            type(NodeRun["ResourceBytes"]) is not int or not 0 < NodeRun["ResourceBytes"] <= 1024 ** 2 or
            type(NodeRun["ResourceSamples"]) is not int or not 2 <= NodeRun["ResourceSamples"] <= 1202 or
            not isinstance(NodeRun["ResourceSha256"], str) or not SHA.fullmatch(NodeRun["ResourceSha256"])):
        raise ValueError("[Qualification:FarmOuter] Node resource receipt path or bound invalid")
    Target = NodeRoot / "node-resources.csv"
    TransportInstance.Fetch(NodeRun["ResourcePath"], Target, Timeout=30)
    if Target.stat().st_size != NodeRun["ResourceBytes"] or Digest(Target) != NodeRun["ResourceSha256"]:
        raise ValueError("[Qualification:FarmOuter] Node resource transfer changed")


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
        Identity["RunId"], "SERVER", 20, CAPTURE_MEMBER_MAX_BYTES)
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
        FetchNodeResources(TransportInstance, NodeRun, NodeRoot)
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
