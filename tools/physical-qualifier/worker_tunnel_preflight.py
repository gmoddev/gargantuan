"""One-use reverse handshake from the installed restricted worker sandbox."""

import csv
import ctypes
import json
import re
import socket
import subprocess
import sys
import time
import uuid


WORKER_SID = "S-1-5-21-455006656-4040886684-1921607991-1006"
REVERSE_ADDRESS = ("127.0.0.1", 49961)


def Probe(Label, RunId):
    if not re.fullmatch(r"[0-9a-f]{16}", Label) or str(uuid.UUID(RunId)) != RunId:
        raise ValueError("invalid reverse tunnel identity")
    Identity = next(csv.reader([subprocess.check_output(
        ["whoami", "/user", "/fo", "csv", "/nh"], text=True).strip()]))
    IsAdmin = bool(ctypes.windll.shell32.IsUserAnAdmin())
    if Identity[1] != WORKER_SID or IsAdmin:
        raise PermissionError("reverse probe did not run as restricted worker")
    WorkerNonce = uuid.uuid4().hex
    Started = time.time_ns() // 1000000
    with socket.create_connection(REVERSE_ADDRESS, timeout=5) as Connection:
        Connection.settimeout(5)
        Request = {"Version": 1, "Label": Label, "RunId": RunId,
                   "WorkerNonce": WorkerNonce}
        Connection.sendall((json.dumps(Request) + "\n").encode("ascii"))
        Response = json.loads(Connection.makefile("rb").readline(512))
        if (Response.get("Version") != 1 or Response.get("Label") != Label or
                Response.get("RunId") != RunId or
                Response.get("WorkerNonce") != WorkerNonce or
                not re.fullmatch(r"[0-9a-f]{32}", Response.get("HostNonce", ""))):
            raise ValueError("reverse tunnel reached the wrong coordinator endpoint")
        Connection.sendall((json.dumps({"HostNonce": Response["HostNonce"]}) +
                            "\n").encode("ascii"))
    return {"Success": True, "Label": Label, "RunId": RunId,
            "Sid": Identity[1], "IsAdmin": IsAdmin,
            "Address": REVERSE_ADDRESS[0], "Port": REVERSE_ADDRESS[1],
            "WorkerNonce": WorkerNonce, "HostNonce": Response["HostNonce"],
            "StartedUnixMs": Started, "CompletedUnixMs": time.time_ns() // 1000000}


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: worker_tunnel_preflight.py LABEL RUN_ID")
    Result = Probe(sys.argv[1], sys.argv[2])
    print(json.dumps(Result, sort_keys=True), flush=True)
