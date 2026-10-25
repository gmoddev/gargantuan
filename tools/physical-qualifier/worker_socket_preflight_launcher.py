"""Run the fixed socket request in the installed worker sandbox session."""

import json
from pathlib import Path
import re
import subprocess
import sys
import uuid


WORKSPACE = Path(r"C:\Sandbox\Codex\Workspaces\agent-coordinator-foundation-2-server")
ARTIFACTS = Path(r"C:\Sandbox\Codex\Artifacts\gargantuan-3l-physical")
CODEX = Path(r"C:\Users\host\AppData\Local\OpenAI\Codex\bin\d23520d1e41bfb24\codex.exe")
PYTHON = Path(r"C:\Sandbox\Codex\Tools\physical-qualifier\runtime\python.exe")


def Main(Label, RunId):
    if not re.fullmatch(r"[0-9a-f]{16}", Label) or str(uuid.UUID(RunId)) != RunId:
        raise ValueError("invalid socket preflight identity")
    Output = ARTIFACTS / Label / "worker-socket-restricted-proof.json"
    if Output.exists():
        raise ValueError("worker socket proof already exists")
    Command = [str(CODEX), "sandbox", "-P", ":workspace", "-p",
               "foundation-2-endpoint", "-C", str(WORKSPACE), str(PYTHON),
               "-B", str(ARTIFACTS / Label / "worker_socket_preflight.py"), Label, RunId]
    try:
        Process = subprocess.run(Command, cwd=WORKSPACE, text=True,
                                 capture_output=True, timeout=25)
        try:
            Result = json.loads(Process.stdout.strip().splitlines()[-1])
        except (IndexError, ValueError):
            Result = {"Success": False, "Error": "restricted socket proof missing",
                      "Stderr": Process.stderr[-300:]}
        Result["ExitCode"] = Process.returncode
    except (OSError, subprocess.TimeoutExpired) as Error:
        Result = {"Success": False, "Error": str(Error)}
    Output.write_text(json.dumps(Result, indent=2) + "\n", encoding="utf-8")
    return 0 if Result.get("Success") else 1


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: worker_socket_preflight_launcher.py LABEL RUN_ID")
    raise SystemExit(Main(sys.argv[1], sys.argv[2]))
