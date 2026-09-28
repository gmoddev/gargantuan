"""One bounded Foundation 2B lifecycle orchestration of the physical qualifier."""

import json
from pathlib import Path
import subprocess
import sys
import threading
import time
import uuid

ROOT = Path(r"C:\Sandbox\Codex\Workspaces\agent-coordinator-foundation-2-client")
CLIENT_CODEX = Path(r"C:\Users\aiden\AppData\Local\OpenAI\Codex\bin\d375f7df50d3b421\codex.exe")
CLIENT_PYTHON = Path(r"C:\Python312\python.exe")
EXPECTED_SID = "S-1-5-21-2820064101-3801502750-265446247-1004"
sys.path.insert(0, str(ROOT))
from agent_coordinator.control import Assignments, Host
from agent_coordinator.lifecycle.policy import Bootstrap
from agent_coordinator.lifecycle.service import Client
from agent_coordinator.lifecycle.secret import LoadKey
from agent_coordinator.transport import Journal
from agent_coordinator.workflow import Workflow


def Main():
    if len(sys.argv) != 2:
        raise ValueError("usage: run_one_client_lifecycle.py STAGED_ARTIFACT_DIRECTORY")
    Artifact = Path(sys.argv[1]).resolve(strict=True)
    Setup = json.loads((Artifact / "setup.json").read_text(encoding="utf-8"))
    Stage = json.loads((Artifact / "stage.json").read_text(encoding="utf-8"))
    Physical = Path(Stage["Physical"])
    if (Physical.parent != Path(r"C:\Sandbox\Codex\Evidence\physical-qualifier") or
            Physical.name != "lifecycle-" + Stage["Label"]):
        raise ValueError("unapproved physical evidence directory")
    PreflightScript = Path(__file__).with_name("evidence_preflight.py")
    PreflightCommand = [str(CLIENT_CODEX), "sandbox", "-P", ":workspace", "-p",
                        "foundation-2-endpoint", "-C", str(ROOT), str(CLIENT_PYTHON),
                        "-B", str(PreflightScript), str(Physical), Stage["RunId"]]
    Preflight = subprocess.run(PreflightCommand, text=True, capture_output=True, timeout=20)
    try:
        Proof = json.loads(Preflight.stdout.strip().splitlines()[-1])
    except (IndexError, ValueError) as Error:
        raise RuntimeError("restricted evidence preflight produced no proof: " +
                           Preflight.stderr[-300:]) from Error
    (Artifact / "client-evidence-preflight.json").write_text(
        json.dumps(Proof, indent=2) + "\n", encoding="utf-8")
    if (Preflight.returncode or not Proof.get("Success") or Proof.get("IsAdmin") or
            Proof.get("Sid") != EXPECTED_SID or Proof.get("RunId") != Stage["RunId"]):
        raise RuntimeError("restricted evidence preflight failed before endpoint wake")
    print("[Qualification:Evidence] Restricted endpoint preflight PASS before wake", flush=True)
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
    raise SystemExit(Main())
