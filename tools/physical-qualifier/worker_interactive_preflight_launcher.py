"""One-use evidence and LAN preflight in the logged-in worker user session."""

import json
from pathlib import Path
import re
import socket
import sys
import time
import uuid

from evidence_preflight import Probe


ARTIFACTS = Path(r"C:\Sandbox\Codex\Artifacts\gargantuan-3l-physical")
EVIDENCE = Path(r"C:\GargantuanQualification\physical-qualifier-service-evidence")
CONFIGS = Path(r"C:\Sandbox\Codex\Evidence\physical-qualifier")
WORKER_SID = "S-1-5-21-455006656-4040886684-1921607991-1001"


def Main(Label, RunId):
    if not re.fullmatch(r"[0-9a-f]{16}", Label) or str(uuid.UUID(RunId)) != RunId:
        raise ValueError("invalid interactive preflight identity")
    Output = ARTIFACTS / Label / "worker-interactive-preflight.json"
    if Output.exists():
        raise ValueError("interactive worker preflight already consumed")
    try:
        Report = Probe(EVIDENCE / ("lifecycle-" + Label), RunId, "WORKER",
                       WORKER_SID, CONFIGS / ("lifecycle-" + Label) / "server.json")
        if not Report["Success"]:
            raise RuntimeError("worker evidence write check failed: " +
                               str(Report.get("FailureStep")))
        Nonce = uuid.uuid4().hex
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as Connection:
            Connection.bind(("192.168.0.108", 0))
            Connection.settimeout(5)
            Connection.connect(("192.168.0.68", 39451))
            Connection.sendall((json.dumps({"Label": Label, "RunId": RunId,
                                            "Nonce": Nonce}) + "\n").encode("ascii"))
            Answer = json.loads(Connection.makefile("rb").readline(512))
            if Answer != {"Nonce": Nonce}:
                raise ValueError("main LAN challenge echo mismatch")
        Report["Lan"] = {"Bind": ["192.168.0.108", 0],
                         "Connect": ["192.168.0.68", 39451],
                         "Nonce": Nonce, "Success": True}
        Report["Label"] = Label
        Report["CompletedUnixMs"] = time.time_ns() // 1000000
        Report["ExitCode"] = 0
    except Exception as Error:
        Report = {"Success": False, "Label": Label, "RunId": RunId,
                  "Error": str(Error), "ExitCode": 1}
    Output.write_text(json.dumps(Report, indent=2) + "\n", encoding="utf-8")
    return Report["ExitCode"]


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: worker_interactive_preflight_launcher.py LABEL RUN_ID")
    raise SystemExit(Main(sys.argv[1], sys.argv[2]))
