# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Measure equivalent creator Script operations in fresh, normally scheduled Play worlds."""
import argparse
import csv
import hashlib
from pathlib import Path
import re
import tempfile
import time

from SpatialCreatorTrial import Create, EditorHost


Profiles = ("individual", "transaction", "empty-channel", "scan", "indexed")
Script = Path(__file__).parent / "blockgrid/gameplay/SpatialCost.server.luau"
Receipt = re.compile(r"\[BlockGrid:ScriptCost\] (\S+) (\d+) ([\d.]+) ([\d.]+) ([\d.]+) ([\d.]+)")


def Measure(Engine, Folder, Profile):
    Folder.mkdir()
    Probe = Folder / Script.name
    Probe.write_text(f'local Profile = "{Profile}"\n' + Script.read_text(encoding="utf-8"), encoding="utf-8")
    with EditorHost(Engine) as Host:
        Create(Host, Folder / "Project", [Probe])
        Reply = Host.Call("StartPlaySession", PersistenceMode="Disabled")
        Session = Reply["PlaySessionId"]
        Deadline = time.monotonic() + 45
        try:
            while time.monotonic() < Deadline:
                for Event in Reply.get("Diagnostics", []):
                    Message = Event.get("Message", "")
                    if Event.get("Severity") == "Error":
                        raise RuntimeError(Message)
                    Match = Receipt.search(Message)
                    if Match:
                        if Match[1] != Profile or Match[2] != "128":
                            raise RuntimeError("Incorrect Script measurement count or identity")
                        return Match.groups()[1:]
                time.sleep(.08)
                Reply = Host.Call("PollPlayDiagnostics", PlaySessionId=Session)
            raise RuntimeError(f"Timed out awaiting {Profile} Script observations")
        finally:
            Host.Call("StopPlaySession", PlaySessionId=Session)


if __name__ == "__main__":
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--engine", type=Path, required=True)
    Parser.add_argument("--trials", type=int, default=3)
    Parser.add_argument("--output", type=Path, required=True)
    Args = Parser.parse_args()
    if not 1 <= Args.trials <= 15:
        Parser.error("--trials must be between 1 and 15")
    Engine = Args.engine.resolve(strict=True)
    Digest = hashlib.sha256(Engine.read_bytes()).hexdigest()
    with tempfile.TemporaryDirectory(prefix="gsc-") as Scratch, Args.output.open("x", newline="") as Output:
        Table = csv.writer(Output)
        Table.writerow(("EngineSHA256", "Trial", "Profile", "Samples", "P50Us", "P95Us", "P99Us", "WorstUs"))
        for Trial in range(Args.trials):
            Shift = Trial % len(Profiles)
            for Profile in Profiles[Shift:] + Profiles[:Shift]:
                Values = Measure(Engine, Path(Scratch) / f"{Trial}-{Profile}", Profile)
                Table.writerow((Digest, Trial + 1, Profile, *Values))
                Output.flush()
                print(Trial + 1, Profile, *Values, flush=True)
