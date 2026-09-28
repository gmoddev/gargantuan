"""Fail closed before wake if the worker cannot reach the lifecycle host."""

import re
import subprocess


def RequireWorkerControlTunnel(Label, WorkerArtifact, Runner=subprocess.run):
    if not re.fullmatch(r"[0-9a-f]{16}", Label):
        raise ValueError("invalid worker tunnel identity")
    Command = ["ssh", "-o", "BatchMode=yes", "dockerbox", "powershell",
               "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
               WorkerArtifact + "/" + Label + "/Check-WorkerControlTunnel.ps1"]
    Result = Runner(Command, capture_output=True, text=True, timeout=10)
    if Result.returncode or "LISTENING" not in Result.stdout:
        raise RuntimeError("worker reverse lifecycle tunnel missing before wake: " +
                           Result.stderr[-200:])
