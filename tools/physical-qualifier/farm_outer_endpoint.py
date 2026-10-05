"""Fixed, bounded endpoint operations for a staged Farm32 campaign.

This helper has no network-supplied command facility. The outer controller may
ask it to make/verify a private stage root, prove a control socket, or run one
of the pinned Farm32 host/role entrypoints. It never starts a capture itself.
"""

import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import secrets
import socket
import stat
import subprocess
import sys
import time
import threading
import uuid

# The pinned worker Python uses python312._pth and omits the script directory.
# The fixed helper and its sibling ACL module are staged together under one
# private, hash-verified tool root.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from private_ticket_acl import AssertPrivate, Harden


SHA = re.compile(r"[0-9a-f]{64}\Z")
MAX_CONFIG = 65536
MAX_LOG = 8 * 1024 * 1024
MAX_SECONDS = 3050  # 3000 s capture finish plus 50 s outer cleanup guard; no live-gate change.
NODE_TOKEN_NAME = "node-token.secret"


def StartOwned(Args, Directory):
    """Only consume the explicitly exported, hash-verified upstream primitive."""
    from dependency import GetRoot
    Root = GetRoot()
    Lock = ReadJson(Path(__file__).with_name("upstream.lock.json"))
    Names = ("agent_coordinator/owned_process.py", "agent_coordinator/windows_owned_process.py")
    if not set(Names).issubset(Lock["Files"]):
        raise ValueError("[Qualification:FarmOuter] owned runner source export is not pinned")
    sys.path.insert(0, str(Root))
    from agent_coordinator.owned_process import OwnedProcess
    Module = sys.modules[OwnedProcess.__module__]
    if Path(Module.__file__).resolve() != (Root / Names[0]).resolve():
        raise ValueError("[Qualification:FarmOuter] unexpected owned runner module")
    Cached = sys.modules.get("agent_coordinator.windows_owned_process")
    if Cached is not None and Path(Cached.__file__).resolve() != (Root / Names[1]).resolve():
        raise ValueError("[Qualification:FarmOuter] unexpected Windows ownership module")
    return OwnedProcess.Start(Args, Directory)


def DrainOwned(Stream, Output, Errors):
    Total = 0
    try:
        while Chunk := Stream.read(65536):
            Keep = max(0, MAX_LOG + 1 - Total)
            Output.write(Chunk[:Keep])
            Output.flush()
            Total += len(Chunk)
            if Total > MAX_LOG and not Errors:
                Errors.append("owned process log bound")
    except (OSError, ValueError) as Error:
        Errors.append(str(Error))
    finally:
        Stream.close()


def Digest(File):
    with Path(File).open("rb") as Stream:
        return hashlib.file_digest(Stream, "sha256").hexdigest()


def ReadJson(File):
    File = Path(File).resolve(strict=True)
    if not File.is_file() or File.stat().st_size > MAX_CONFIG:
        raise ValueError("[Qualification:FarmOuter] missing or oversized input")
    return json.loads(File.read_text(encoding="utf-8"))


def Prepare(Root):
    Root = UnlinkedPath(Root)
    if Root.exists() or not Root.parent.is_dir():
        raise ValueError("[Qualification:FarmOuter] stage root already exists")
    ParentIdentity = Root.parent.stat()
    Root.mkdir(mode=0o700)
    UnlinkedPath(Root)
    CurrentParent = Root.parent.stat()
    if (ParentIdentity.st_dev, ParentIdentity.st_ino) != (CurrentParent.st_dev, CurrentParent.st_ino):
        raise ValueError("[Qualification:FarmOuter] preparation parent changed")
    Harden(Root)
    UnlinkedPath(Root)


def UnlinkedPath(Value):
    if not isinstance(Value, (str, Path)) or not 1 <= len(str(Value)) <= 512:
        raise ValueError("[Qualification:FarmOuter] invalid preparation path")
    Root = Path(Value)
    if not Root.is_absolute() or str(Root).startswith("\\\\") or ".." in Root.parts:
        raise ValueError("[Qualification:FarmOuter] preparation path is not local and absolute")
    for Current in (Root, *Root.parents):
        try:
            Attributes = Current.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(Attributes.st_mode) or \
                getattr(Attributes, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT:
            raise ValueError("[Qualification:FarmOuter] preparation reparse path is forbidden")
    return Root.resolve()


def PrepareCapture(Root, IndexPath):
    """Provision only the capture root named by the verified one-use role ticket."""
    Root = UnlinkedPath(Root).resolve(strict=True)
    if UnlinkedPath(IndexPath) != Root / "stage-index.json":
        raise ValueError("[Qualification:FarmOuter] capture preparation index escaped its stage")
    Index = Verify(Root, IndexPath)
    Required = {"ticket.json", "farm-config.json", "capture-config.json", "farm_campaign_runner.py",
                "farm_capture_campaign.py", "farm_lifecycle.py", "private_ticket_acl.py",
                "dependency.py", "upstream.lock.json", "farm_capture_directions.py"}
    if not Required <= {Entry["Name"] for Entry in Index["Files"]}:
        raise ValueError("[Qualification:FarmOuter] capture preparation stage is incomplete")
    Ticket = ReadJson(Root / "ticket.json")
    # Load only verified stage modules. VerifyRole binds the pinned configs,
    # identity, manifest, workflow and fresh actual-role preflight.
    Modules = []
    for Name in ("farm_campaign_runner", "farm_capture_campaign"):
        Spec = importlib.util.spec_from_file_location("prepared_" + Name, Root / (Name + ".py"))
        Module = importlib.util.module_from_spec(Spec)
        Spec.loader.exec_module(Module)
        Modules.append(Module)
    Campaign, Capture = Modules
    for Name, BaseName in (("FarmConfigPath", "farm-config.json"),
                           ("CaptureConfigPath", "capture-config.json"),
                           ("CaptureControllerPath", "farm_capture_campaign.py")):
        if UnlinkedPath(Ticket.get(Name, "")) != Root / BaseName:
            raise ValueError("[Qualification:FarmOuter] capture preparation escaped its stage")
    _, Farm, _ = Campaign.VerifyRole(Ticket, Ticket.get("Role"))
    Config = Capture.ReadConfig(Root / "capture-config.json", CaptureRootMustExist=False)
    CaptureRoot = Config["CaptureRoot"]
    Protected = [Root, *[UnlinkedPath(Farm[Name]) for Name in
                         ("EvidenceRoot", "PackageRoot", "RunRegistryRoot")],
                 UnlinkedPath(Ticket["JournalRoot"]), UnlinkedPath(Ticket["ResultPath"])]
    if "NodeStagePath" in Ticket:
        Protected.append(UnlinkedPath(Ticket["NodeStagePath"]).parent)
    for Boundary in Protected:
        if CaptureRoot == Boundary or CaptureRoot in Boundary.parents or Boundary in CaptureRoot.parents:
            raise ValueError("[Qualification:FarmOuter] capture preparation overlaps a protected root")
    if CaptureRoot.exists():
        raise ValueError("[Qualification:FarmOuter] capture preparation root already exists")
    Prepare(CaptureRoot)
    Verify(Root, IndexPath)
    # Exercise the exact consumer before any service, capture or role is started.
    Configured = Capture.Configured(Root / "capture-config.json")
    if any(Configured[Name] != Config[Name] for Name in
           ("CaptureRoot", "RoleEvidenceRoot", "RunId", "CoordinatorRunId", "Role")):
        raise ValueError("[Qualification:FarmOuter] capture preparation config changed")
    print("[Qualification:FarmOuter] CAPTURE_ROOT_PREPARED_CONFIGURED_ONLY", flush=True)


def NewNodeToken(Root):
    AssertPrivate(Root)
    Root = Path(Root).resolve(strict=True)
    if str(uuid.UUID(Root.name)) != Root.name:
        raise ValueError("[Qualification:FarmOuter] Node token run identity mismatch")
    Token = Root / NODE_TOKEN_NAME
    with Token.open("xb") as Stream:
        Stream.write(secrets.token_hex(32).encode("ascii"))
    print("[Qualification:FarmOuter] NODE_TOKEN_CREATED", flush=True)


def RetireNodeToken(Root):
    if Path(Root).is_symlink() or (hasattr(Path(Root), "is_junction") and Path(Root).is_junction()):
        raise ValueError("[Qualification:FarmOuter] Node token root is a link")
    if not Path(Root).exists():
        return
    AssertPrivate(Root)
    Root = Path(Root).resolve(strict=True)
    if str(uuid.UUID(Root.name)) != Root.name:
        raise ValueError("[Qualification:FarmOuter] Node token run identity mismatch")
    Token = Root / NODE_TOKEN_NAME
    if Token.is_symlink():
        raise ValueError("[Qualification:FarmOuter] Node token is a link")
    Token.unlink(missing_ok=True)


def RetireNodeTls(Root):
    if Path(Root).is_symlink() or (hasattr(Path(Root), "is_junction") and Path(Root).is_junction()):
        raise ValueError("[Qualification:FarmOuter] Node TLS root is a link")
    if not Path(Root).exists():
        return
    AssertPrivate(Root)
    Root = Path(Root).resolve(strict=True)
    if str(uuid.UUID(Root.name)) != Root.name:
        raise ValueError("[Qualification:FarmOuter] Node TLS run identity mismatch")
    for Name in ("node-key.pem", "node-cert.pem", "node-root-ca.pem"):
        File = Root / Name
        if File.is_symlink():
            raise ValueError("[Qualification:FarmOuter] Node TLS material is a link")
        File.unlink(missing_ok=True)


def Verify(Root, IndexPath):
    AssertPrivate(Root)
    Root = Path(Root).resolve(strict=True)
    Index = ReadJson(IndexPath)
    if not isinstance(Index, dict) or set(Index) != {"Format", "Version", "Files"} or \
            Index["Format"] != "GargantuanFarm32StageIndex" or Index["Version"] != 1 or \
            not isinstance(Index["Files"], list) or not 1 <= len(Index["Files"]) <= 20:
        raise ValueError("[Qualification:FarmOuter] invalid stage index")
    Seen = set()
    for Entry in Index["Files"]:
        if not isinstance(Entry, dict) or set(Entry) != {"Name", "Sha256"} or \
                not isinstance(Entry["Name"], str) or not re.fullmatch(r"[A-Za-z0-9._-]{1,96}", Entry["Name"]) or \
                Entry["Name"] in Seen or Entry["Name"] == Path(IndexPath).name or \
                not isinstance(Entry["Sha256"], str) or not SHA.fullmatch(Entry["Sha256"]):
            raise ValueError("[Qualification:FarmOuter] invalid stage member")
        Seen.add(Entry["Name"])
        File = Root / Entry["Name"]
        if File.is_symlink() or not File.is_file() or Digest(File) != Entry["Sha256"]:
            raise ValueError("[Qualification:FarmOuter] stage member hash mismatch")
    if {"dependency.py", "upstream.lock.json"} <= Seen:
        Spec = importlib.util.spec_from_file_location("farm32_stage_dependency", Root / "dependency.py")
        if Spec is None or Spec.loader is None:
            raise ValueError("[Qualification:FarmOuter] pinned coordinator dependency is unavailable")
        Module = importlib.util.module_from_spec(Spec)
        Spec.loader.exec_module(Module)
        Module.GetRoot()  # Verify every pinned upstream source file before a role starts.
    return Index


def Probe(Host, Port, RunId):
    if Host != "192.168.0.108" or Port != 39451 or not re.fullmatch(r"[0-9a-f-]{36}", RunId):
        raise ValueError("[Qualification:FarmOuter] unexpected normal-LAN probe")
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as Listener:
        Listener.bind((Host, Port))
        Listener.listen(1)
        Listener.settimeout(10)
        print("[Qualification:FarmOuter] CONTROL_LISTENING", flush=True)
        Connection, Peer = Listener.accept()
        with Connection:
            Connection.settimeout(3)
            Challenge = Connection.recv(128)
            if Peer[0] != "192.168.0.68" or Challenge != RunId.encode("ascii"):
                raise ValueError("[Qualification:FarmOuter] control probe identity mismatch")
            Connection.sendall(Challenge)


def StopOwned(Process):
    if Process.poll() is not None:
        return True
    TreeStopped = False
    if os.name == "nt":
        Result = subprocess.run(["taskkill.exe", "/PID", str(Process.pid), "/T", "/F"],
                                capture_output=True, timeout=8,
                                creationflags=subprocess.CREATE_NO_WINDOW, check=False)
        TreeStopped = Result.returncode == 0
    else:
        Process.terminate()
        TreeStopped = True
    try:
        Process.wait(timeout=8)
    except subprocess.TimeoutExpired:
        Process.kill()
        Process.wait(timeout=5)
        TreeStopped = False
    return TreeStopped and Process.poll() is not None


def Abort(Root, IndexPath, Action):
    AssertPrivate(Root)
    if Action not in ("host", "role"):
        raise ValueError("[Qualification:FarmOuter] invalid abort target")
    Root = Path(Root).resolve(strict=True)
    Input = ReadJson(Root / ("host-config.json" if Action == "host" else "ticket.json"))
    RunId = Input.get("RunId")
    if (Input.get("Format") != "GargantuanFarm32Campaign" or Input.get("Version") != 1 or
            not isinstance(RunId, str) or str(uuid.UUID(RunId)) != RunId):
        raise ValueError("[Qualification:FarmOuter] abort identity mismatch")
    Marker = Root / (Action + ".abort.request")
    Row = {"Format": "GargantuanFarm32Abort", "Version": 1,
           "RunId": RunId, "Action": Action}
    if Marker.exists():
        if ReadJson(Marker) != Row:
            raise ValueError("[Qualification:FarmOuter] abort marker changed")
        return
    with Marker.open("xb") as Stream:
        Stream.write((json.dumps(Row, sort_keys=True) + "\n").encode("utf-8"))


def Run(Root, IndexPath, ConfigPath, Action):
    Index = Verify(Root, IndexPath)
    Config = ReadJson(ConfigPath)
    if not isinstance(Config, dict) or set(Config) != {"Format", "Version", "Action", "RunnerName",
                                                     "RunnerSha256", "InputName", "InputSha256"} or \
            Config["Format"] != "GargantuanFarm32FixedLaunch" or Config["Version"] != 1 or \
            Config["Action"] != Action or Action not in ("host", "role"):
        raise ValueError("[Qualification:FarmOuter] invalid launch configuration")
    Root = Path(Root).resolve(strict=True)
    Listed = {Entry["Name"] for Entry in Index["Files"]}
    for Name in ("RunnerName", "InputName"):
        if not isinstance(Config[Name], str) or not re.fullmatch(r"[A-Za-z0-9._-]{1,96}", Config[Name]):
            raise ValueError("[Qualification:FarmOuter] unsafe launch member name")
        File = Root / Config[Name]
        if Config[Name] not in Listed or not File.is_file() or File.is_symlink() or \
                Digest(File) != Config[Name.replace("Name", "Sha256")]:
            raise ValueError("[Qualification:FarmOuter] launch member changed")
    if Config["RunnerName"] != "farm_campaign_runner.py":
        raise ValueError("[Qualification:FarmOuter] runner is not fixed")
    Input = ReadJson(Root / Config["InputName"])
    if Action == "host":
        if Input.get("Format") != "GargantuanFarm32Campaign" or Input.get("Version") != 1:
            raise ValueError("[Qualification:FarmOuter] host input mismatch")
    elif Input.get("Format") != "GargantuanFarm32Campaign" or Input.get("Version") != 1 or \
            Input.get("Role") not in ("SERVER", "CLIENT"):
        raise ValueError("[Qualification:FarmOuter] role input mismatch")
    RunId = Input.get("RunId")
    if not isinstance(RunId, str) or str(uuid.UUID(RunId)) != RunId:
        raise ValueError("[Qualification:FarmOuter] launch run identity mismatch")
    Flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    Output = Root / (Action + ".stdout.log")
    Error = Root / (Action + ".stderr.log")
    AbortMarker = Root / (Action + ".abort.request")
    Terminal = Root / (Action + ".terminal.json")
    DiagnosticPath = Root / (Action + ".terminal-diagnostic.json")
    if AbortMarker.exists() or Terminal.exists() or DiagnosticPath.exists():
        raise ValueError("[Qualification:FarmOuter] stale abort or terminal marker")
    with Output.open("xb") as Stdout, Error.open("xb") as Stderr:
        Args = [sys.executable, "-B", str(Root / Config["RunnerName"]),
                Action, str(Root / Config["InputName"])]
        Tree = None
        Drains, DrainErrors = [], []
        if os.name == "nt":
            Process, Tree = StartOwned(Args, Root)
            try:
                Process.stdin.close()
                for Stream, OutputFile in ((Process.stdout, Stdout), (Process.stderr, Stderr)):
                    Thread = threading.Thread(target=DrainOwned, args=(Stream, OutputFile, DrainErrors), daemon=True)
                    Thread.start()
                    Drains.append(Thread)
            except BaseException:
                Tree.Close()
                DrainDeadline = time.monotonic() + 5
                for Thread in Drains:
                    Thread.join(timeout=max(0, DrainDeadline - time.monotonic()))
                for Stream in (Process.stdout, Process.stderr):
                    if not Stream.closed:
                        Stream.close()
                raise
        else:
            Process = subprocess.Popen(Args, cwd=str(Root), stdout=Stdout, stderr=Stderr,
                                       creationflags=Flags)
        Deadline = time.monotonic() + MAX_SECONDS
        Outcome = "FAILED"
        try:
            while Process.poll() is None:
                if AbortMarker.is_file():
                    if ReadJson(AbortMarker) != {"Format": "GargantuanFarm32Abort", "Version": 1,
                                                 "RunId": RunId, "Action": Action}:
                        raise ValueError("[Qualification:FarmOuter] abort marker identity mismatch")
                    Outcome = "ABORTED"
                    raise RuntimeError("[Qualification:FarmOuter] run-bound abort requested")
                if DrainErrors or time.monotonic() >= Deadline or Output.stat().st_size > MAX_LOG or Error.stat().st_size > MAX_LOG:
                    Outcome = "BOUND_EXCEEDED"
                    raise TimeoutError("[Qualification:FarmOuter] owned process time or log bound")
                time.sleep(0.1)
            if Process.returncode:
                raise RuntimeError("[Qualification:FarmOuter] pinned " + Action + " failed")
            Outcome = "COMPLETED"
        finally:
            WasRunning = Process.poll() is None
            Reaped = False
            CleanupError = None
            try:
                Reaped = Tree.Close() if Tree is not None else StopOwned(Process)
            except (OSError, TimeoutError, subprocess.TimeoutExpired) as Failure:
                CleanupError = str(Failure)
            DrainDeadline = time.monotonic() + 5
            for Thread in Drains:
                Thread.join(timeout=max(0, DrainDeadline - time.monotonic()))
            if any(Thread.is_alive() for Thread in Drains):
                Reaped = False
                CleanupError = "owned output did not drain"
            OutputFailed = bool(DrainErrors) or Output.stat().st_size > MAX_LOG or Error.stat().st_size > MAX_LOG
            if OutputFailed and Outcome == "COMPLETED":
                Outcome = "BOUND_EXCEEDED"
            # A failed runner that already exited may have left an unowned
            # descendant; PID-tree termination can only prove the live case.
            if Tree is None and Outcome != "COMPLETED" and not WasRunning:
                Reaped = False
            Row = {"Format": "GargantuanFarm32Terminal", "Version": 1,
                   "RunId": RunId, "Action": Action, "ChildPid": Process.pid,
                   "ChildExitCode": Process.returncode if Tree is not None else Process.poll(), "ChildTreeReaped": Reaped,
                   "Outcome": Outcome,
                   "EndedUtc": datetime.now(timezone.utc).isoformat()}
            with Terminal.open("xb") as Stream:
                Stream.write((json.dumps(Row, sort_keys=True) + "\n").encode("utf-8"))
            if CleanupError is not None or OutputFailed:
                Diagnostic = {"Format": "GargantuanFarm32TerminalDiagnostic", "Version": 1,
                              "RunId": RunId, "Action": Action, "ChildPid": Process.pid,
                              "CleanupError": CleanupError[:512] if CleanupError is not None else None,
                              "OutputError": (DrainErrors[0] if DrainErrors else "owned process log bound")[:512]
                                             if OutputFailed else None}
                with DiagnosticPath.open("xb") as Stream:
                    Stream.write((json.dumps(Diagnostic, sort_keys=True) + "\n").encode("utf-8"))
            if not Reaped:
                raise RuntimeError("[Qualification:FarmOuter] owned child tree was not reaped")
            if OutputFailed:
                raise RuntimeError("[Qualification:FarmOuter] owned process output failed or exceeded log bound")


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("Action", choices=("prepare", "prepare-capture", "new-node-token", "retire-node-token",
                                           "retire-node-tls",
                                           "verify", "digest", "probe", "abort", "host", "role"))
    Parser.add_argument("Root")
    Parser.add_argument("IndexOrPort", nargs="?")
    Parser.add_argument("ConfigOrRunId", nargs="?")
    Args = Parser.parse_args()
    if Args.Action == "prepare" and Args.IndexOrPort is None:
        Prepare(Args.Root)
    elif Args.Action == "prepare-capture" and Args.IndexOrPort is not None and Args.ConfigOrRunId is None:
        PrepareCapture(Args.Root, Args.IndexOrPort)
    elif Args.Action == "new-node-token" and Args.IndexOrPort is None:
        NewNodeToken(Args.Root)
    elif Args.Action == "retire-node-token" and Args.IndexOrPort is None:
        RetireNodeToken(Args.Root)
    elif Args.Action == "retire-node-tls" and Args.IndexOrPort is None:
        RetireNodeTls(Args.Root)
    elif Args.Action == "verify" and Args.IndexOrPort is not None:
        Verify(Args.Root, Args.IndexOrPort)
    elif Args.Action == "digest" and Args.IndexOrPort is None:
        File = Path(Args.Root).resolve(strict=True)
        if File.is_symlink() or not File.is_file():
            raise ValueError("[Qualification:FarmOuter] digest target is not a file")
        print(Digest(File))
    elif Args.Action == "probe" and Args.ConfigOrRunId is not None:
        Probe(Args.Root, int(Args.IndexOrPort), Args.ConfigOrRunId)
    elif Args.Action == "abort" and Args.ConfigOrRunId is not None:
        Abort(Args.Root, Args.IndexOrPort, Args.ConfigOrRunId)
    elif Args.Action in ("host", "role") and Args.ConfigOrRunId is not None:
        Run(Args.Root, Args.IndexOrPort, Args.ConfigOrRunId, Args.Action)
    else:
        Parser.error("invalid fixed endpoint operation arguments")


if __name__ == "__main__":
    try:
        Main()
    except (ValueError, RuntimeError, TimeoutError, OSError, json.JSONDecodeError) as Error:
        print(str(Error), file=sys.stderr)
        raise SystemExit(1)
