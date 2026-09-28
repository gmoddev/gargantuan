"""One bounded Foundation 2B lifecycle orchestration of the physical qualifier."""

import json
import hashlib
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
import uuid

ROOT = Path(r"C:\Sandbox\Codex\Workspaces\agent-coordinator-foundation-2-client")
CLIENT_CODEX = Path(r"C:\Users\aiden\AppData\Local\OpenAI\Codex\bin\d375f7df50d3b421\codex.exe")
CLIENT_PYTHON = Path(r"C:\Python312\python.exe")
WORKER_ARTIFACT = "C:/Sandbox/Codex/Artifacts/gargantuan-3l-physical"
from evidence_gate import CLIENT_SID, WORKER_SID, RequireProof, RequireEvidenceReady
from lifecycle_tunnel import TunnelSession

INTERACTIVE_PROFILE = "PHYSICAL_QUALIFICATION_INTERACTIVE"
CLIENT_INTERACTIVE_SID = "S-1-5-21-2820064101-3801502750-265446247-1001"
WORKER_INTERACTIVE_SID = "S-1-5-21-455006656-4040886684-1921607991-1001"


def IsInteractive(Stage):
    Profile = Stage.get("QualificationProfile", "RESTRICTED")
    if Profile not in ("RESTRICTED", INTERACTIVE_PROFILE):
        raise ValueError("unknown physical qualification profile")
    return Profile == INTERACTIVE_PROFILE
sys.path.insert(0, str(ROOT))
from agent_coordinator.control import Assignments, Host
from agent_coordinator.lifecycle.policy import Bootstrap
from agent_coordinator.lifecycle.service import Client
from agent_coordinator.lifecycle.secret import LoadKey, Restrict
from agent_coordinator.transport import Journal
from agent_coordinator.workflow import Workflow


def RunClientPreflight(Stage, Artifact):
    Physical = Path(Stage["Physical"])
    if IsInteractive(Stage):
        Command = [str(CLIENT_PYTHON), "-B",
                   str(Path(__file__).with_name("evidence_preflight.py")),
                   "CLIENT", str(Physical), Stage["RunId"], CLIENT_INTERACTIVE_SID]
        Sid = CLIENT_INTERACTIVE_SID
    else:
        Command = [str(CLIENT_CODEX), "sandbox", "-P", ":workspace", "-p",
                   "foundation-2-endpoint", "-C", str(ROOT), str(CLIENT_PYTHON),
                   "-B", str(Path(__file__).with_name("evidence_preflight.py")),
                   "CLIENT", str(Physical), Stage["RunId"], CLIENT_SID]
        Sid = CLIENT_SID
    Process = subprocess.run(Command, text=True, capture_output=True, timeout=20)
    try:
        Proof = json.loads(Process.stdout.strip().splitlines()[-1])
    except (IndexError, ValueError) as Error:
        raise RuntimeError("client evidence preflight produced no proof: " +
                           Process.stderr[-300:]) from Error
    (Artifact / "client-evidence-preflight.json").write_text(
        json.dumps(Proof, indent=2) + "\n", encoding="utf-8")
    if Process.returncode:
        raise RuntimeError("client evidence preflight failed before wake")
    RequireProof(Proof, Stage["RunId"], "CLIENT", Sid)
    return Proof


def RunInteractiveWorkerPreflight(Stage, Artifact):
    Label, RunId = Stage["Label"], Stage["RunId"]
    if not re.fullmatch(r"[0-9a-f]{16}", Label) or str(uuid.UUID(RunId)) != RunId:
        raise ValueError("invalid interactive preflight identity")
    Source = Path(__file__).parent
    Names = ("evidence_preflight.py", "worker_interactive_preflight_launcher.py",
             "Start-WorkerInteractivePreflight.ps1", "Check-WorkerPhysicalIdle.ps1",
             "Check-WorkerControlTunnel.ps1")
    Remote = "dockerbox:" + WORKER_ARTIFACT + "/" + Label + "/"
    subprocess.run(["scp", "-q", *(str(Source / Name) for Name in Names), Remote],
                   check=True, capture_output=True, text=True, timeout=20)
    TaskName = "Gargantuan3L-InteractivePreflight-" + Label
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as Listener:
        Listener.bind(("192.168.0.68", 39451))
        Listener.listen(1)
        Listener.settimeout(35)
        try:
            Start = ["ssh", "-o", "BatchMode=yes", "dockerbox", "powershell",
                     "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                     WORKER_ARTIFACT + "/" + Label + "/Start-WorkerInteractivePreflight.ps1",
                     "-Label", Label, "-RunId", RunId]
            subprocess.run(Start, check=True, capture_output=True, text=True, timeout=20)
            Connection, Peer = Listener.accept()
            with Connection:
                Connection.settimeout(5)
                Challenge = json.loads(Connection.makefile("rb").readline(512))
                if (Peer[0] != "192.168.0.108" or Challenge.get("Label") != Label or
                        Challenge.get("RunId") != RunId or
                        not re.fullmatch(r"[0-9a-f]{32}", Challenge.get("Nonce", ""))):
                    raise ValueError("interactive worker LAN challenge mismatch")
                Connection.sendall((json.dumps({"Nonce": Challenge["Nonce"]}) + "\n").encode("ascii"))
            LocalProof = Artifact / "worker-interactive-preflight.json"
            Deadline = time.monotonic() + 35
            while time.monotonic() < Deadline:
                Copy = subprocess.run(["scp", "-q", Remote + LocalProof.name,
                                       str(LocalProof)], capture_output=True,
                                      text=True, timeout=5)
                if Copy.returncode == 0:
                    break
                time.sleep(0.25)
            else:
                raise TimeoutError("interactive worker preflight proof timed out")
        finally:
            subprocess.run(["ssh", "-o", "BatchMode=yes", "dockerbox", "schtasks",
                            "/Delete", "/TN", TaskName, "/F"],
                           capture_output=True, text=True, timeout=10)
    Proof = json.loads(LocalProof.read_text(encoding="utf-8"))
    RequireProof(Proof, RunId, "WORKER", WORKER_INTERACTIVE_SID)
    if (Proof.get("ExitCode") != 0 or Proof.get("Label") != Label or
            Proof.get("Lan") != {"Bind": ["192.168.0.108", 0],
                                 "Connect": ["192.168.0.68", 39451],
                                 "Nonce": Challenge["Nonce"], "Success": True}):
        raise RuntimeError("interactive worker LAN preflight failed")
    return Proof


def RunWorkerPreflight(Stage, Artifact):
    Label = Stage["Label"]
    RunId = Stage["RunId"]
    if not re.fullmatch(r"[0-9a-f]{16}", Label) or str(uuid.UUID(RunId)) != RunId:
        raise ValueError("invalid staged physical identity")
    Source = Path(__file__).parent
    Files = ("evidence_preflight.py", "worker_evidence_preflight_launcher.py",
             "Start-WorkerEvidencePreflight.ps1")
    Copy = ["scp", "-q", *(str(Source / Name) for Name in Files),
            "dockerbox:" + WORKER_ARTIFACT + "/"]
    subprocess.run(Copy, check=True, capture_output=True, text=True, timeout=20)
    TaskName = "Gargantuan3L-Evidence-" + Label
    Start = ["ssh", "-o", "BatchMode=yes", "dockerbox", "powershell", "-NoProfile",
             "-ExecutionPolicy", "Bypass", "-File",
             r"C:\Sandbox\Codex\Artifacts\gargantuan-3l-physical\Start-WorkerEvidencePreflight.ps1",
             "-Label", Label, "-RunId", RunId]
    subprocess.run(Start, check=True, capture_output=True, text=True, timeout=20)
    LocalProof = Artifact / "worker-evidence-preflight.json"
    RemoteProof = ("dockerbox:" + WORKER_ARTIFACT + "/" + Label +
                   "/worker-evidence-preflight.json")
    try:
        Deadline = time.monotonic() + 35
        while time.monotonic() < Deadline:
            CopyProof = subprocess.run(["scp", "-q", RemoteProof, str(LocalProof)],
                                       capture_output=True, text=True, timeout=5)
            if CopyProof.returncode == 0:
                break
            time.sleep(0.5)
        else:
            raise RuntimeError("worker restricted evidence preflight timed out before wake")
    finally:
        subprocess.run(["ssh", "-o", "BatchMode=yes", "dockerbox", "schtasks",
                        "/Delete", "/TN", TaskName, "/F"],
                       capture_output=True, text=True, timeout=10)
    Proof = json.loads(LocalProof.read_text(encoding="utf-8"))
    if Proof.get("ExitCode") != 0:
        raise RuntimeError("worker restricted evidence preflight failed before wake: " +
                           str(Proof.get("Error", Proof.get("Stderr", "")))[:300])
    RequireProof(Proof, RunId, "WORKER", WORKER_SID)
    return Proof


def Digest(File):
    with Path(File).open("rb") as Stream:
        return hashlib.file_digest(Stream, "sha256").hexdigest().upper()


def StopWorkerBroker(Stage):
    Label, RunId = Stage["Label"], Stage["RunId"]
    Command = ["ssh", "-o", "BatchMode=yes", "dockerbox", "powershell",
               "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
               WORKER_ARTIFACT + "/" + Label + "/Stop-WorkerSocketBroker.ps1",
               "-Label", Label, "-RunId", RunId]
    return subprocess.run(Command, capture_output=True, text=True, timeout=20)


def RunWorkerSocketPreflight(Stage, Artifact):
    Label, RunId = Stage["Label"], Stage["RunId"]
    if not re.fullmatch(r"[0-9a-f]{16}", Label) or str(uuid.UUID(RunId)) != RunId:
        raise ValueError("invalid worker socket identity")
    Source = Path(__file__).parent
    Config = Path(Stage["Physical"]) / "server.json"
    BrokerConfig = Artifact / "server-broker.json"
    shutil.copy2(Config, BrokerConfig)
    Restrict(BrokerConfig)
    Manifest = Artifact / "broker-manifest.json"
    Manifest.write_text(json.dumps({"Label": Label, "RunId": RunId,
                                    "ToolSHA256": Stage["ToolSHA256"],
                                    "ConfigSHA256": Digest(BrokerConfig),
                                    "PackageSHA256": Stage["PackageSHA256"]},
                                   indent=2) + "\n", encoding="utf-8")
    Restrict(Manifest)
    Files = ("worker_socket_broker.py", "worker_socket_preflight.py",
             "worker_socket_preflight_launcher.py", "Start-WorkerSocketBroker.ps1",
             "Start-WorkerSocketPreflight.ps1", "Stop-WorkerSocketBroker.ps1",
             "Check-WorkerControlTunnel.ps1", "Check-WorkerPhysicalIdle.ps1")
    Remote = "dockerbox:" + WORKER_ARTIFACT + "/" + Label + "/"
    subprocess.run(["scp", "-q", *(str(Source / Name) for Name in Files),
                    str(BrokerConfig), str(Manifest), Remote],
                   check=True, capture_output=True, text=True, timeout=20)
    PreflightTask = "Gargantuan3L-SocketPreflight-" + Label
    BrokerStarted = False
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as Listener:
        Listener.bind(("192.168.0.68", 39451))
        Listener.listen(1)
        Listener.settimeout(30)
        try:
            Start = ["ssh", "-o", "BatchMode=yes", "dockerbox", "powershell",
                     "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                     WORKER_ARTIFACT + "/" + Label + "/Start-WorkerSocketBroker.ps1",
                     "-Label", Label, "-ManifestSHA256", Digest(Manifest),
                     "-BrokerSHA256", Digest(Source / "worker_socket_broker.py")]
            BrokerStarted = True
            subprocess.run(Start, check=True, capture_output=True, text=True, timeout=20)
            Preflight = ["ssh", "-o", "BatchMode=yes", "dockerbox", "powershell",
                         "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                         WORKER_ARTIFACT + "/" + Label + "/Start-WorkerSocketPreflight.ps1",
                         "-Label", Label, "-RunId", RunId]
            subprocess.run(Preflight, check=True, capture_output=True, text=True, timeout=20)
            LocalProof = Artifact / "worker-socket-restricted-proof.json"
            RemoteProof = Remote + "worker-socket-restricted-proof.json"
            Deadline = time.monotonic() + 35
            while time.monotonic() < Deadline:
                Copy = subprocess.run(["scp", "-q", RemoteProof, str(LocalProof)],
                                      capture_output=True, text=True, timeout=5)
                if Copy.returncode == 0:
                    break
                time.sleep(0.25)
            else:
                raise TimeoutError("restricted worker socket proof timed out")
            Connection, Peer = Listener.accept()
            Connection.close()
            if Peer[0] != "192.168.0.108":
                raise ValueError("unexpected socket preflight peer")
            BrokerProof = Artifact / "worker-socket-broker-proof.json"
            subprocess.run(["scp", "-q", Remote + BrokerProof.name, str(BrokerProof)],
                           check=True, capture_output=True, text=True, timeout=5)
            Proof = json.loads(LocalProof.read_text(encoding="utf-8"))
            Broker = json.loads(BrokerProof.read_text(encoding="utf-8"))
            if (Proof.get("ExitCode") != 0 or not Proof.get("Success") or
                    Proof.get("Sid") != WORKER_SID or Proof.get("IsAdmin") or
                    Proof.get("RunId") != RunId or Proof.get("Label") != Label or
                    Proof.get("Socket") != Broker or not Broker.get("Success") or
                    Broker.get("Bind") != ["192.168.0.108", 0] or
                    Broker.get("Connect") != ["192.168.0.68", 39451]):
                raise RuntimeError("worker restricted socket capability preflight failed")
            return {"Restricted": Proof, "Broker": Broker}
        except BaseException:
            if BrokerStarted:
                StopWorkerBroker(Stage)
            raise
        finally:
            subprocess.run(["ssh", "-o", "BatchMode=yes", "dockerbox", "schtasks",
                            "/Delete", "/TN", PreflightTask, "/F"],
                           capture_output=True, timeout=10)


def ReadStage(Artifact):
    Setup = json.loads((Artifact / "setup.json").read_text(encoding="utf-8"))
    Stage = json.loads((Artifact / "stage.json").read_text(encoding="utf-8"))
    if IsInteractive(Stage):
        Expected = "physical-qualification-interactive"
        ClientConfig = json.loads(Path(Stage["ClientConfig"]).read_text(encoding="utf-8"))
        WorkerConfig = json.loads((Artifact / "server-daemon.json").read_text(encoding="utf-8"))
        WorkerPhysical = json.loads((Artifact / "server-physical.json").read_text(encoding="utf-8"))
        if (ClientConfig.get("Profile") != Expected or ClientConfig.get("EndpointId") != "CLIENT" or
                WorkerConfig.get("Profile") != Expected or WorkerConfig.get("EndpointId") != "SERVER" or
                WorkerPhysical.get("Role") != "SERVER" or "BrokerLabel" in WorkerPhysical or
                WorkerPhysical.get("RunId") != Stage["RunId"]):
            raise ValueError("interactive stage still depends on a restricted endpoint")
    Physical = Path(Stage["Physical"])
    if (Physical.parent != Path(r"C:\Sandbox\Codex\Evidence\physical-qualifier") or
            Physical.name != "lifecycle-" + Stage["Label"]):
        raise ValueError("unapproved physical evidence directory")
    return Setup, Stage


def Preflight(Artifact):
    _, Stage = ReadStage(Artifact)
    if IsInteractive(Stage):
        ClientProof = RunClientPreflight(Stage, Artifact)
        WorkerProof = RunInteractiveWorkerPreflight(Stage, Artifact)
        Proofs = {"CLIENT": ClientProof, "WORKER": WorkerProof}
        SocketProof = WorkerProof["Lan"]
    else:
        Proofs = RequireEvidenceReady(Stage, Artifact, RunClientPreflight, RunWorkerPreflight)
        SocketProof = RunWorkerSocketPreflight(Stage, Artifact)
    Ready = {"RunId": Stage["RunId"], "TimestampUnixMs": time.time_ns() // 1000000,
             "QualificationProfile": Stage.get("QualificationProfile", "RESTRICTED"),
             "Proofs": Proofs, "Socket": SocketProof}
    (Artifact / "evidence-ready.json").write_text(
        json.dumps(Ready, indent=2) + "\n", encoding="utf-8")
    print("[Qualification:Evidence] Both evidence and worker LAN preflights PASS before lifecycle daemon start", flush=True)


def VerifyReady(Artifact, Stage):
    Ready = json.loads((Artifact / "evidence-ready.json").read_text(encoding="utf-8"))
    AgeMs = time.time_ns() // 1000000 - Ready["TimestampUnixMs"]
    if Ready["RunId"] != Stage["RunId"] or not 0 <= AgeMs <= 300000:
        raise ValueError("evidence readiness proof missing, stale or mismatched")
    if Ready.get("QualificationProfile") != Stage.get("QualificationProfile", "RESTRICTED"):
        raise ValueError("qualification profile changed after preflight")
    Sids = (("CLIENT", CLIENT_INTERACTIVE_SID), ("WORKER", WORKER_INTERACTIVE_SID)) if IsInteractive(Stage) else (("CLIENT", CLIENT_SID), ("WORKER", WORKER_SID))
    for Role, Sid in Sids:
        RequireProof(Ready["Proofs"][Role], Stage["RunId"], Role, Sid)
    Socket = Ready.get("Socket", {})
    if IsInteractive(Stage):
        if (Ready["Proofs"]["WORKER"].get("Lan") != Socket or
                Socket.get("Success") is not True or
                Socket.get("Connect") != ["192.168.0.68", 39451]):
            raise ValueError("interactive worker LAN readiness proof missing")
    else:
        Restricted, Broker = Socket.get("Restricted", {}), Socket.get("Broker", {})
        if (not Restricted.get("Success") or Restricted.get("IsAdmin") or
                Restricted.get("Sid") != WORKER_SID or Restricted.get("RunId") != Stage["RunId"] or
                Restricted.get("Socket") != Broker or not Broker.get("Success") or
                Broker.get("Connect") != ["192.168.0.68", 39451]):
            raise ValueError("worker socket readiness proof missing or mismatched")
    Physical = Path(Stage["Physical"])
    ClientStamp = json.loads((Physical / "preflight.json").read_text(encoding="utf-8"))
    if ClientStamp != Ready["Proofs"]["CLIENT"]:
        raise ValueError("client evidence stamp changed after preflight")
    WorkerRun = (r"C:\GargantuanQualification\physical-qualifier-service-evidence\lifecycle-" +
                 Stage["Label"])
    WorkerPresence = subprocess.run(
        ["ssh", "-o", "BatchMode=yes", "dockerbox", "powershell", "-NoProfile",
         "-Command", "Test-Path -LiteralPath " + WorkerRun],
        check=True, capture_output=True, text=True, timeout=10)
    if WorkerPresence.stdout.strip() != "False":
        raise ValueError("worker evidence directory was used after preflight")
    return Ready


def RequireWorkerPhysicalIdle(Stage, Artifact):
    Command = ["ssh", "-o", "BatchMode=yes", "dockerbox", "powershell",
               "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
               WORKER_ARTIFACT + "/" + Stage["Label"] + "/Check-WorkerPhysicalIdle.ps1"]
    Result = subprocess.run(Command, capture_output=True, text=True, timeout=15)
    if Result.returncode:
        raise RuntimeError("worker physical idle preflight failed: " + Result.stderr[-300:])
    try:
        Report = json.loads(Result.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError) as Error:
        raise RuntimeError("worker physical idle report malformed") from Error
    if (Report.get("CaptureService") != "Running" or Report.get("CaptureActiveRun") is not False or
            Report.get("PacketMonitor") != "Stopped" or Report.get("PacketFilters") != "None" or
            Report.get("WindowsTrace") != "Stopped" or
            Report.get("Udp39450") != "Unbound" or
            not 0 <= time.time_ns() // 1000000 - Report.get("TimestampUnixMs", -1) <= 30000):
        raise ValueError("worker physical prerequisite is not idle")
    (Artifact / "worker-physical-idle.json").write_text(
        json.dumps(Report, indent=2) + "\n", encoding="utf-8")
    return Report


def RunLifecycle(Setup, Stage, Artifact, Tunnel):
    WorkflowItem = Workflow(json.loads(Path(Setup["WorkflowFile"]).read_text(encoding="utf-8")))
    NowMs = time.time_ns() // 1000000
    Notices = {Role: {"Version": 1, "EndpointId": Item["EndpointId"],
                      "RunId": str(uuid.uuid4()), "Generation": str(uuid.uuid4()),
                      "ExpiresUnixMs": NowMs + WorkflowItem.Value["RegistrationTimeout"] * 1000}
               for Role, Item in Setup["Endpoints"].items()}
    Keys = {Role: LoadKey(Item["KeyFile"]) for Role, Item in Setup["Endpoints"].items()}
    Local = {Role: {"EndpointId": Item["EndpointId"], "PeerIp": "127.0.0.1",
                    **Bootstrap(Notices[Role], Keys[Role])}
             for Role, Item in Setup["Endpoints"].items()}
    Assignment = Assignments(WorkflowItem, Local, {})
    for Role, Item in Setup["Endpoints"].items():
        Notices[Role]["RunId"] = Assignment.RunId
        Assignment.Entries[Item["EndpointId"]]["Local"].update(Bootstrap(Notices[Role], Keys[Role]))
    Clients = {Role: Client("127.0.0.1", Item["LifecyclePort"], Item["EndpointId"], Keys[Role])
               for Role, Item in Setup["Endpoints"].items()}
    Evidence = Path(Setup["Evidence"]) / Assignment.RunId
    Evidence.mkdir(parents=True, exist_ok=False)
    Ready = threading.Event()
    Codes = {}
    Samples = []

    def Coordinator():
        Codes["Host"] = Host("127.0.0.1", Setup["ControlPort"], Assignment,
                             Journal(Evidence / "host"), lambda Port: Ready.set())

    Thread = threading.Thread(target=Coordinator, name="PhysicalLifecycleHost")
    Thread.start()
    try:
        if not Ready.wait(3):
            raise RuntimeError("Agent Coordinator host did not listen")
        for Role, Item in Clients.items():
            Presence = Item.GetPresence()
            Samples.append({"Role": Role, "Stage": "before", "Presence": Presence})
            if Presence["Status"] != "OFFLINE":
                raise ValueError("physical endpoint agent is not offline")
        Tunnel.RequireAlive()
        for Role, Item in Clients.items():
            Item.StartAgent(Notices[Role])
        Deadline = time.monotonic() + 250
        Final = {}
        while time.monotonic() < Deadline and len(Final) < len(Clients):
            for Role, Item in Clients.items():
                if Role in Final:
                    continue
                Presence = Item.GetAgentStatus(Notices[Role]["Generation"])
                if Presence["Status"] in ("IDLE", "FAILED", "NEEDS_USER"):
                    Final[Role] = Presence
                Samples.append({"Role": Role, "Stage": "observed", "Presence": Presence})
            if Codes.get("Host") == 1 and all(Role in Final for Role in Clients):
                break
            time.sleep(0.5)
        Thread.join(5)
        Success = (not Thread.is_alive() and Codes.get("Host") == 0 and
                   all(Final.get(Role, {}).get("Status") == "IDLE" for Role in Clients))
        Report = {"Success": Success, "LifecycleRunId": Assignment.RunId,
                  "Codes": Codes, "Final": Final, "Samples": Samples}
        (Evidence / "qualification.json").write_text(json.dumps(Report, indent=2) + "\n", encoding="utf-8")
        print("[Coordinator:Lifecycle] " + ("PASS" if Success else "FAIL") + " " + str(Evidence), flush=True)
        return 0 if Success else 1
    finally:
        for Role, Item in Clients.items():
            try:
                Item.StopAgent(Notices[Role]["Generation"])
            except (OSError, ValueError):
                pass
        Thread.join(WorkflowItem.Value["RegistrationTimeout"] + 2)


def Main(Artifact):
    Setup, Stage = ReadStage(Artifact)
    try:
        VerifyReady(Artifact, Stage)
        RequireWorkerPhysicalIdle(Stage, Artifact)
        ClientItem = Setup["Endpoints"]["CLIENT"]
        ClientPresence = Client("127.0.0.1", ClientItem["LifecyclePort"],
                                ClientItem["EndpointId"], LoadKey(ClientItem["KeyFile"])).GetPresence()
        if (ClientPresence.get("EndpointId") != ClientItem["EndpointId"] or
                ClientPresence.get("Status") != "OFFLINE"):
            raise ValueError("client lifecycle endpoint is not ready before assignment")
        with TunnelSession(Setup, Stage, Artifact) as Tunnel:
            ServerKey = LoadKey(Setup["Endpoints"]["SERVER"]["KeyFile"])
            Tunnel.Preflight(ServerKey, Client)
            print("[Qualification:Tunnel] Forward and reverse handshakes PASS before assignment", flush=True)
            return RunLifecycle(Setup, Stage, Artifact, Tunnel)
    finally:
        if not IsInteractive(Stage):
            Clean = StopWorkerBroker(Stage)
            if Clean.returncode:
                if sys.exc_info()[0] is None:
                    raise RuntimeError("worker broker cleanup failed: " + Clean.stderr[-300:])
                print("[Qualification:Socket] Worker broker cleanup failed: " +
                      Clean.stderr[-300:], flush=True)


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] not in ("preflight", "run"):
        raise SystemExit("usage: run_one_client_lifecycle.py preflight|run STAGED_ARTIFACT_DIRECTORY")
    Artifact = Path(sys.argv[2]).resolve(strict=True)
    raise SystemExit(Preflight(Artifact) if sys.argv[1] == "preflight" else Main(Artifact))
