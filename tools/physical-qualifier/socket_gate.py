"""Inspect the fixed worker reverse listener; a handshake is required separately."""

import json
import re
import subprocess


def WorkerReverseListener(Label, WorkerArtifact, Runner=subprocess.run):
    if not re.fullmatch(r"[0-9a-f]{16}", Label):
        raise ValueError("invalid worker tunnel identity")
    Command = ["ssh", "-o", "BatchMode=yes", "dockerbox", "powershell",
               "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
               WorkerArtifact + "/" + Label + "/Check-WorkerControlTunnel.ps1"]
    Result = Runner(Command, capture_output=True, text=True, timeout=10)
    if Result.returncode:
        raise RuntimeError("worker reverse listener inspection failed: " + Result.stderr[-200:])
    try:
        Listener = json.loads(Result.stdout.strip().splitlines()[-1])
    except (IndexError, ValueError) as Error:
        raise RuntimeError("worker reverse listener report malformed") from Error
    if Listener.get("Present") is True:
        if (Listener.get("Address") != "127.0.0.1" or Listener.get("Port") != 49961 or
                Listener.get("ProcessName", "").lower() != "sshd" or
                not isinstance(Listener.get("OwnerPid"), int)):
            raise RuntimeError("worker reverse listener owner or port mismatch")
    elif Listener != {"Present": False}:
        raise RuntimeError("worker reverse listener report malformed")
    return Listener
