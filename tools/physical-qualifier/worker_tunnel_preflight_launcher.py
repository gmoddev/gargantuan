"""Launch the fixed reverse probe in the worker's real Codex sandbox."""

import json
from pathlib import Path
import re
import subprocess
import sys
import uuid


ARTIFACTS = Path(r"C:\Sandbox\Codex\Artifacts\gargantuan-3l-physical")
WORKSPACE = Path(r"C:\Sandbox\Codex\Workspaces\agent-coordinator-foundation-2-server")
CODEX = Path(r"C:\Users\host\AppData\Local\OpenAI\Codex\bin\d23520d1e41bfb24\codex.exe")
PYTHON = Path(r"C:\Sandbox\Codex\Tools\physical-qualifier\runtime\python.exe")


def Main(Label, RunId):
    if not re.fullmatch(r"[0-9a-f]{16}", Label) or str(uuid.UUID(RunId)) != RunId:
        raise ValueError("invalid reverse tunnel identity")
    Output = ARTIFACTS / Label / "worker-tunnel-restricted-proof.json"
    if Output.exists():
        raise ValueError("reverse tunnel proof already consumed")
    Command = [str(CODEX), "sandbox", "-P", ":workspace", "-p",
               "foundation-2-endpoint", "-C", str(WORKSPACE), str(PYTHON),
               "-B", str(ARTIFACTS / Label / "worker_tunnel_preflight.py"), Label, RunId]
    try:
        Process = subprocess.run(Command, cwd=WORKSPACE, text=True,
                                 capture_output=True, timeout=20)
        try:
            Report = json.loads(Process.stdout.strip().splitlines()[-1])
        except (IndexError, ValueError):
            Report = {"Success": False, "Error": "restricted reverse proof missing",
                      "Stderr": Process.stderr[-300:]}
        Report["ExitCode"] = Process.returncode
    except (OSError, subprocess.TimeoutExpired) as Error:
        Report = {"Success": False, "Error": str(Error)}
    Output.write_text(json.dumps(Report, indent=2) + "\n", encoding="utf-8")
    return 0 if Report.get("Success") else 1


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: worker_tunnel_preflight_launcher.py LABEL RUN_ID")
    raise SystemExit(Main(sys.argv[1], sys.argv[2]))
