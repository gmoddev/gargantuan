"""Verify the exact pinned local library before importing any upstream code."""
import hashlib
import json
from pathlib import Path
import sys


def GetRoot():
    Source = Path(__file__).resolve().parent
    Lock = json.loads((Source / "upstream.lock.json").read_text(encoding="utf-8"))
    Root = Source / ".agent-coordinator"
    for Name, Expected in Lock["Files"].items():
        File = Root / Name
        if not File.is_file() or hashlib.sha256(File.read_bytes()).hexdigest() != Expected:
            raise ValueError("[Qualification:Dependency] Missing/changed pinned library; run bootstrap.py")
    return Root


def Load():
    Root = GetRoot()
    sys.path.insert(0, str(Root))
    from agent_coordinator import legacy, transport
    if Path(transport.__file__).resolve() != (Root / "agent_coordinator/transport.py").resolve():
        raise ValueError("[Qualification:Dependency] Unexpected upstream module origin")
    return legacy, transport
