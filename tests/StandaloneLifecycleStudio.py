# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Exercise authored lifecycle scripts through the negotiated Studio Play boundary."""
import argparse
import json
import os
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
        Controller = next(Node for Node in Scene["Children"] if Node["Name"] == "Application")["Children"][0]
        Source = Controller["Properties"]["Source"]["String"]
        Source = Source.replace("local Previous =", """
    local Saves = game:GetService("WorldSaveService")
    local Page = Saves:List():Await()
    assert(Page.Ok)
    local Slot = Page.Value.Items[1]
    if not Slot then
        local Created = Saves:Create({Name="Studio world", Seed="Studio seed"}):Await()
        assert(Created.Ok)
        Slot = Created.Value
    end
    print("[Lifecycle:StudioSlot] " .. Slot.SaveId)
    local Previous =""").replace("local Started = Sessions:StartLocal():Await()",
        "local Started = Sessions:StartLocal({SaveId=Slot.SaveId}):Await()\n        assert(Started.Ok and Started.Value.WorldId == Slot.WorldId)")
        Controller["Properties"]["Source"]["String"] = Source
        ScenePath.write_text(json.dumps(Scene), encoding="utf-8")
        DataRoot = Path(Scratch) / "PlayData"
        DataRoot.mkdir()
        os.environ["GARGANTUAN_STUDIO_PLAY_DATA_ROOT"] = str(DataRoot)
        with EditorHost(Options.engine.resolve()) as Host:
            assert "ApplicationPlay" in Host.Call("Handshake")["Capabilities"]
            Host.Call("OpenProject", Root=Project.as_posix())
            Authored = Host.Call("GetSnapshot")["Snapshot"]
            Root = next(Row["Id"] for Row in Authored["Objects"] if Row["ClassName"] == "DataModel")
            Definition = next(Row for Row in Host.Call("GetSchema")["Definitions"] if Row["Kind"] == "Class" and
                              Row["Namespace"] == "Engine" and Row["Name"] == "DataModel")
            Host.Call("SetProperty", Object=Root, Property="ApplicationEntry", Value={"Type": "String", "Value": EntryId},
                      ClassSchemaId=Definition["SchemaId"], ClassDefinitionVersion=Definition["DefinitionVersion"],
                      DeclaringClassSchemaId=Definition["SchemaId"], DeclaringDefinitionVersion=Definition["DefinitionVersion"])
            Before = Host.Call("GetSnapshot")["Snapshot"]
            Previous = None
            PersistentSlot = None
            for Mode in ("Disposable", "Persistent", "Persistent"):
                Launch = Host.Call("StartPlaySession", LaunchMode="Application", PersistenceMode=Mode)
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
                Slot = next(Message.split("[Lifecycle:StudioSlot] ")[1].split()[0] for Message in Messages
                            if "[Lifecycle:StudioSlot] " in Message)
                if Mode == "Persistent":
                    assert PersistentSlot is None or Slot == PersistentSlot
                    PersistentSlot = Slot
                assert any(Row["MenuVisible"] and not Row["WorldActive"] for Row in States)
                assert any(Row["MenuVisible"] and Row["GameplayPaused"] for Row in States)
            After = Host.Call("GetSnapshot")["Snapshot"]
            assert Before == After, "Play changed the authoritative authoring snapshot"
        print("[Lifecycle:Studio] Three application Play runs completed six saved worlds at one authoring revision")


if __name__ == "__main__":
    Main()

