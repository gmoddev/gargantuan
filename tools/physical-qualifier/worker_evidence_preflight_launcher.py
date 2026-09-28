"""Launch a fixed worker evidence probe from the worker's interactive session."""

import json
from pathlib import Path
import re
import subprocess
import sys
import uuid


WORKSPACE = Path(r"C:\Sandbox\Codex\Workspaces\agent-coordinator-foundation-2-server")
ARTIFACT_ROOT = Path(r"C:\Sandbox\Codex\Artifacts\gargantuan-3l-physical")
SERVICE_ROOT = Path(r"C:\GargantuanQualification\physical-qualifier-service-evidence")
CONFIG_ROOT = Path(r"C:\Sandbox\Codex\Evidence\physical-qualifier")
CODEX = Path(r"C:\Users\host\AppData\Local\OpenAI\Codex\bin\d23520d1e41bfb24\codex.exe")
PYTHON = Path(r"C:\Sandbox\Codex\Tools\physical-qualifier\runtime\python.exe")
SID = "S-1-5-21-455006656-4040886684-1921607991-1006"


def Main(Label, RunId):
    if not re.fullmatch(r"[0-9a-f]{16}", Label):
        raise ValueError("invalid worker evidence label")
    if str(uuid.UUID(RunId)) != RunId:
        raise ValueError("invalid physical run ID")
    Directory = ARTIFACT_ROOT / Label
    ResultFile = Directory / "worker-evidence-preflight.json"
    if ResultFile.exists():
        raise ValueError("worker evidence preflight already consumed")
    Command = [str(CODEX), "sandbox", "-P", ":workspace", "-p",
               "foundation-2-endpoint", "-C", str(WORKSPACE), str(PYTHON),
               "-B", str(ARTIFACT_ROOT / "evidence_preflight.py"), "WORKER",
               str(SERVICE_ROOT / ("lifecycle-" + Label)), RunId, SID,
               str(CONFIG_ROOT / ("lifecycle-" + Label) / "server.json")]
    try:
        Process = subprocess.run(Command, text=True, capture_output=True, timeout=25)
        try:
            Report = json.loads(Process.stdout.strip().splitlines()[-1])
        except (IndexError, ValueError):
            Report = {"Success": False, "Error": "restricted worker probe produced no proof",
                      "Stderr": Process.stderr[-300:]}
        Report["ExitCode"] = Process.returncode
    except (OSError, subprocess.TimeoutExpired) as Error:
        Report = {"Success": False, "Error": str(Error)}
    ResultFile.write_text(json.dumps(Report, indent=2) + "\n", encoding="utf-8")
    return 0 if Report.get("Success") else 1


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: worker_evidence_preflight_launcher.py LABEL RUN_ID")
    raise SystemExit(Main(sys.argv[1], sys.argv[2]))
