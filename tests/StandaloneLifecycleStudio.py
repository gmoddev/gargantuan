# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Exercise authored lifecycle scripts through the negotiated Studio Play boundary."""
import argparse
import json
from pathlib import Path
import sys
import tempfile
import time

from StandaloneLifecyclePlayer import PrepareProject

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "samples" / "TerrainQuarry"))
from BuildProject import EditorHost


def Main():
    Parser = argparse.ArgumentParser()
    Parser.add_argument("--engine", type=Path, required=True)
    Parser.add_argument("--project", type=Path, required=True)
    Options = Parser.parse_args()
    Options.cycles = 2
    with tempfile.TemporaryDirectory(prefix="gls-") as Scratch:
        Project = PrepareProject(Options, Path(Scratch), "Menu")
        ScenePath = Project / ".gargantuan" / "project.instance.json"
        Scene = json.loads(ScenePath.read_bytes())
        EntryId = Scene["Properties"].pop("ApplicationEntry")["String"]
        ScenePath.write_text(json.dumps(Scene), encoding="utf-8")
        with EditorHost(Options.engine.resolve()) as Host:
            assert "ApplicationPlay" in Host.Call("Handshake")["Capabilities"]
            Host.Call("OpenProject", Root=Project.as_posix())
            Authored = Host.Call("GetSnapshot")["Snapshot"]
            Root = next(Row["Id"] for Row in Authored["Objects"] if Row["ClassName"] == "DataModel")
            Host.Call("SetProperty", Object=Root, Property="ApplicationEntry", Value={"Type": "String", "Value": EntryId})
            Before = Host.Call("GetSnapshot")["Snapshot"]
            Previous = None
            for _ in range(2):
                Launch = Host.Call("StartPlaySession", LaunchMode="Application")
                Session = Launch["PlaySessionId"]
                assert Session != Previous
                Previous = Session
                Messages = [Row["Message"] for Row in Launch["Diagnostics"]]
                States = []
                Deadline = time.monotonic() + 15
                while time.monotonic() < Deadline:
                    State = Host.Call("GetPlaySessionState")
                    assert State["PlaySessionId"] == Session
                    if "Application" in State:
                        States.append(State["Application"])
                    Messages.extend(Row["Message"] for Row in Host.Call("PollPlayDiagnostics", PlaySessionId=Session)["Diagnostics"])
                    if State["State"] == "Stopped":
                        break
                    time.sleep(0.005)
                else:
                    raise RuntimeError("Application Play did not stop: " + str(Messages[-12:]))
                Host.Call("StopPlaySession", PlaySessionId=Session)
                Log = "\n".join(Messages)
                assert "[Lifecycle:Acceptance] Offline cycles complete" in Log, Log[-2200:]
                assert Log.count("[Lifecycle:Acceptance] Gameplay startup") == Options.cycles, Log[-2200:]
                assert any(Row["MenuVisible"] and not Row["WorldActive"] for Row in States)
                assert any(Row["MenuVisible"] and Row["GameplayPaused"] for Row in States)
            After = Host.Call("GetSnapshot")["Snapshot"]
            assert Before == After, "Play changed the authoritative authoring snapshot"
        print("[Lifecycle:Studio] Two outer Play IDs completed four fresh worlds at one authoring revision")


if __name__ == "__main__":
    Main()

