#!/usr/bin/env python3
"""Run the sample landing interlock twice in fresh native Play hosts with controlled ticket terminal states."""
import argparse
from pathlib import Path
import tempfile
import time

from SpatialConstructionYardInput import EditorHost, PrepareProject, Samples


def Trial(Engine, Project):
    with EditorHost(Engine) as Host:
        Host.Call("Handshake")
        Host.Call("OpenProject", Root=Project.as_posix())
        Objects = Host.Call("GetSnapshot")["Snapshot"]["Objects"]
        Yard, = [Row["Id"] for Row in Objects if Row["ClassName"] == "Script" and Row["Name"] == "Yard"]
        Source = Host.Call("GetScriptSource", Object=Yard)
        if Source["Source"] != (Samples / "ConstructionYard/Yard.luau").read_text(encoding="utf-8"):
            raise ValueError("The saved fixture must contain the qualified Yard Script")
        Suffix = (Path(__file__).parent / "blockgrid/gameplay/YardDoorInterlockTrial.luau").read_text(encoding="utf-8")
        Host.Call("SetScriptSource", Object=Yard, ExpectedSourceVersion=Source["SourceVersion"],
                  Source=Source["Source"] + "\n" + Suffix)
        Packet = Host.Call("StartPlaySession")
        Session = Packet["PlaySessionId"]
        try:
            Deadline = time.monotonic() + 15
            while time.monotonic() < Deadline:
                for Row in Packet.get("Diagnostics", []):
                    if Row.get("Severity") == "Error":
                        raise RuntimeError(Row)
                    if "[Spatial:YardInterlock] holders=" in Row.get("Message", ""):
                        print(Row["Message"], flush=True)
                        return
                time.sleep(.02)
                Packet = Host.Call("PollPlayDiagnostics", PlaySessionId=Session)
            raise TimeoutError("Missing Yard interlock receipt")
        finally:
            Host.Call("StopPlaySession", PlaySessionId=Session)


if __name__ == "__main__":
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--engine", type=Path, required=True)
    Parser.add_argument("--project", type=Path, required=True)
    Args = Parser.parse_args()
    Engine = Args.engine.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="yard-door-") as Temporary:
        Project = PrepareProject(Engine, Path(Temporary), Args.project)
        for _ in range(2):
            Trial(Engine, Project)
