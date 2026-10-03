"""One-use evidence checks under the selected endpoint identity.

Restricted callers use ``codex sandbox -P :workspace``; interactive callers
run under the logged-in user. No agent is woken here.
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


ROOTS = {
    "CLIENT": Path(r"C:\Sandbox\Codex\Evidence\physical-qualifier"),
    "WORKER": Path(r"C:\GargantuanQualification\physical-qualifier-service-evidence"),
}
OLD_RUN_IDS = {"58f15af7-46b7-462b-af78-df88a8db428d",
               "ff95b60e-8c5e-4125-bbc9-73e1e6844231"}
PAYLOAD = b"gargantuan-physical-evidence-preflight-v1\n"


def ValidateRun(RunDirectory, RunId, EndpointKind="CLIENT", ConfigPath=None,
                Root=None, MaxAgeSeconds=600, ConfigRoot=None):
    if EndpointKind not in ROOTS:
        raise ValueError("unapproved endpoint kind")
    RunDirectory = Path(RunDirectory)
    Root = Path(Root) if Root is not None else ROOTS[EndpointKind]
    if (RunDirectory.parent != Root or
            not re.fullmatch(r"lifecycle-[0-9a-f]{16}", RunDirectory.name) or
            RunId in OLD_RUN_IDS):
        raise ValueError("unapproved or stale evidence run directory")
    if EndpointKind == "CLIENT":
        if RunDirectory.is_symlink() or not RunDirectory.is_dir():
            raise ValueError("client run directory missing or redirected")
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
    else:
        if RunDirectory.exists() or RunDirectory.is_symlink():
            raise ValueError("worker run evidence already used")
        ConfigRoot = (Path(ConfigRoot) if ConfigRoot is not None else
                      Path(r"C:\Sandbox\Codex\Evidence\physical-qualifier"))
        ExpectedConfig = (ConfigRoot /
                          RunDirectory.name / "server.json")
        if ConfigPath is None or Path(ConfigPath) != ExpectedConfig:
            raise ValueError("unapproved worker config path")
        if time.time() - ExpectedConfig.stat().st_ctime > MaxAgeSeconds:
            raise ValueError("stale worker physical config")
        Config = json.loads(ExpectedConfig.read_text(encoding="utf-8"))
        if Config["RunId"] != RunId or Path(Config["EvidenceDir"]) != RunDirectory:
            raise ValueError("worker physical config does not match fresh evidence directory")


def Probe(RunDirectory, RunId, EndpointKind, ExpectedSid, ConfigPath=None, Root=None):
    ValidateRun(RunDirectory, RunId, EndpointKind, ConfigPath, Root)
    Identity = next(csv.reader([subprocess.check_output(
        ["whoami", "/user", "/fo", "csv", "/nh"], text=True).strip()]))
    IsAdmin = bool(ctypes.windll.shell32.IsUserAnAdmin())
    Report = {"RunId": RunId, "EndpointKind": EndpointKind,
              "Identity": Identity[0], "Sid": Identity[1],
              "IsAdmin": IsAdmin, "Success": False}
    if Identity[1] != ExpectedSid or IsAdmin:
        raise PermissionError("preflight did not run as the expected endpoint identity")
    RunDirectory = Path(RunDirectory)
    Directory = RunDirectory / ("preflight-" + uuid.uuid4().hex)
    Step = "create-run-directory" if EndpointKind == "WORKER" else "create-directory"
    try:
        if EndpointKind == "WORKER":
            RunDirectory.mkdir()
            Step = "create-directory"
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
        if EndpointKind == "WORKER":
            Step = "delete-run-directory"
            RunDirectory.rmdir()
        Report["Success"] = True
        if EndpointKind == "CLIENT":
            (RunDirectory / "preflight.json").write_text(
                json.dumps(Report, sort_keys=True) + "\n", encoding="utf-8")
    except Exception as Error:
        Report.update({"FailureStep": Step, "Error": str(Error)})
    return Report


if __name__ == "__main__":
    if len(sys.argv) not in (5, 6):
        raise SystemExit("usage: evidence_preflight.py CLIENT|WORKER RUN_DIRECTORY RUN_ID EXPECTED_SID [SERVER_CONFIG]")
    Result = Probe(sys.argv[2], sys.argv[3], sys.argv[1], sys.argv[4],
                   sys.argv[5] if len(sys.argv) == 6 else None)
    print(json.dumps(Result, sort_keys=True), flush=True)
    raise SystemExit(0 if Result["Success"] else 1)
