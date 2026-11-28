# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Resume the unchanged fixed schedule after a pre-measurement project-creation failure."""
import argparse
import csv
from datetime import datetime, timezone
import gzip
import hashlib
import json
from pathlib import Path
import sys
from unittest.mock import patch

import SpatialConstructionPrecision as Precision


def Now():
    return datetime.now(timezone.utc).isoformat()


def Main():
    Parser = argparse.ArgumentParser(description=__doc__, add_help=False)
    Parser.add_argument("--prior", type=Path, required=True)
    Parser.add_argument("--recovery-log", type=Path, required=True)
    Local, Forward = Parser.parse_known_args()
    with (Local.prior/"Processes.csv").open(encoding="utf-8", newline="") as File:
        Old = list(csv.DictReader(File))
    with gzip.open(Local.prior/"Ticks.csv.gz", "rt", encoding="utf-8", newline="") as File:
        Samples = list(csv.DictReader(File))
    if not Old or len(Old) != len(Samples):
        raise ValueError("The existing process and raw receipts must have the same nonzero length")
    Prefix = len(Old)
    Position = 0
    Hashes = {}
    Measured = Precision.Measure

    def Log(Event, **Fields):
        with Local.recovery_log.open("a", encoding="utf-8") as Stream:
            Stream.write(json.dumps(dict(Utc=Now(), Event=Event, **Fields))+ "\n")

    def Recover(Win, Binary, Project, Options):
        nonlocal Position
        Position += 1
        if Position > Prefix:
            if Position == Prefix+1:
                Log("FirstUnsampledPosition", Ordinal=Position, Position=Project.name, Product=Binary.name,
                    OriginalParent=Project.parent.parent.as_posix())
            for Attempt in range(1, 4):
                Scene = Project if Attempt == 1 else Project.with_name(Project.name+"-prepare-"+str(Attempt))
                try:
                    return Measured(Win, Binary, Scene, Options)
                except RuntimeError as Error:
                    if str(Error) != "CreateProject failed (FilesystemFailure) The new project could not be established atomically":
                        raise
                    Log("FailedBeforeWarmup", Ordinal=Position, Position=Project.name, Attempt=Attempt, Error=str(Error))
                    if Attempt == 3:
                        raise
        Row, Raw = Old[Position-1], Samples[Position-1]
        Name = Row["Profile"]
        if Name not in ("baseline", "current"):
            raise ValueError("Unknown scheduled profile")
        if Name not in Hashes:
            Hashes[Name] = hashlib.sha256(Binary.read_bytes()).hexdigest()
        if (Binary != getattr(Options, Name).resolve(strict=True) or Hashes[Name] != Row["EngineSHA256"] or
                Project.name != "B"+Row["Block"]+"R"+Row["Run"] or Options.scene != Row["Scene"] or
                any(Row[K] != Raw[K] for K in ("Block", "Run", "Profile"))):
            raise ValueError("Previously observed receipt differs from the unchanged acquisition schedule")
        Ticks = list(map(int, Raw["StepNanoseconds"].split()))
        Groups = [tuple(map(int, Group.split(":"))) for Group in Raw["OmittedGroupsStartCountTotal"].split()]
        Resets = [tuple(map(int, Group.split(":"))) for Group in Raw["ResetGapsStartCount"].split()]
        if (len(Ticks) != int(Row["Exact"]) or sum(Ticks)+sum(Total for _, _, Total in Groups) != int(Row["EngineStepNs"])):
            raise ValueError("Previously observed native receipt failed its count or sum check")
        Values = {K:V for K,V in Row.items() if K not in ("Scene","Block","Run","Profile","EngineSHA256")}
        Values["P95Ns"] = int(Values["P95Ns"])
        return Values, Ticks, Groups, Resets

    def Progress(*Text, **Options):
        if Text and isinstance(Text[0],str) and Text[0].startswith("block "):
            if Position > Prefix:
                print(Text[0].split(" p95=",1)[0], flush=True)
        else:
            print(*Text, **Options)

    Log("Resume", Prefix=Prefix,
        PriorFiles={F:hashlib.sha256((Local.prior/F).read_bytes()).hexdigest() for F in ("Processes.csv", "Ticks.csv.gz")})
    with patch.object(Precision, "Measure", Recover), patch.object(Precision, "print", Progress, create=True), patch.object(
            sys, "argv", [sys.argv[0], *Forward]):
        Precision.Main()
    Log("Complete", Positions=Position)


if __name__ == "__main__":
    Main()
