# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Exercise relocated offline games through the installed Player entry point."""
import argparse
import json
import os
from pathlib import Path
import shutil
import tempfile

from SpatialConstructionPackageSession import ManagedEnvironment, Run, WriteJson


EntrySource = """
local Menu = Instance.new("ScreenGui")
Menu.Parent = script.Parent
local Label = Instance.new("TextLabel")
Label.Text = "Ready"
Label.Parent = Menu
local Sessions = game:GetService("SessionService")
local Run = game:GetService("RunService")
assert(game:SetMainMenu(Menu).Ok)
task.defer(function()
    local Cancelled = Sessions:StartLocal()
    assert(Cancelled:Cancel())
    assert(Cancelled:Await().Code == "Cancelled")
    assert(Sessions:GetSession().State == "Idle")
    local Previous = Sessions:GetSession().Generation
    for Cycle = 1, CYCLES do
        local Started = Sessions:StartLocal():Await()
        assert(Started.Ok and Started.Value.State == "Active")
        assert(Started.Value.Generation ~= Previous)
        Previous = Started.Value.Generation
        assert(game:SetMainMenuVisible(false).Ok)
        assert(Run:PauseGameplay():Await().Value and Run:IsGameplayPaused())
        assert(game:SetMainMenuVisible(true).Ok)
        task.wait(0.035)
        Label.Text = "Paused"
        assert(Run:IsGameplayPaused() and game:IsMainMenuVisible())
        local Resumed = Run:ResumeGameplay():Await()
        assert(Resumed.Ok and not Resumed.Value and not Run:IsGameplayPaused())
        assert(Sessions:Leave():Await().Ok)
        assert(Sessions:GetSession().State == "Idle")
    end
    print("[Lifecycle:Acceptance] Offline cycles complete")
    assert(game:GetService("ApplicationService"):Quit().Ok)
end)
"""


def Exercise(Options, Directory, Case):
    Project = Directory / "Project"
    shutil.copytree(Options.project, Project)
    ScenePath = Project / ".gargantuan" / "project.instance.json"
    Scene = json.loads(ScenePath.read_bytes())
    if Case != "Direct":
        Folder = next(Item for Item in Scene["Children"] if Item["Name"] == "GameScripts")
        Entry = next(Item for Item in Folder["Children"] if Item["ClassName"] == "Script")
        Folder["Name"] = "Application"
        Folder["Children"] = [Entry]
        SourceId = "841d147af21b4547a1f515cb672cab5f"
        Entry["Name"] = "Entry"
        Entry["Properties"].update(SourceId={"String": SourceId}, Enabled={"Bool": True},
                                   RunContext={"EnumItem": ["RunContext", "Client"]},
                                   Source={"String": EntrySource.replace("CYCLES", str(Options.cycles))
                                           if Case == "Menu" else "return"})
        Scene["Properties"]["ApplicationEntry"] = {"String": SourceId}
        Scene["Children"] = [Item for Item in Scene["Children"] if Item["ClassName"] == "AssetService"] + [Folder]
        WriteJson(ScenePath, Scene)
    Stage = Directory / "Stage"
    Run([Options.packager, "build", "--project", Project, "--output", Stage,
         "--runtime", Options.runtime, "--configuration", "Release"], Directory)
    Installed = Directory / "Installed"
    Stage.rename(Installed)
    Run([Options.packager, "validate", Installed], Directory)
    Player = Installed / ("GargantuanPlayer.exe" if os.name == "nt" else "GargantuanPlayer")
    Arguments = [Player] if Options.graphical else [Player, "--headless"]
    Environment = ManagedEnvironment()
    if Options.graphical:
        Environment.pop("SDL_AUDIODRIVER", None)
    Output = Run([*Arguments, "--max-frames", str(Options.cycles * 20 + 120) if Case == "Menu" else "4"],
                 Directory, Environment)
    Expected = "[Lifecycle:Acceptance] Offline cycles complete" if Case == "Menu" else "[Runtime:Player] StartupWallMilliseconds="
    if Expected not in Output or (Case != "Menu" and "[Lifecycle:Player] Application entry registered" in Output):
        raise RuntimeError(f"{Case} startup did not reach its expected terminal marker\n{Output[-4000:]}")
    print(f"[Lifecycle:Acceptance] {Case} installed game passed", flush=True)


def Main():
    Parser = argparse.ArgumentParser()
    for Name in ("packager", "runtime", "project"):
        Parser.add_argument("--" + Name, type=lambda Value: Path(Value).resolve(), required=True)
    Parser.add_argument("--cycles", type=int, choices=range(1, 101), default=2)
    Parser.add_argument("--graphical", action="store_true")
    Options = Parser.parse_args()
    for Case in ("Menu", "EmptyEntry", "Direct"):
        with tempfile.TemporaryDirectory(prefix="gsl-") as Scratch:
            Exercise(Options, Path(Scratch), Case)


if __name__ == "__main__":
    Main()
