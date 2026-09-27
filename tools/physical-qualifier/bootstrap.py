"""Fetch an immutable upstream commit; never install or execute endpoint tools."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

from dependency import GetRoot


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--check", action="store_true")
    Args = Parser.parse_args()
    Source = Path(__file__).resolve().parent
    Lock = json.loads((Source / "upstream.lock.json").read_text(encoding="utf-8"))
    Root = Source / ".agent-coordinator"
    Hidden = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}
    def Git(*Args):
        return subprocess.run(["git", *Args], check=True, capture_output=True, text=True, timeout=120, **Hidden).stdout.strip()
    try:
        if not Root.exists() and not Args.check:
            Git("clone", "-c", "core.autocrlf=false", "--filter=blob:none", "--no-checkout", Lock["Repository"], str(Root))
            Git("-C", str(Root), "fetch", "--depth=1", "origin", Lock["Commit"])
            Git("-C", str(Root), "checkout", "--detach", Lock["Commit"])
        GetRoot()
        if (Root / ".git").exists() and Git("-C", str(Root), "rev-parse", "HEAD") != Lock["Commit"]:
            raise ValueError("upstream revision differs from pin")
        print("[Qualification:Bootstrap] Verified " + Lock["Commit"])
        return 0
    except (Exception, KeyboardInterrupt) as Error:
        print("[Qualification:Bootstrap] " + str(Error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(Main())
