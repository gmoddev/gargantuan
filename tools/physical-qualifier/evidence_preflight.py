"""One-use client evidence check, executed inside the installed Codex sandbox.

The caller must run this with ``codex sandbox -P :workspace`` and the same
endpoint profile and workspace as the lifecycle daemon. No agent is woken here.
"""

import csv
import ctypes
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import uuid


APPROVED_ROOT = Path(r"C:\Sandbox\Codex\Evidence\physical-qualifier")
EXPECTED_SID = "S-1-5-21-2820064101-3801502750-265446247-1004"
OLD_RUN_IDS = {"58f15af7-46b7-462b-af78-df88a8db428d"}
PAYLOAD = b"gargantuan-physical-evidence-preflight-v1\n"


def ValidateRun(RunDirectory, RunId, Root=APPROVED_ROOT, MaxAgeSeconds=600):
    RunDirectory = Path(RunDirectory)
    Root = Path(Root)
    if (RunDirectory.parent != Root or
            not re.fullmatch(r"lifecycle-[0-9a-f]{16}", RunDirectory.name) or
            RunDirectory.is_symlink() or not RunDirectory.is_dir() or
            RunId in OLD_RUN_IDS):
        raise ValueError("unapproved or stale evidence run directory")
    if time.time() - RunDirectory.stat().st_ctime > MaxAgeSeconds:
        raise ValueError("stale evidence run directory")
    if (RunDirectory / "preflight.json").exists():
        raise ValueError("evidence preflight already consumed")
    for Name, EvidenceName in (("coordinator.json", "coordinator-evidence"),
                               ("client.json", "client-evidence")):
        Config = json.loads((RunDirectory / Name).read_text(encoding="utf-8"))
        if Config["RunId"] != RunId or Path(Config["EvidenceDir"]) != RunDirectory / EvidenceName:
            raise ValueError("physical config does not match fresh evidence directory")
        if (RunDirectory / EvidenceName).exists():
            raise ValueError("physical evidence already used")


def Probe(RunDirectory, RunId, Root=APPROVED_ROOT, ExpectedSid=EXPECTED_SID):
    ValidateRun(RunDirectory, RunId, Root)
    Identity = next(csv.reader([subprocess.check_output(
        ["whoami", "/user", "/fo", "csv", "/nh"], text=True).strip()]))
    IsAdmin = bool(ctypes.windll.shell32.IsUserAnAdmin())
    Report = {"RunId": RunId, "Identity": Identity[0], "Sid": Identity[1],
              "IsAdmin": IsAdmin, "Success": False}
    if Identity[1] != ExpectedSid or IsAdmin:
        raise PermissionError("preflight did not run as the restricted endpoint identity")
    Directory = Path(RunDirectory) / ("preflight-" + uuid.uuid4().hex)
    Step = "create-directory"
    try:
        Directory.mkdir()
        File = Directory / "payload.bin"
        Step = "write-and-flush"
        with File.open("xb") as Stream:
            Stream.write(PAYLOAD)
            Stream.flush()
            os.fsync(Stream.fileno())
        Step = "reopen-read"
        if File.read_bytes() != PAYLOAD:
            raise ValueError("preflight bytes changed")
        Step = "rename"
        Renamed = Directory / "renamed.bin"
        File.rename(Renamed)
        Step = "delete-file"
        Renamed.unlink()
        Step = "delete-directory"
        Directory.rmdir()
        Report["Success"] = True
        (Path(RunDirectory) / "preflight.json").write_text(
            json.dumps(Report, sort_keys=True) + "\n", encoding="utf-8")
    except Exception as Error:
        Report.update({"FailureStep": Step, "Error": str(Error)})
    return Report


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: evidence_preflight.py RUN_DIRECTORY RUN_ID")
    Result = Probe(sys.argv[1], sys.argv[2])
    print(json.dumps(Result, sort_keys=True), flush=True)
    raise SystemExit(0 if Result["Success"] else 1)
