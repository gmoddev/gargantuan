"""One-run local worker capability for the pinned physical qualifier endpoint.

The restricted Codex catalog can request only PREFLIGHT and START for one
pre-staged run. A limited interactive host process owns the LAN socket and the
physical endpoint; no command, address, port, or path comes from a request.
"""

import hashlib
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import time
import traceback
import uuid


WORKSPACE = Path(r"C:\Sandbox\Codex\Workspaces\agent-coordinator-foundation-2-server")
ARTIFACTS = Path(r"C:\Sandbox\Codex\Artifacts\gargantuan-3l-physical")
PYTHON = Path(r"C:\Sandbox\Codex\Tools\physical-qualifier\runtime\python.exe")
CONTROL_HOST = "192.168.0.68"
WORKER_HOST = "192.168.0.108"
CONTROL_PORT = 39451


def Digest(File):
    with Path(File).open("rb") as Stream:
        return hashlib.file_digest(Stream, "sha256").hexdigest().upper()


def Paths(Label):
    if not re.fullmatch(r"[0-9a-f]{16}", Label):
        raise ValueError("invalid broker label")
    Prefix = WORKSPACE / ".lifecycle" / ("physical-broker-" + Label)
    return {Name: Path(str(Prefix) + "." + Name + ".json")
            for Name in ("preflight", "preflight-result", "start", "start-result", "cancel")}


def ToolPath(Manifest):
    return Path(Manifest).parent / "broker-tool" / "physical-qualifier" / "qualifier.py"


def ValidateManifest(Manifest, ExpectedHash):
    Manifest = Path(Manifest)
    if Manifest.parent.parent != ARTIFACTS or Manifest.name != "broker-manifest.json":
        raise ValueError("unapproved broker manifest")
    Label = Manifest.parent.name
    Paths(Label)
    if Digest(Manifest) != ExpectedHash:
        raise ValueError("broker manifest pin changed")
    Item = json.loads(Manifest.read_text(encoding="utf-8"))
    if set(Item) != {"Label", "RunId", "ToolSHA256", "ConfigSHA256",
                     "PackageSHA256"} or Item["Label"] != Label:
        raise ValueError("invalid broker manifest fields")
    if str(uuid.UUID(Item["RunId"])) != Item["RunId"]:
        raise ValueError("invalid broker run ID")
    Config = Manifest.parent / "server-broker.json"
    Tool = ToolPath(Manifest)
    Package = Manifest.parent / "physical-catalog.zip"
    if (Digest(Config) != Item["ConfigSHA256"] or
            Digest(Tool) != Item["ToolSHA256"] or
            Digest(Package) != Item["PackageSHA256"]):
        raise ValueError("broker input pin changed")
    if not PYTHON.is_file():
        raise ValueError("worker qualifier Python runtime missing")
    Server = json.loads(Config.read_text(encoding="utf-8"))
    if (Server.get("RunId") != Item["RunId"] or Server.get("Role") != "SERVER" or
            Server.get("CoordinatorHost") != CONTROL_HOST or
            Server.get("Port") != CONTROL_PORT or
            Server.get("PeerIps", {}).get("SERVER") != WORKER_HOST or
            Server.get("EvidenceDir") != str(Path(
                r"C:\GargantuanQualification\physical-qualifier-service-evidence") /
                ("lifecycle-" + Label))):
        raise ValueError("broker endpoint/port/role/evidence mismatch")
    return Item, Config, Tool


def CheckRequest(File, RunId, Action):
    Row = json.loads(File.read_text(encoding="utf-8"))
    if Row != {"RunId": RunId, "Action": Action}:
        raise ValueError("unapproved broker request")


def SocketPreflight():
    Report = {"Family": "AF_INET", "Type": "SOCK_STREAM", "Protocol": "TCP",
              "Bind": [WORKER_HOST, 0], "Connect": [CONTROL_HOST, CONTROL_PORT],
              "Options": ["timeout=3"], "Success": False}
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM, 0) as Sock:
        Report["Operation"] = "bind"
        Sock.bind((WORKER_HOST, 0))
        Report["Local"] = Sock.getsockname()
        Report["Operation"] = "settimeout"
        Sock.settimeout(3)
        Report["Operation"] = "connect"
        Sock.connect((CONTROL_HOST, CONTROL_PORT))
        Report["Peer"] = Sock.getpeername()
        Report["Success"] = True
        Report["Operation"] = "close"
    return Report


def AtomicWrite(File, Row):
    Temporary = File.with_suffix(".tmp")
    if File.exists() or Temporary.exists():
        raise ValueError("broker result already exists")
    Temporary.write_text(json.dumps(Row, sort_keys=True) + "\n", encoding="utf-8")
    Temporary.replace(File)


def Serve(Manifest, ExpectedHash):
    Item, Config, Tool = ValidateManifest(Manifest, ExpectedHash)
    PathsForRun = Paths(Item["Label"])
    Proof = Path(Manifest).parent / "worker-socket-broker-proof.json"
    if Proof.exists() or any(PathsForRun[Name].exists() for Name in
                             ("preflight-result", "start", "start-result", "cancel")):
        raise ValueError("broker run already consumed")
    Deadline = time.monotonic() + 540
    PreflightDone = False
    Process = None
    try:
        while time.monotonic() < Deadline:
            if PathsForRun["cancel"].exists():
                CheckRequest(PathsForRun["cancel"], Item["RunId"], "CANCEL")
                if Process is not None and Process.poll() is None:
                    subprocess.run(["taskkill", "/T", "/F", "/PID", str(Process.pid)],
                                   capture_output=True, timeout=10)
                    Process.wait(timeout=5)
                return 1
            if not PreflightDone and PathsForRun["preflight"].exists():
                CheckRequest(PathsForRun["preflight"], Item["RunId"], "PREFLIGHT")
                ValidateManifest(Manifest, ExpectedHash)
                try:
                    Result = SocketPreflight()
                except OSError as Error:
                    Result = {"Success": False, "Operation": "socket-preflight",
                              "WinError": Error.winerror, "Error": str(Error)}
                Result.update({"RunId": Item["RunId"], "Label": Item["Label"],
                               "Executable": str(PYTHON), "BrokerPid": os.getpid()})
                AtomicWrite(Proof, Result)
                AtomicWrite(PathsForRun["preflight-result"], Result)
                PreflightDone = True
                if not Result["Success"]:
                    return 1
            if PreflightDone and Process is None and PathsForRun["start"].exists():
                CheckRequest(PathsForRun["start"], Item["RunId"], "START")
                ValidateManifest(Manifest, ExpectedHash)
                Flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
                with (Path(Manifest).parent / "worker-endpoint.stdout.log").open("wb") as Out, \
                        (Path(Manifest).parent / "worker-endpoint.stderr.log").open("wb") as Err:
                    Process = subprocess.Popen(
                        [str(PYTHON), "-B", "-u", str(Tool), "endpoint", str(Config)],
                        cwd=str(Tool.parent), stdin=subprocess.DEVNULL,
                        stdout=Out, stderr=Err, creationflags=Flags)
                AtomicWrite(Path(Manifest).parent / "worker-endpoint-process.json",
                            {"RunId": Item["RunId"], "Pid": Process.pid,
                             "Executable": str(PYTHON), "Config": str(Config)})
            if Process is not None and Process.poll() is not None:
                Result = {"RunId": Item["RunId"], "ReturnCode": Process.returncode,
                          "EndpointPid": Process.pid, "Success": Process.returncode == 0}
                AtomicWrite(PathsForRun["start-result"], Result)
                return 0 if Result["Success"] else 1
            time.sleep(0.05)
        raise TimeoutError("worker socket broker lease expired")
    finally:
        if Process is not None and Process.poll() is None:
            subprocess.run(["taskkill", "/T", "/F", "/PID", str(Process.pid)],
                           capture_output=True, timeout=10)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: worker_socket_broker.py MANIFEST EXPECTED_SHA256")
    try:
        raise SystemExit(Serve(sys.argv[1], sys.argv[2]))
    except Exception as Error:
        Failure = {"Success": False, "Error": str(Error),
                   "Stack": traceback.format_exc()}
        Path(sys.argv[1]).parent.joinpath("worker-socket-broker-error.json").write_text(
            json.dumps(Failure, indent=2) + "\n", encoding="utf-8")
        raise SystemExit(1)
