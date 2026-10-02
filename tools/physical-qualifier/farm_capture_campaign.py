"""Fixed, role-local Farm32 capture lifetime and offline evidence binding.

This source-only adapter is started locally on each endpoint before the farm
coordinator is armed. It never launches the farm application or a remote command.
The caller must wait for capture-controller-ready.json on both endpoints before
arming the existing workflow. A sealed receipt is not packet qualification.
"""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import uuid


CONFIG_KEYS = frozenset({"Format", "Version", "RunId", "CoordinatorRunId", "Role",
                         "CaptureRoot", "RoleEvidenceRoot", "PowerShellPath", "PowerShellSha256"})
WORKER_KEYS = frozenset({"ServicePath", "ServiceSha256", "HookPath", "HookSha256"})
CLIENT_KEYS = frozenset({"CaptureScriptPath", "CaptureScriptSha256", "DumpcapPath", "DumpcapSha256"})
SHA256 = re.compile(r"[0-9a-fA-F]{64}\Z")
ROLE_DEADLINE_SECONDS = 500  # Leaves 80 s for Stop and 20 s before the service's 600 s lease.
STOP_TIMEOUT_SECONDS = 80
FINALIZE_TIMEOUT_SECONDS = 1800  # Offline only; the privileged Stop remains separately bounded.
CLIENT_AUTOSTOP_SECONDS = 630  # 600 s dumpcap autostop plus its own 30 s close bound.
CAPTURE_PROFILE = "Farm32Capture16GiB-v2"
MAX_CAPTURE_BYTES = 15 * 1024 * 1024 * 1024
MAX_TEXT_BYTES = 65536


def Digest(File):
    with Path(File).open("rb") as Stream:
        return hashlib.file_digest(Stream, "sha256").hexdigest().lower()


def ReadJson(File, Maximum=MAX_TEXT_BYTES):
    File = Path(File)
    if not File.is_file() or File.stat().st_size > Maximum:
        raise ValueError("[Qualification:FarmCapture] missing or oversized JSON: " + str(File))
    return json.loads(File.read_text(encoding="utf-8"))


def UtcNow():
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def ParseUtc(Value):
    if not isinstance(Value, str):
        raise ValueError("[Qualification:FarmCapture] timestamp is absent")
    Parsed = datetime.fromisoformat(Value.replace("Z", "+00:00"))
    if Parsed.tzinfo is None:
        raise ValueError("[Qualification:FarmCapture] timestamp has no UTC offset")
    return Parsed.astimezone(timezone.utc)


def CanonicalUuid(Value):
    try:
        return isinstance(Value, str) and str(uuid.UUID(Value)) == Value
    except (ValueError, AttributeError):
        return False


def LocalPath(Value, MustExist=False):
    if not isinstance(Value, str) or not Value or len(Value) > 512:
        raise ValueError("[Qualification:FarmCapture] invalid local path")
    File = Path(Value)
    if not File.is_absolute() or str(File).startswith("\\\\"):
        raise ValueError("[Qualification:FarmCapture] path must be local and absolute")
    Current = File
    while True:
        if Current.exists() and (Current.is_symlink() or
                                 (hasattr(Current, "is_junction") and Current.is_junction())):
            raise ValueError("[Qualification:FarmCapture] reparse path is forbidden")
        if Current == Current.parent:
            break
        Current = Current.parent
    return File.resolve(strict=MustExist)


def PinnedFile(Value, Expected, Name):
    if not isinstance(Expected, str) or not SHA256.fullmatch(Expected):
        raise ValueError("[Qualification:FarmCapture] invalid " + Name + " pin")
    File = LocalPath(Value, MustExist=True)
    if not File.is_file() or Digest(File) != Expected.lower():
        raise ValueError("[Qualification:FarmCapture] " + Name + " pin changed")
    return File


def Configured(ConfigPath):
    Config = ReadJson(ConfigPath, 8192)
    if not isinstance(Config, dict) or Config.get("Format") != "GargantuanFarm32CaptureCampaign" or \
            type(Config.get("Version")) is not int or Config["Version"] != 1 or \
            Config.get("Role") not in ("SERVER", "CLIENT") or \
            not CanonicalUuid(Config.get("RunId")) or not CanonicalUuid(Config.get("CoordinatorRunId")):
        raise ValueError("[Qualification:FarmCapture] invalid fixed config identity")
    Expected = CONFIG_KEYS | (WORKER_KEYS if Config["Role"] == "SERVER" else CLIENT_KEYS)
    if set(Config) != Expected:
        raise ValueError("[Qualification:FarmCapture] unexpected fixed config fields")
    Config = dict(Config)
    Config["PowerShellPath"] = PinnedFile(Config["PowerShellPath"], Config["PowerShellSha256"], "PowerShell")
    if Config["PowerShellPath"].name.lower() != "pwsh.exe":
        raise ValueError("[Qualification:FarmCapture] unexpected PowerShell image")
    for Name, Hash in (("ServicePath", "ServiceSha256"), ("HookPath", "HookSha256")) if Config["Role"] == "SERVER" else \
                      (("CaptureScriptPath", "CaptureScriptSha256"), ("DumpcapPath", "DumpcapSha256")):
        Config[Name] = PinnedFile(Config[Name], Config[Hash], Name)
    RequiredNames = {"ServicePath": "AgentCoordinator.CaptureFarm32Service.exe",
                     "HookPath": "CaptureFarm32.ps1", "CaptureScriptPath": "DumpcapFarm32Capture.ps1",
                     "DumpcapPath": "dumpcap.exe"}
    for Name, ExpectedName in RequiredNames.items():
        if Name in Config and Config[Name].name.lower() != ExpectedName.lower():
            raise ValueError("[Qualification:FarmCapture] unexpected fixed capture executable")
    Config["CaptureRoot"] = LocalPath(Config["CaptureRoot"], MustExist=True)
    Config["RoleEvidenceRoot"] = LocalPath(Config["RoleEvidenceRoot"])
    if not Config["CaptureRoot"].is_dir() or not Config["RoleEvidenceRoot"].parent.is_dir() or \
            Config["CaptureRoot"] == Config["RoleEvidenceRoot"] or \
            Config["CaptureRoot"] in Config["RoleEvidenceRoot"].parents or \
            Config["RoleEvidenceRoot"] in Config["CaptureRoot"].parents:
        raise ValueError("[Qualification:FarmCapture] capture and role roots overlap")
    Config["CaptureDirectory"] = Config["CaptureRoot"] / Config["RunId"]
    if Config["CaptureDirectory"].exists():
        raise ValueError("[Qualification:FarmCapture] capture run directory already exists")
    return Config


def FixedRun(Command, Timeout, Runner=subprocess.run):
    Result = Runner([str(Item) for Item in Command], capture_output=True, text=True,
                    timeout=Timeout, check=False,
                    creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    if len(Result.stdout) > MAX_TEXT_BYTES or len(Result.stderr) > MAX_TEXT_BYTES or Result.returncode:
        raise RuntimeError("[Qualification:FarmCapture] fixed operation failed or exceeded output bound")
    return Result.stdout


def ServiceReply(Text, Operation, RunId, State):
    Row = json.loads(Text.strip())
    if not isinstance(Row, dict) or Row.get("Success") is not True or \
            Row.get("Operation") != Operation or Row.get("RunId") != RunId or Row.get("State") != State:
        raise ValueError("[Qualification:FarmCapture] capture service response mismatch")


def RoleEvidence(Root, RunId, Role):
    Root = LocalPath(str(Root), MustExist=True)
    Index = Root / "evidence-sha256.json"
    Result = Root / "result.json"
    IndexRow = ReadJson(Index, 1024 * 1024)
    ResultRow = ReadJson(Result)
    ExpectedRole = "Server" if Role == "SERVER" else "Clients"
    if IndexRow.get("RunId") != RunId or IndexRow.get("Role") != ExpectedRole or \
            ResultRow.get("RunId") != RunId or ResultRow.get("Role") != ExpectedRole or \
            ResultRow.get("Status") != "PASS":
        raise ValueError("[Qualification:FarmCapture] role receipt failed or has wrong identity")
    Matches = [Item for Item in IndexRow.get("Files", []) if Item.get("Name") == "result.json"]
    if len(Matches) != 1 or Matches[0].get("Sha256", "").lower() != Digest(Result):
        raise ValueError("[Qualification:FarmCapture] role result is not in its sealed index")
    return Index, ResultRow


def SealCapture(Config, StartedUtc, ReadyUtc, StoppedUtc, RoleIndex, ClockSeconds):
    Directory = Config["CaptureDirectory"]
    AssertCaptureProfile(Config)
    Pcap = Directory / ("farm32-worker-capture.pcapng" if Config["Role"] == "SERVER" else
                        "farm32-client-capture.pcapng")
    if not Pcap.is_file() or not 0 < Pcap.stat().st_size < MAX_CAPTURE_BYTES or \
            (Directory / (Pcap.name + ".pending")).exists():
        raise ValueError("[Qualification:FarmCapture] missing, capped or malformed capture")
    with Pcap.open("rb") as Stream:
        if Stream.read(4) != b"\x0a\x0d\x0d\x0a":
            raise ValueError("[Qualification:FarmCapture] malformed capture section")
    if Config["Role"] == "SERVER":
        Etl = Directory / "farm32-worker-capture.etl"
        Summary = Directory / "farm32-capture-summary.txt"
        if not Etl.is_file() or not 0 < Etl.stat().st_size < MAX_CAPTURE_BYTES or \
                not Summary.is_file() or not re.search(r"(?m)^Total Events\s+Lost\s+0\s*$",
                                                       Summary.read_text(encoding="utf-8", errors="replace")):
            raise ValueError("[Qualification:FarmCapture] worker trace completeness is unproven")
    Files = []
    for File in sorted(Directory.iterdir()):
        if File.name == "capture-sha256.json":
            continue
        if File.is_symlink() or not File.is_file() or len(Files) >= 20:
            raise ValueError("[Qualification:FarmCapture] unsafe capture artifact")
        Size = File.stat().st_size
        if Size >= MAX_CAPTURE_BYTES:
            raise ValueError("[Qualification:FarmCapture] capture artifact exceeds bound")
        Files.append({"Name": File.name, "Bytes": Size, "Sha256": Digest(File)})
    Index = {"Format": "GargantuanFarm32CaptureEvidence", "Version": 1, "Profile": CAPTURE_PROFILE,
             "RunId": Config["RunId"], "CoordinatorRunId": Config["CoordinatorRunId"],
             "Role": Config["Role"], "State": "SEALED_UNQUALIFIED",
             "StartedUtc": StartedUtc, "ReadyUtc": ReadyUtc, "StoppedUtc": StoppedUtc,
             "ElapsedSeconds": ClockSeconds, "RoleIndexSha256": Digest(RoleIndex), "Files": Files}
    with (Directory / "capture-sha256.json").open("x", encoding="utf-8") as Stream:
        json.dump(Index, Stream, indent=2)
        Stream.write("\n")
    return Directory / "capture-sha256.json"


def AssertCaptureProfile(Config):
    Worker = Config["Role"] == "SERVER"
    Marker = ReadJson(Config["CaptureDirectory"] / (
        "farm32-netsh-owner.json" if Worker else "farm32-client-capture.json"))
    Expected = {"Profile": CAPTURE_PROFILE}
    Expected.update({"TraceMaximumMiB": 16384, "NoWrapThresholdMiB": 15360} if Worker else
                    {"DurationSeconds": 600, "AutostopKilobytes": 16777216,
                     "CompletenessBytes": MAX_CAPTURE_BYTES})
    if any(Marker.get(Key) != Value for Key, Value in Expected.items()):
        raise ValueError("[Qualification:FarmCapture] capture profile marker differs from the pinned candidate")


class FarmCaptureController:
    def __init__(self, Config, Runner=subprocess.run, Spawner=subprocess.Popen,
                 Clock=time.monotonic, Sleep=time.sleep):
        self.Config = Config
        self.Runner, self.Spawner, self.Clock, self.Sleep = Runner, Spawner, Clock, Sleep
        self.Started = None
        self.StartedUtc = self.ReadyUtc = None
        self.Child = None
        self.WorkerActive = False

    def Service(self, Operation, State, Timeout):
        Config = self.Config
        Text = FixedRun([Config["ServicePath"], Operation.lower(), Config["CaptureDirectory"],
                         Config["RunId"], os.getpid()], Timeout, self.Runner)
        ServiceReply(Text, Operation.lower(), Config["RunId"], State)

    def Start(self):
        Config = self.Config
        Directory = Config["CaptureDirectory"]
        Directory.mkdir(mode=0o700)
        self.Started = self.Clock()
        self.StartedUtc = UtcNow()
        if Config["Role"] == "SERVER":
            # A lost response after a successful Start is still our lease;
            # attempt the fixed Stop on every failed Start path.
            self.WorkerActive = True
            self.Service("start", "running", STOP_TIMEOUT_SECONDS)
            if not (Directory / "farm32-capture-active.txt").is_file():
                raise ValueError("[Qualification:FarmCapture] worker capture did not become active")
        else:
            Command = [str(Config["PowerShellPath"]), "-NoProfile", "-NonInteractive", "-File",
                       str(Config["CaptureScriptPath"]), "-EvidenceRoot", str(Config["CaptureRoot"]),
                       "-RunId", Config["RunId"], "-DumpcapPath", str(Config["DumpcapPath"]),
                       "-DumpcapSha256", Config["DumpcapSha256"]]
            with (Directory / "capture-controller.stdout.log").open("xb") as Output, \
                    (Directory / "capture-controller.stderr.log").open("xb") as Error:
                self.Child = self.Spawner(Command, cwd=str(Config["CaptureScriptPath"].parent),
                                          stdout=Output, stderr=Error,
                                          creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            Pcap = Directory / "farm32-client-capture.pcapng"
            Deadline = self.Clock() + 15
            while self.Clock() < Deadline:
                if self.Child.poll() is not None:
                    raise RuntimeError("[Qualification:FarmCapture] client capture exited before readiness")
                if Pcap.is_file() and Pcap.stat().st_size >= 28:
                    with Pcap.open("rb") as Stream:
                        if Stream.read(4) == b"\x0a\x0d\x0d\x0a":
                            break
                self.Sleep(0.1)
            else:
                raise TimeoutError("[Qualification:FarmCapture] client capture readiness timed out")
        AssertCaptureProfile(Config)
        self.ReadyUtc = UtcNow()
        with (Directory / "capture-controller-ready.json").open("x", encoding="utf-8") as Stream:
            json.dump({"RunId": Config["RunId"], "CoordinatorRunId": Config["CoordinatorRunId"],
                       "Role": Config["Role"], "ReadyUtc": self.ReadyUtc}, Stream)
        return Directory / "capture-controller-ready.json"

    def AwaitRole(self):
        Config = self.Config
        while self.Clock() - self.Started < ROLE_DEADLINE_SECONDS:
            Index = Config["RoleEvidenceRoot"] / "evidence-sha256.json"
            if Index.is_file():
                try:
                    Evidence = RoleEvidence(Config["RoleEvidenceRoot"], Config["RunId"], Config["Role"])
                except (json.JSONDecodeError, FileNotFoundError):
                    pass  # A read may overlap the role supervisor's final write.
                else:
                    # The wrapper's successful exit does not prove when dumpcap
                    # stopped. It must still be alive after the role is sealed.
                    if self.Child is not None and self.Child.poll() is not None:
                        raise RuntimeError("[Qualification:FarmCapture] client capture ended before role completion")
                    return Evidence
            if self.Child is not None and self.Child.poll() is not None:
                raise RuntimeError("[Qualification:FarmCapture] client capture ended before role completion")
            self.Sleep(0.1)
        raise TimeoutError("[Qualification:FarmCapture] role exceeded the 500-second capture deadline")

    def Finish(self):
        Config = self.Config
        RoleIndex, Result = self.AwaitRole()
        RoleIndexHash = Digest(RoleIndex)
        if ParseUtc(Result["StartedUtc"]) < ParseUtc(self.ReadyUtc):
            raise ValueError("[Qualification:FarmCapture] role began before capture readiness")
        if Config["Role"] == "SERVER":
            self.Service("stop", "stopped", STOP_TIMEOUT_SECONDS)
            self.WorkerActive = False
            StoppedUtc = UtcNow()
            self.Service("status", "idle", 10)
            Output = FixedRun([Config["PowerShellPath"], "-NoProfile", "-NonInteractive", "-File",
                               Config["HookPath"], "-EvidenceDir", Config["CaptureDirectory"],
                               "-Action", "Finalize"], FINALIZE_TIMEOUT_SECONDS, self.Runner)
            (Config["CaptureDirectory"] / "capture-controller-finalize.log").write_text(Output, encoding="utf-8")
        else:
            Remaining = max(1, CLIENT_AUTOSTOP_SECONDS - (self.Clock() - self.Started))
            if self.Child.wait(timeout=Remaining) != 0:
                raise RuntimeError("[Qualification:FarmCapture] client capture did not close cleanly")
            StoppedUtc = UtcNow()
        if ParseUtc(StoppedUtc) < ParseUtc(Result["CompletedUtc"]):
            raise ValueError("[Qualification:FarmCapture] capture ended before role completion")
        RoleEvidence(Config["RoleEvidenceRoot"], Config["RunId"], Config["Role"])
        if Digest(RoleIndex) != RoleIndexHash:
            raise ValueError("[Qualification:FarmCapture] role evidence changed after completion")
        return SealCapture(Config, self.StartedUtc, self.ReadyUtc, StoppedUtc,
                           RoleIndex, round(self.Clock() - self.Started, 3))

    def Abort(self):
        if self.WorkerActive:
            try:
                self.Service("stop", "stopped", STOP_TIMEOUT_SECONDS)
            except Exception:
                # Start may have failed before activation, or its reply may
                # have been lost. Idle status is the only safe alternative.
                self.Service("status", "idle", 10)
            else:
                self.Service("status", "idle", 10)
            self.WorkerActive = False
        if self.Child is not None and self.Child.poll() is None:
            if os.name == "nt":
                subprocess.run(["taskkill.exe", "/PID", str(self.Child.pid), "/T", "/F"],
                               capture_output=True, timeout=10, check=False,
                               creationflags=subprocess.CREATE_NO_WINDOW)
            else:
                self.Child.kill()
            self.Child.wait(timeout=10)


def RunRole(ConfigPath):
    Config = Configured(ConfigPath)
    Controller = FarmCaptureController(Config)
    try:
        Controller.Start()
        Index = Controller.Finish()
        print("[Qualification:FarmCapture] SEALED_UNQUALIFIED run=" + Config["RunId"] +
              " role=" + Config["Role"] + " index=" + str(Index), flush=True)
        return 0
    except BaseException as Error:
        try:
            Controller.Abort()
        except Exception as AbortError:
            Error = RuntimeError(str(Error) + "; owned capture cleanup failed: " + str(AbortError))
        Directory = Config["CaptureDirectory"]
        if Directory.is_dir():
            with (Directory / "capture-controller-incomplete.json").open("x", encoding="utf-8") as Stream:
                json.dump({"RunId": Config["RunId"], "Role": Config["Role"],
                           "State": "INCOMPLETE", "Reason": str(Error)[:512]}, Stream)
        raise


def BindReceipt(RunId, CoordinatorRunId, CoordinatorResult, ServerIndex, ClientIndex,
                ServerCapture, ClientCapture, Receipt):
    if not CanonicalUuid(RunId) or not CanonicalUuid(CoordinatorRunId):
        raise ValueError("[Qualification:FarmCapture] invalid binding identity")
    Coordinator = ReadJson(CoordinatorResult, 1024 * 1024)
    if Coordinator.get("RunId") != CoordinatorRunId or Coordinator.get("Success") is not True:
        raise ValueError("[Qualification:FarmCapture] coordinator did not complete successfully")
    References = {}
    for Role, IndexPath, CapturePath in (("SERVER", ServerIndex, ServerCapture),
                                         ("CLIENT", ClientIndex, ClientCapture)):
        Index = ReadJson(IndexPath, 1024 * 1024)
        Capture = ReadJson(CapturePath, 1024 * 1024)
        if Index.get("RunId") != RunId or Index.get("Role") != ("Server" if Role == "SERVER" else "Clients") or \
                Capture.get("RunId") != RunId or Capture.get("CoordinatorRunId") != CoordinatorRunId or \
                Capture.get("Role") != Role or Capture.get("State") != "SEALED_UNQUALIFIED" or \
                Capture.get("Profile") != CAPTURE_PROFILE or \
                Capture.get("RoleIndexSha256") != Digest(IndexPath):
            raise ValueError("[Qualification:FarmCapture] cross-role capture binding mismatch")
        References[Role] = {"RoleIndexSha256": Digest(IndexPath),
                            "CaptureIndexSha256": Digest(CapturePath)}
    Receipt = LocalPath(str(Receipt))
    with Receipt.open("x", encoding="utf-8") as Stream:
        json.dump({"Format": "GargantuanFarm32OuterReceipt", "Version": 1,
                   "RunId": RunId, "CoordinatorRunId": CoordinatorRunId,
                   "State": "SEALED_UNQUALIFIED", "CoordinatorResultSha256": Digest(CoordinatorResult),
                   "Roles": References, "CreatedUtc": UtcNow()}, Stream, indent=2)
        Stream.write("\n")
    return Receipt


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Sub = Parser.add_subparsers(dest="Operation", required=True)
    Role = Sub.add_parser("role")
    Role.add_argument("Config")
    Bind = Sub.add_parser("bind")
    for Name in ("RunId", "CoordinatorRunId", "CoordinatorResult", "ServerIndex", "ClientIndex",
                 "ServerCapture", "ClientCapture", "Receipt"):
        Bind.add_argument(Name)
    Args = Parser.parse_args()
    if Args.Operation == "role":
        return RunRole(Args.Config)
    BindReceipt(Args.RunId, Args.CoordinatorRunId, Args.CoordinatorResult,
                Args.ServerIndex, Args.ClientIndex, Args.ServerCapture, Args.ClientCapture, Args.Receipt)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(Main())
    except Exception as Error:
        print("[Qualification:FarmCapture] " + str(Error), file=sys.stderr)
        sys.exit(1)
