# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Relocate the production Construction Yard and reopen two persistent world slots."""
import argparse
import json
import os
from pathlib import Path
import sys
import tempfile

from SpatialConstructionPackageSession import ManagedEnvironment, Run, WriteJson

Samples = Path(__file__).resolve().parents[1] / "samples"
sys.path.insert(0, str(Samples / "TerrainQuarry"))
from BuildProject import EditorHost


MenuSource = """
local Title = Instance.new("ScreenGui")
Title.Parent = script.Parent
local Status = Instance.new("TextLabel")
Status.Parent = Title
Status.Text = "Construction Yard"
assert(game:SetMainMenu(Title).Ok)
local Saves = game:GetService("WorldSaveService")
local Sessions = game:GetService("SessionService")
local Run = game:GetService("RunService")
task.defer(function()
    local Listed = Saves:List():Await()
    assert(Listed.Ok)
    local Slots = Listed.Value.Items
    if #Slots == 0 then
        Slots = {}
        for _, Name in {"Hillside", "Riverside"} do
            local Created = Saves:Create({Name = Name, Seed = Name}):Await()
            assert(Created.Ok)
            table.insert(Slots, Created.Value)
        end
    end
    assert(#Slots == 2 and Slots[1].SaveId ~= Slots[2].SaveId)
    for _, Slot in Slots do
        local Started = Sessions:StartLocal({SaveId = Slot.SaveId}):Await()
        assert(Started.Ok and Started.Value.WorldId == Slot.WorldId)
        assert(game:SetMainMenuVisible(false).Ok)
        task.wait(3)
        assert(Run:PauseGameplay():Await().Value)
        assert(game:SetMainMenuVisible(true).Ok)
        Status.Text = Slot.Name
        task.wait(0.1)
        assert(Run:IsGameplayPaused())
        assert(Run:ResumeGameplay():Await().Ok)
        assert(Sessions:Leave():Await().Ok)
    end
    assert(game:GetService("ApplicationService"):Quit().Ok)
    print("[Lifecycle:Yard] Returned from both worlds")
end)
"""

# This calls the sample's own authority and validates its existing durable replay.
AuthorityProbe = """
local Selected = game:GetService("WorldSaveService"):GetCurrentWorld()
assert(Selected.Ok and Selected.Value.Name == Selected.Value.Seed and Durable and not Faulted)
local Name = Selected.Value.Name
local Expected = if Name == "Hillside" then 1 else 2
local Initial = #History
task.spawn(function()
    local Player = Players.LocalPlayer or Players:GetPlayers()[1]
    while not Player or not Player.Character or not Player.Character.RootPart do
        task.wait(0.02)
        Player = Players.LocalPlayer or Players:GetPlayers()[1]
    end
    if Initial == 0 then
        for _ = 1, Expected do UseTool(Player, "YardBuild") end
    end
    assert(#History == Expected and BlocksAdded[Wall] == Expected and BlocksAdded[Floor] == 0)
    print("[Lifecycle:Yard] " .. Name .. " initial=" .. tostring(Initial) .. " exterior=" .. tostring(#History))
end)
"""


def Main():
    Parser = argparse.ArgumentParser()
    for Name in ("packager", "runtime", "engine"):
        Parser.add_argument("--" + Name, type=lambda Value: Path(Value).resolve(), required=True)
    Parser.add_argument("--graphical", action="store_true")
    Options = Parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="gly-") as Scratch:
        Root = Path(Scratch)
        Project = Root / "Yard"
        Run([sys.executable, "-B", Samples / "ConstructionYard" / "BuildProject.py",
             "--engine", Options.engine, "--project", Project], Root)
        with EditorHost(Options.engine) as Host:
            Host.Call("OpenProject", Root=Project.as_posix())
            Snapshot = Host.Call("GetSnapshot")["Snapshot"]
            Model = next(Item["Id"] for Item in Snapshot["Objects"] if Item["ClassName"] == "DataModel")
            Schema = {Row["Name"]: Row for Row in Host.Call("GetSchema")["Definitions"]
                      if Row["Kind"] == "Class" and Row["Namespace"] == "Engine"}
            Folder, Script = Schema["Folder"], Schema["Script"]
            App = Host.Call("CreateInstance", ClassSchemaId=Folder["SchemaId"], DefinitionVersion=Folder["DefinitionVersion"],
                            Parent=Model, Name="Application")["Object"]
            Host.Call("CreateInstance", ClassSchemaId=Script["SchemaId"], DefinitionVersion=Script["DefinitionVersion"],
                      Parent=App, Name="Entry", InitialSource=MenuSource,
                      InitialProperties=[dict(DeclaringClassSchemaId=Script["SchemaId"],
                                              DeclaringDefinitionVersion=Script["DefinitionVersion"], Property="RunContext",
                                              Value=dict(Type="EnumItem", Enum="RunContext", Value="Client"))])
            Host.Call("SaveProject")
        ScenePath = Project / ".gargantuan" / "project.instance.json"
        Scene = json.loads(ScenePath.read_bytes())
        Entry = next(Item for Item in Scene["Children"] if Item["Name"] == "Application")["Children"][0]
        Scene["Properties"]["ApplicationEntry"] = Entry["Properties"]["SourceId"]
        Yard = next(Item for Item in Scene["Children"] if Item["ClassName"] == "Script" and Item["Name"] == "Yard")
        Yard["Properties"]["Source"]["String"] += AuthorityProbe
        WriteJson(ScenePath, Scene)
        Stage = Root / "Stage"
        Run([Options.packager, "build", "--project", Project, "--runtime", Options.runtime,
             "--output", Stage, "--configuration", "Release"], Root)
        Installed = Root / "Installed"
        Stage.rename(Installed)
        Run([Options.packager, "validate", Installed], Root)
        Player = Installed / ("GargantuanPlayer.exe" if os.name == "nt" else "GargantuanPlayer")
        Environment = ManagedEnvironment()
        if Options.graphical:
            Environment.pop("SDL_AUDIODRIVER", None)
        for Fresh in (True, False):
            Args = [Player] if Options.graphical else [Player, "--headless"]
            Output = Run([*Args, "--profile-root", Root, "--max-frames", "1200"], Root, Environment)
            for Name, Count in (("Hillside", 1), ("Riverside", 2)):
                Marker = f"[Lifecycle:Yard] {Name} initial={0 if Fresh else Count} exterior={Count}"
                if Marker not in Output:
                    raise RuntimeError(Marker + " missing\n" + Output[-4500:])
            if "[Lifecycle:Yard] Returned from both worlds" not in Output or "Reached requested frame limit" in Output:
                raise RuntimeError("Construction Yard did not quit successfully\n" + Output[-3000:])
        print("[Lifecycle:Yard] Verified two durable construction histories across standalone Player restarts")


if __name__ == "__main__":
    Main()
