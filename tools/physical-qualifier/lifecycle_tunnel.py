"""Host-owned, one-run SSH topology and two-direction lifecycle preflight."""

import json
from pathlib import Path
import re
import socket
import subprocess
import threading
import time
import uuid

from socket_gate import WorkerReverseListener


WORKER_ARTIFACT = "C:/Sandbox/Codex/Artifacts/gargantuan-3l-physical"
WORKER_SID = "S-1-5-21-455006656-4040886684-1921607991-1006"
WORKER_INTERACTIVE_SID = "S-1-5-21-455006656-4040886684-1921607991-1001"
INTERACTIVE_PROFILE = "PHYSICAL_QUALIFICATION_INTERACTIVE"
TOPOLOGY = {
    "Forward": {"ListenerHost": "main", "Bind": "127.0.0.1", "Port": 49964,
                "TargetHost": "worker", "TargetBind": "127.0.0.1", "TargetPort": 49963,
                "Consumer": "main lifecycle coordinator", "TargetOwner": "worker lifecycle daemon"},
    "Reverse": {"ListenerHost": "worker", "Bind": "127.0.0.1", "Port": 49961,
                "TargetHost": "main", "TargetBind": "127.0.0.1", "TargetPort": 49961,
                "Consumer": "worker lifecycle endpoint", "TargetOwner": "main coordinator"},
}


def ValidateTopology(Setup):
    if (Setup.get("ControlPort") != 49961 or
            Setup.get("Endpoints", {}).get("SERVER", {}).get("LifecyclePort") != 49964 or
            Setup.get("Endpoints", {}).get("CLIENT", {}).get("LifecyclePort") != 49962):
        raise ValueError("lifecycle setup does not match the fixed SSH topology")


def TunnelCommand():
    return ["ssh", "-N", "-T", "-o", "BatchMode=yes",
            "-o", "ExitOnForwardFailure=yes",
            "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=2",
            "-L", "127.0.0.1:49964:127.0.0.1:49963",
            "-R", "127.0.0.1:49961:127.0.0.1:49961", "dockerbox"]


def FreeLocalPort(Port):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as Probe:
        Probe.bind(("127.0.0.1", Port))


def ServeChallenge(Listener, Label, RunId):
    Connection, Peer = Listener.accept()
    if Peer[0] != "127.0.0.1":
        Connection.close()
        raise ValueError("reverse tunnel reached from non-loopback peer")
    with Connection:
        Connection.settimeout(5)
        Request = json.loads(Connection.makefile("rb").readline(512))
        WorkerNonce = Request.get("WorkerNonce", "")
        if (Request.get("Version") != 1 or Request.get("Label") != Label or
                Request.get("RunId") != RunId or
                not re.fullmatch(r"[0-9a-f]{32}", WorkerNonce)):
            raise ValueError("reverse tunnel request has wrong run or endpoint")
        HostNonce = uuid.uuid4().hex
        Response = {"Version": 1, "Label": Label, "RunId": RunId,
                    "WorkerNonce": WorkerNonce, "HostNonce": HostNonce}
        Connection.sendall((json.dumps(Response) + "\n").encode("ascii"))
        Echo = json.loads(Connection.makefile("rb").readline(128))
        if Echo != {"HostNonce": HostNonce}:
            raise ValueError("reverse tunnel challenge echo mismatch")
    return {"Listener": "127.0.0.1:49961", "Peer": Peer[0],
            "WorkerNonce": WorkerNonce, "HostNonce": HostNonce,
            "CompletedUnixMs": time.time_ns() // 1000000}


def LaunchRestrictedReverseProbe(Stage, Artifact):
    Label, RunId = Stage["Label"], Stage["RunId"]
    Source = Path(__file__).parent
    Remote = "dockerbox:" + WORKER_ARTIFACT + "/" + Label + "/"
    Files = ("worker_tunnel_preflight.py", "worker_tunnel_preflight_launcher.py",
             "Start-WorkerTunnelPreflight.ps1")
    subprocess.run(["scp", "-q", *(str(Source / Name) for Name in Files), Remote],
                   check=True, capture_output=True, text=True, timeout=20)
    TaskName = "Gargantuan3L-TunnelPreflight-" + Label
    try:
        Command = ["ssh", "-o", "BatchMode=yes", "dockerbox", "powershell",
                        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                        WORKER_ARTIFACT + "/" + Label + "/Start-WorkerTunnelPreflight.ps1",
                        "-Label", Label, "-RunId", RunId]
        Interactive = Stage.get("QualificationProfile") == INTERACTIVE_PROFILE
        if Interactive:
            Command.append("-Interactive")
        Started = subprocess.run(Command, capture_output=True, text=True, timeout=20)
        if Started.returncode:
            raise RuntimeError("worker reverse preflight refused: " +
                               (Started.stderr.strip() or Started.stdout.strip())[-300:])
        Output = Artifact / ("worker-tunnel-interactive-proof.json" if Interactive else
                             "worker-tunnel-restricted-proof.json")
        Deadline = time.monotonic() + 30
        while time.monotonic() < Deadline:
            Copy = subprocess.run(["scp", "-q", Remote + Output.name, str(Output)],
                                  capture_output=True, text=True, timeout=5)
            if Copy.returncode == 0:
                return json.loads(Output.read_text(encoding="utf-8"))
            time.sleep(0.25)
        raise TimeoutError("worker reverse handshake proof timed out")
    finally:
        subprocess.run(["ssh", "-o", "BatchMode=yes", "dockerbox", "schtasks",
                        "/Delete", "/TN", TaskName, "/F"],
                       capture_output=True, text=True, timeout=10)


def RequireReverseProof(Worker, Host, Stage, StartedUnixMs):
    Sid = WORKER_INTERACTIVE_SID if Stage.get("QualificationProfile") == INTERACTIVE_PROFILE else WORKER_SID
    if (Worker.get("ExitCode") != 0 or Worker.get("Success") is not True or
            Worker.get("Label") != Stage["Label"] or
            Worker.get("RunId") != Stage["RunId"] or
            Worker.get("Sid") != Sid or Worker.get("IsAdmin") is not False or
            Worker.get("Address") != "127.0.0.1" or Worker.get("Port") != 49961 or
            Worker.get("WorkerNonce") != Host.get("WorkerNonce") or
            Worker.get("HostNonce") != Host.get("HostNonce") or
            not StartedUnixMs <= Worker.get("StartedUnixMs", -1) <=
            Worker.get("CompletedUnixMs", -1) <= time.time_ns() // 1000000):
        raise ValueError("reverse tunnel proof missing, stale or mismatched")


class TunnelSession:
    def __init__(self, Setup, Stage, Artifact):
        ValidateTopology(Setup)
        if not re.fullmatch(r"[0-9a-f]{16}", Stage["Label"]):
            raise ValueError("invalid tunnel stage label")
        self.Setup, self.Stage, self.Artifact = Setup, Stage, Path(Artifact)
        self.Process = None
        self.Log = None
        self.StartedUnixMs = None
        self.SessionId = uuid.uuid4().hex

    def __enter__(self):
        Marker = self.Artifact / "tunnel-session.json"
        with Marker.open("x", encoding="utf-8") as Stream:
            json.dump({"SessionId": self.SessionId, "PhysicalRunId": self.Stage["RunId"],
                       "Topology": TOPOLOGY, "State": "STARTING"}, Stream, indent=2)
        try:
            FreeLocalPort(49964)
            if WorkerReverseListener(self.Stage["Label"], WORKER_ARTIFACT)["Present"]:
                raise RuntimeError("stale worker reverse listener blocks fresh tunnel")
            self.Log = (self.Artifact / "tunnel-ssh.log").open("xb")
            self.StartedUnixMs = time.time_ns() // 1000000
            self.Process = subprocess.Popen(TunnelCommand(), stdin=subprocess.DEVNULL,
                                            stdout=self.Log, stderr=subprocess.STDOUT,
                                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            Deadline = time.monotonic() + 10
            while time.monotonic() < Deadline:
                if self.Process.poll() is not None:
                    raise RuntimeError("fixed SSH tunnel exited before readiness")
                if WorkerReverseListener(self.Stage["Label"], WORKER_ARTIFACT)["Present"]:
                    break
                time.sleep(0.2)
            else:
                raise TimeoutError("worker reverse listener did not appear")
            Marker.write_text(json.dumps({"SessionId": self.SessionId,
                                          "PhysicalRunId": self.Stage["RunId"],
                                          "Topology": TOPOLOGY, "Pid": self.Process.pid,
                                          "StartedUnixMs": self.StartedUnixMs,
                                          "State": "ACTIVE"}, indent=2) + "\n",
                              encoding="utf-8")
            return self
        except BaseException:
            try:
                self.Stop()
            except Exception as CleanupError:
                print("[Qualification:Tunnel] Cleanup after startup failure: " + str(CleanupError),
                      flush=True)
            raise

    def Preflight(self, Key, ClientClass):
        Worker = self.Setup["Endpoints"]["SERVER"]
        MainClient = ClientClass("127.0.0.1", 49964, Worker["EndpointId"], Key)
        Presence = MainClient.GetPresence()
        if Presence.get("Status") != "OFFLINE" or Presence.get("EndpointId") != Worker["EndpointId"]:
            raise ValueError("main forward reached wrong or non-idle worker endpoint")
        Forward = {"Address": "127.0.0.1", "Port": 49964,
                   "Target": "127.0.0.1:49963", "EndpointId": Worker["EndpointId"],
                   "Status": Presence["Status"], "ListenerOwnerPid": self.Process.pid,
                   "CompletedUnixMs": time.time_ns() // 1000000}
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as Listener:
            Listener.bind(("127.0.0.1", 49961))
            Listener.listen(1)
            Listener.settimeout(0.25)
            Result = {}

            def WorkerProbe():
                try:
                    Result["Proof"] = LaunchRestrictedReverseProbe(self.Stage, self.Artifact)
                except BaseException as Error:
                    Result["Error"] = Error

            Thread = threading.Thread(target=WorkerProbe, name="WorkerReverseProbe")
            Thread.start()
            try:
                Deadline = time.monotonic() + 30
                while True:
                    try:
                        Host = ServeChallenge(Listener, self.Stage["Label"], self.Stage["RunId"])
                        break
                    except socket.timeout:
                        if "Error" in Result:
                            raise RuntimeError("worker reverse probe failed: " + str(Result["Error"]))
                        if time.monotonic() >= Deadline:
                            raise TimeoutError("worker reverse handshake timed out")
            finally:
                Thread.join(35)
            if Thread.is_alive():
                raise TimeoutError("worker reverse probe did not finish")
            if "Error" in Result:
                raise RuntimeError("worker reverse probe failed: " + str(Result["Error"]))
            Reverse = Result["Proof"]
            RequireReverseProof(Reverse, Host, self.Stage, self.StartedUnixMs)
        self.RequireAlive()
        Proof = {"SessionId": self.SessionId, "PhysicalRunId": self.Stage["RunId"],
                 "StartedUnixMs": self.StartedUnixMs, "Topology": TOPOLOGY,
                 "Forward": Forward, "Reverse": Reverse,
                 "ReverseHost": Host,
                 "ReverseListener": WorkerReverseListener(self.Stage["Label"], WORKER_ARTIFACT)}
        (self.Artifact / "tunnel-preflight.json").write_text(
            json.dumps(Proof, indent=2) + "\n", encoding="utf-8")
        return Proof

    def RequireAlive(self):
        if self.Process is None or self.Process.poll() is not None:
            raise RuntimeError("one-run SSH tunnel ended before endpoint wake")
        if not WorkerReverseListener(self.Stage["Label"], WORKER_ARTIFACT)["Present"]:
            raise RuntimeError("worker reverse listener disappeared before wake")

    def Stop(self):
        if self.Process is not None and self.Process.poll() is None:
            self.Process.terminate()
            try:
                self.Process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.Process.kill()
                self.Process.wait(timeout=5)
        if self.Log is not None:
            self.Log.close()
        if self.Process is None:
            return
        Deadline = time.monotonic() + 10
        while time.monotonic() < Deadline:
            try:
                FreeLocalPort(49964)
                Clear = not WorkerReverseListener(self.Stage["Label"], WORKER_ARTIFACT)["Present"]
            except (OSError, RuntimeError):
                Clear = False
            if Clear:
                break
            time.sleep(0.25)
        Report = {"SessionId": self.SessionId, "PhysicalRunId": self.Stage["RunId"],
                  "StoppedUnixMs": time.time_ns() // 1000000,
                  "SshExited": self.Process is None or self.Process.poll() is not None,
                  "BothListenersClear": Clear}
        (self.Artifact / "tunnel-cleanup.json").write_text(
            json.dumps(Report, indent=2) + "\n", encoding="utf-8")
        Marker = self.Artifact / "tunnel-session.json"
        if Marker.exists():
            State = json.loads(Marker.read_text(encoding="utf-8"))
            if State.get("SessionId") == self.SessionId:
                State.update({"State": "STOPPED" if Clear else "CLEANUP_FAILED",
                              "StoppedUnixMs": Report["StoppedUnixMs"]})
                Marker.write_text(json.dumps(State, indent=2) + "\n", encoding="utf-8")
        if not Clear:
            raise RuntimeError("owned tunnel cleanup left a forward or reverse listener")

    def __exit__(self, _Type, _Value, _Traceback):
        try:
            self.Stop()
        except Exception as CleanupError:
            if _Type is None:
                raise
            print("[Qualification:Tunnel] Cleanup after lifecycle failure: " +
                  str(CleanupError), flush=True)
