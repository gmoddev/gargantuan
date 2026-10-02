"""Fixed, role-local Agent Coordinator adapter for a 32-client farm campaign.

This adapter controls only the already-staged PowerShell supervisor. It does not
start a capture, install a service, change network policy, or accept wire-supplied
paths/arguments. The supervisor owns its Server or Player process tree and the
campaign's application evidence.
"""

import hashlib
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
from agent_coordinator import workflow  # noqa: E402

if Path(workflow.__file__).resolve() != (UPSTREAM / "agent_coordinator/workflow.py").resolve():
    raise ValueError("[Qualification:FarmLifecycle] unexpected coordinator library origin")


CONFIG_KEYS = frozenset({
    "Role", "CoordinatorRunId", "RunId", "SourceCommit", "PowerShellPath", "PowerShellSHA256",
    "SupervisorPath", "SupervisorSHA256", "ManifestPath", "ManifestSHA256",
    "PackageRoot", "EvidenceRoot", "RunRegistryRoot",
})
NODE_KEY = "NodeRootCertificatePath"
SHA256 = re.compile(r"[a-fA-F0-9]{64}\Z")
COMMIT = re.compile(r"[a-f0-9]{40}\Z")
CLIENT_READY = re.compile(r"^\[Qualification:Client\] event=ready(?:\s|$)")
FIELD = re.compile(r"(?:^|\s)([a-z][a-z0-9_]*)=([^\s]+)")
POLL_SECONDS = 50.0
STARTUP_SECONDS = 60.0
MAX_RESULT_BYTES = 65536
MAX_MARKER_BYTES = 4096
MAX_CLIENT_LOG_BYTES = 4 * 1024 * 1024


def CanonicalUuid(Value):
    return isinstance(Value, str) and str(uuid.UUID(Value)) == Value


def Digest(File):
    with File.open("rb") as Stream:
        return hashlib.file_digest(Stream, "sha256").hexdigest()


def PinnedFile(Text, Expected, Name):
    if not isinstance(Text, str) or not isinstance(Expected, str) or not SHA256.fullmatch(Expected):
        raise ValueError("[Qualification:FarmLifecycle] invalid " + Name + " pin")
    File = Path(Text).resolve(strict=True)
    if not File.is_file() or Digest(File) != Expected.lower():
        raise ValueError("[Qualification:FarmLifecycle] " + Name + " pin changed")
    return File


def ReadJson(File, Limit):
    if not File.is_file() or File.stat().st_size > Limit:
        raise ValueError("[Qualification:FarmLifecycle] missing or oversized evidence: " + str(File))
    return json.loads(File.read_text(encoding="utf-8"))


def Metadata(File):
    return {"Path": str(File), "SHA256": Digest(File), "Bytes": File.stat().st_size}


class FarmLifecycle:
    def __init__(self, ConfigPath):
        ConfigFile = Path(ConfigPath).resolve(strict=True)
        Config = ReadJson(ConfigFile, 8192)
        if not isinstance(Config, dict) or Config.get("Role") not in ("SERVER", "CLIENT"):
            raise ValueError("[Qualification:FarmLifecycle] invalid installed role")
        Allowed = CONFIG_KEYS | ({NODE_KEY} if NODE_KEY in Config else set())
        Allowed |= ({"RunTimeoutMilliseconds"} if "RunTimeoutMilliseconds" in Config else set())
        if set(Config) != Allowed:
            raise ValueError("[Qualification:FarmLifecycle] invalid installed config fields")
        if not CanonicalUuid(Config["RunId"]) or not CanonicalUuid(Config["CoordinatorRunId"]):
            raise ValueError("[Qualification:FarmLifecycle] noncanonical run identity")
        if not isinstance(Config["SourceCommit"], str) or not COMMIT.fullmatch(Config["SourceCommit"]):
            raise ValueError("[Qualification:FarmLifecycle] invalid source commit")
        self.Config = Config
        self.ConfigFile = ConfigFile
        self.PowerShell = PinnedFile(Config["PowerShellPath"], Config["PowerShellSHA256"], "PowerShell")
        self.Supervisor = PinnedFile(Config["SupervisorPath"], Config["SupervisorSHA256"], "supervisor")
        if self.PowerShell.name.lower() != "pwsh.exe" or self.Supervisor.name != "PhysicalGameSessionFarmEndpoint.ps1":
            raise ValueError("[Qualification:FarmLifecycle] unexpected fixed executable or supervisor")
        self.Manifest = PinnedFile(Config["ManifestPath"], Config["ManifestSHA256"], "run manifest")
        ManifestData = ReadJson(self.Manifest, 8192)
        if (not isinstance(ManifestData, dict) or ManifestData.get("Format") != "GargantuanPhysicalFarmEndpoint" or
                type(ManifestData.get("Version")) is not int or ManifestData["Version"] != 1 or
                ManifestData.get("RunId") != Config["RunId"] or
                ManifestData.get("SourceCommit") != Config["SourceCommit"] or
                ManifestData.get("Provider") not in ("Local", "Node") or
                not isinstance(ManifestData.get("Nonces"), list) or len(ManifestData["Nonces"]) != 32):
            raise ValueError("[Qualification:FarmLifecycle] installed run manifest mismatch")
        self.ManifestData = ManifestData
        Recovery = ManifestData.get("RecoveryWorkload", False)
        if type(Recovery) is not bool or (("RunTimeoutMilliseconds" in Config) != Recovery) or \
                (Recovery and (type(Config["RunTimeoutMilliseconds"]) is not int or
                               Config["RunTimeoutMilliseconds"] != 420000)):
            raise ValueError("[Qualification:FarmLifecycle] invalid recovery runtime bound")
        if (ManifestData["Provider"] == "Node") != (NODE_KEY in Config):
            raise ValueError("[Qualification:FarmLifecycle] Node certificate config mismatch")
        self.Package = Path(Config["PackageRoot"]).resolve(strict=True)
        self.Registry = Path(Config["RunRegistryRoot"]).resolve()
        self.Evidence = Path(Config["EvidenceRoot"]).resolve()
        if self.Evidence.exists() or not self.Package.is_dir() or not self.Evidence.parent.is_dir():
            raise ValueError("[Qualification:FarmLifecycle] stale evidence or missing local package/root")
        if self.Evidence == self.Package or self.Package in self.Evidence.parents or self.Evidence in self.Package.parents:
            raise ValueError("[Qualification:FarmLifecycle] package and evidence roots overlap")
        self.NodeCertificate = None
        if NODE_KEY in Config:
            self.NodeCertificate = Path(Config[NODE_KEY]).resolve(strict=True)
            if (not self.NodeCertificate.is_file() or
                    Digest(self.NodeCertificate) != ManifestData.get("NodeRootCertificateSha256", "").lower()):
                raise ValueError("[Qualification:FarmLifecycle] Node certificate pin changed")
        self.Process = None
        self.Output = None
        self.Error = None
        self.OutputFile = ConfigFile.parent / ("farm-" + Config["Role"].lower() + ".stdout.log")
        self.ErrorFile = ConfigFile.parent / ("farm-" + Config["Role"].lower() + ".stderr.log")
        print("[Qualification:FarmLifecycle] staged coordinator_run=" + Config["CoordinatorRunId"] +
              " manifest_run=" + Config["RunId"] + " role=" + Config["Role"], flush=True)

    def Require(self, Parameters, Role):
        if Parameters or self.Config["Role"] != Role:
            raise ValueError("[Qualification:FarmLifecycle] unauthorized role or parameters")

    def CheckProcess(self):
        if self.Process is None:
            raise ValueError("[Qualification:FarmLifecycle] role supervisor was not launched")
        Code = self.Process.poll()
        if Code is not None and Code != 0:
            raise RuntimeError("[Qualification:FarmLifecycle] role supervisor exited " + str(Code))
        return Code

    def Launch(self, Parameters, Context, Role):
        self.Require(Parameters, Role)
        Context.Check()
        if self.Process is not None or self.Evidence.exists():
            raise ValueError("[Qualification:FarmLifecycle] run already consumed")
        Args = [str(self.PowerShell), "-NoProfile", "-NonInteractive", "-File", str(self.Supervisor),
                "-Role", "Server" if Role == "SERVER" else "Clients",
                "-ManifestPath", str(self.Manifest), "-ManifestSha256", self.Config["ManifestSHA256"],
                "-PackageRoot", str(self.Package), "-EvidenceRoot", str(self.Evidence),
                "-RunRegistryRoot", str(self.Registry)]
        if self.NodeCertificate is not None and Role == "SERVER":
            Args.extend(("-NodeRootCertificatePath", str(self.NodeCertificate)))
        if "RunTimeoutMilliseconds" in self.Config:
            Args.extend(("-RunTimeoutMilliseconds", str(self.Config["RunTimeoutMilliseconds"])))
        self.Output = self.OutputFile.open("xb")
        try:
            self.Error = self.ErrorFile.open("xb")
            Flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
            self.Process = subprocess.Popen(Args, cwd=str(self.Supervisor.parent),
                                            stdout=self.Output, stderr=self.Error,
                                            creationflags=Flags)
            self.CheckProcess()
            print("[Qualification:FarmLifecycle] launched coordinator_run=" + self.Config["CoordinatorRunId"] +
                  " manifest_run=" + self.Config["RunId"] + " role=" + Role +
                  " supervisor_pid=" + str(self.Process.pid), flush=True)
            return {"Success": True, "Evidence": []}
        except BaseException:
            self.Cleanup()
            raise

    def ServerLaunch(self, Parameters, Context):
        return self.Launch(Parameters, Context, "SERVER")

    def ClientLaunch(self, Parameters, Context):
        return self.Launch(Parameters, Context, "CLIENT")

    def Await(self, Context, Predicate, Description, Seconds=STARTUP_SECONDS):
        Deadline = time.monotonic() + Seconds
        while True:
            Context.Check()
            if self.CheckProcess() is not None:
                raise RuntimeError("[Qualification:FarmLifecycle] supervisor ended before " + Description)
            Evidence = Predicate()
            if Evidence is not None:
                return {"Success": True, "Evidence": [Metadata(Evidence)]}
            if time.monotonic() >= Deadline:
                raise TimeoutError("[Qualification:FarmLifecycle] " + Description + " deadline elapsed")
            time.sleep(0.1)

    def ServerReady(self, Parameters, Context):
        self.Require(Parameters, "SERVER")

        def Marker():
            File = self.Evidence / "server-ready.json"
            if not File.exists():
                return None
            try:
                Row = ReadJson(File, MAX_MARKER_BYTES)
            except json.JSONDecodeError:
                # The supervisor writes this small marker after the server starts.
                # A read may overlap the write; the startup deadline still bounds retries.
                return None
            if (set(Row) != {"RunId", "Role", "Pid", "Endpoint", "ReadyUtc"} or
                    Row["RunId"] != self.Config["RunId"] or Row["Role"] != "Server" or
                    Row["Endpoint"] != self.ManifestData.get("Endpoint") or
                    type(Row["Pid"]) is not int or Row["Pid"] <= 0):
                raise ValueError("[Qualification:FarmLifecycle] invalid server-ready marker")
            return File

        return self.Await(Context, Marker, "server-ready")

    def ClientReady(self, Parameters, Context):
        self.Require(Parameters, "CLIENT")

        def Log():
            File = self.Evidence / "client-00.stdout.log"
            if not File.exists():
                return None
            if File.stat().st_size > MAX_CLIENT_LOG_BYTES:
                raise ValueError("[Qualification:FarmLifecycle] client readiness log over bound")
            Text = File.read_text(encoding="utf-8", errors="replace")
            Ready = []
            for Line in Text.splitlines(keepends=True):
                if not Line.endswith("\n"):
                    continue
                if CLIENT_READY.match(Line):
                    Ready.append(dict(FIELD.findall(Line)))
            if len(Ready) > 1:
                raise ValueError("[Qualification:FarmLifecycle] duplicate producer readiness")
            if not Ready:
                return None
            Row = Ready[0]
            if (Row.get("run_id") != self.Config["RunId"] or Row.get("slot") != "0" or
                    Row.get("nonce") != str(self.ManifestData["Nonces"][0]) or
                    not Row.get("connection_slot", "").isdigit() or
                    not Row.get("connection_generation", "").isdigit() or
                    int(Row["connection_slot"]) == 0 or int(Row["connection_generation"]) == 0):
                raise ValueError("[Qualification:FarmLifecycle] invalid producer readiness")
            return File

        return self.Await(Context, Log, "producer-client-ready", Seconds=80.0)

    def Progress(self, Parameters, Context):
        self.Require(Parameters, self.Config["Role"])
        Deadline = time.monotonic() + POLL_SECONDS
        while time.monotonic() < Deadline:
            Context.Check()
            if self.CheckProcess() is not None:
                break
            time.sleep(0.1)
        return {"Success": True, "Evidence": []}

    def Result(self, Parameters, Context):
        self.Require(Parameters, self.Config["Role"])
        while self.CheckProcess() is None:
            Context.Check()
            time.sleep(0.1)
        Context.Check()
        File = self.Evidence / "result.json"
        Row = ReadJson(File, MAX_RESULT_BYTES)
        ExpectedRole = "Server" if self.Config["Role"] == "SERVER" else "Clients"
        if (not isinstance(Row, dict) or Row.get("RunId") != self.Config["RunId"] or
                Row.get("Role") != ExpectedRole or Row.get("Provider") != self.ManifestData["Provider"] or
                Row.get("ManifestSha256", "").lower() != self.Config["ManifestSHA256"].lower() or
                Row.get("Status") != "PASS"):
            raise ValueError("[Qualification:FarmLifecycle] role result identity or verdict failed")
        return {"Success": True, "Evidence": [Metadata(File)]}

    def Cleanup(self):
        Process = self.Process
        Failure = None
        try:
            if Process is not None and Process.poll() is None:
                if os.name == "nt":
                    # Only the Popen-owned supervisor PID and its descendants;
                    # never a machine-wide image-name kill or another run.
                    Command = ["taskkill.exe", "/PID", str(Process.pid), "/T", "/F"]
                    Result = subprocess.run(Command, capture_output=True, timeout=8, check=False,
                                            creationflags=subprocess.CREATE_NO_WINDOW)
                    if Result.returncode != 0 and Process.poll() is None:
                        Failure = "owned process-tree stop failed"
                else:
                    Process.terminate()
                try:
                    Process.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    Process.kill()
                    Process.wait(timeout=5)
                    Failure = "owned supervisor required fallback kill"
        finally:
            for Stream in (self.Output, self.Error):
                if Stream is not None:
                    Stream.close()
            self.Output = self.Error = None
        if Failure:
            raise RuntimeError("[Qualification:FarmLifecycle] " + Failure)

    def Catalog(self):
        return workflow.Catalog({
            "farm.server-launch.v1": self.ServerLaunch,
            "farm.server-ready.v1": self.ServerReady,
            "farm.client-launch.v1": self.ClientLaunch,
            "farm.client-ready.v1": self.ClientReady,
            "farm.server-progress.v1": self.Progress,
            "farm.client-progress.v1": self.Progress,
            "farm.server-result.v1": self.Result,
            "farm.client-result.v1": self.Result,
        }, self.Cleanup)


def GetCatalog(ConfigPath):
    return FarmLifecycle(ConfigPath).Catalog()
