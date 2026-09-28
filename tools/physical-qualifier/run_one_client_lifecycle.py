"""One bounded Foundation 2B lifecycle orchestration of the physical qualifier."""

import json
from pathlib import Path
import re
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
sys.path.insert(0, str(ROOT))
from agent_coordinator.control import Assignments, Host
from agent_coordinator.lifecycle.policy import Bootstrap
from agent_coordinator.lifecycle.service import Client
from agent_coordinator.lifecycle.secret import LoadKey
from agent_coordinator.transport import Journal
from agent_coordinator.workflow import Workflow


def RunClientPreflight(Stage, Artifact):
    Physical = Path(Stage["Physical"])
    Command = [str(CLIENT_CODEX), "sandbox", "-P", ":workspace", "-p",
               "foundation-2-endpoint", "-C", str(ROOT), str(CLIENT_PYTHON),
               "-B", str(Path(__file__).with_name("evidence_preflight.py")),
               "CLIENT", str(Physical), Stage["RunId"], CLIENT_SID]
    Process = subprocess.run(Command, text=True, capture_output=True, timeout=20)
    try:
        Proof = json.loads(Process.stdout.strip().splitlines()[-1])
    except (IndexError, ValueError) as Error:
        raise RuntimeError("client evidence preflight produced no proof: " +
                           Process.stderr[-300:]) from Error
    (Artifact / "client-evidence-preflight.json").write_text(
        json.dumps(Proof, indent=2) + "\n", encoding="utf-8")
    if Process.returncode:
        raise RuntimeError("client restricted evidence preflight failed before wake")
    RequireProof(Proof, Stage["RunId"], "CLIENT", CLIENT_SID)
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


def ReadStage(Artifact):
    Setup = json.loads((Artifact / "setup.json").read_text(encoding="utf-8"))
    Stage = json.loads((Artifact / "stage.json").read_text(encoding="utf-8"))
    Physical = Path(Stage["Physical"])
    if (Physical.parent != Path(r"C:\Sandbox\Codex\Evidence\physical-qualifier") or
            Physical.name != "lifecycle-" + Stage["Label"]):
        raise ValueError("unapproved physical evidence directory")
    return Setup, Stage


def Preflight(Artifact):
    _, Stage = ReadStage(Artifact)
    Proofs = RequireEvidenceReady(Stage, Artifact, RunClientPreflight, RunWorkerPreflight)
    Ready = {"RunId": Stage["RunId"], "TimestampUnixMs": time.time_ns() // 1000000,
             "Proofs": Proofs}
    (Artifact / "evidence-ready.json").write_text(
        json.dumps(Ready, indent=2) + "\n", encoding="utf-8")
    print("[Qualification:Evidence] Both restricted endpoint preflights PASS before lifecycle daemon start", flush=True)


def VerifyReady(Artifact, Stage):
    Ready = json.loads((Artifact / "evidence-ready.json").read_text(encoding="utf-8"))
    AgeMs = time.time_ns() // 1000000 - Ready["TimestampUnixMs"]
    if Ready["RunId"] != Stage["RunId"] or not 0 <= AgeMs <= 300000:
        raise ValueError("evidence readiness proof missing, stale or mismatched")
    for Role, Sid in (("CLIENT", CLIENT_SID), ("WORKER", WORKER_SID)):
        RequireProof(Ready["Proofs"][Role], Stage["RunId"], Role, Sid)
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


def Main(Artifact):
    Setup, Stage = ReadStage(Artifact)
    VerifyReady(Artifact, Stage)
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


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] not in ("preflight", "run"):
        raise SystemExit("usage: run_one_client_lifecycle.py preflight|run STAGED_ARTIFACT_DIRECTORY")
    Artifact = Path(sys.argv[2]).resolve(strict=True)
    raise SystemExit(Preflight(Artifact) if sys.argv[1] == "preflight" else Main(Artifact))
