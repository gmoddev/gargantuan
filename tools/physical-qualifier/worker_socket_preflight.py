"""Request one fixed socket check from inside the worker's real Codex sandbox."""

import csv
import ctypes
import json
from pathlib import Path
import re
import subprocess
import sys
import time
import uuid


WORKSPACE = Path(r"C:\Sandbox\Codex\Workspaces\agent-coordinator-foundation-2-server")
WORKER_SID = "S-1-5-21-455006656-4040886684-1921607991-1006"


def Probe(Label, RunId):
    if not re.fullmatch(r"[0-9a-f]{16}", Label) or str(uuid.UUID(RunId)) != RunId:
        raise ValueError("invalid socket preflight identity")
    Identity = next(csv.reader([subprocess.check_output(
        ["whoami", "/user", "/fo", "csv", "/nh"], text=True).strip()]))
    IsAdmin = bool(ctypes.windll.shell32.IsUserAnAdmin())
    Report = {"Label": Label, "RunId": RunId, "Identity": Identity[0],
              "Sid": Identity[1], "IsAdmin": IsAdmin, "Success": False}
    if Identity[1] != WORKER_SID or IsAdmin:
        raise PermissionError("socket preflight did not run as restricted worker")
    Prefix = WORKSPACE / ".lifecycle" / ("physical-broker-" + Label)
    Request = Path(str(Prefix) + ".preflight.json")
    ResultFile = Path(str(Prefix) + ".preflight-result.json")
    if Request.exists() or ResultFile.exists():
        raise ValueError("socket preflight already consumed")
    with Request.open("x", encoding="utf-8") as Stream:
        json.dump({"RunId": RunId, "Action": "PREFLIGHT"}, Stream)
    Deadline = time.monotonic() + 15
    while time.monotonic() < Deadline:
        if ResultFile.is_file():
            Result = json.loads(ResultFile.read_text(encoding="utf-8"))
            Report["Success"] = (Result.get("Success") is True and
                                 Result.get("RunId") == RunId and
                                 Result.get("Label") == Label)
            Report["Socket"] = Result
            return Report
        time.sleep(0.05)
    raise TimeoutError("worker socket broker did not answer restricted request")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: worker_socket_preflight.py LABEL RUN_ID")
    Result = Probe(sys.argv[1], sys.argv[2])
    print(json.dumps(Result, sort_keys=True), flush=True)
    raise SystemExit(0 if Result["Success"] else 1)
