# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Exercise relocated offline games through the installed Player entry point."""
import argparse
import copy
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
local Transitions = {}
Sessions.Changed:Connect(function(Info)
    assert(table.isfrozen(Info) and Sessions:GetSession().Generation == Info.Generation)
    table.insert(Transitions, Info.State)
end)
local Application = game:GetService("ApplicationService")
local PendingAtClose
assert(Application:GetState() == "Starting")
Application.Closing:Connect(function(Committed)
    assert(Committed and Application:GetState() == "Closing")
    assert(PendingAtClose and PendingAtClose:GetResult().Code == "Retired")
    assert(Sessions:StartLocal():Await().Code == "WrongState")
    print("[Lifecycle:Acceptance] Closing committed")
end)
assert(game:SetMainMenu(Menu).Ok)
task.defer(function()
    assert(Application:GetState() == "Running")
    local Idle = Sessions:Leave():Await()
    assert(Idle.Ok and Idle.Value.State == "Idle")
    local Cancelled = Sessions:StartLocal()
    Run.PreRender:Wait()
    assert(Sessions:GetSession().State == "Starting")
    assert(Cancelled:Cancel())
    assert(Cancelled:Await().Code == "Cancelled")
    assert(Sessions:GetSession().State == "Idle")
    assert(table.concat(Transitions, ",") == "Starting,Idle")
    local Previous = Sessions:GetSession().Generation
    for Cycle = 1, CYCLES do
        local Operation = Sessions:StartLocal()
        assert(Operation:GetProgress().Phase == "Validating")
        local SawLoad = false
        Operation.ProgressChanged:Connect(function(Progress)
            if Progress.Phase == "Loading" then SawLoad = true end
        end)
        local Started = Operation:Await()
        assert(Operation:GetProgress().Phase == "Ready" and SawLoad)
        assert(Started.Ok and Started.Value.State == "Active")
        assert(Started.Value.Generation ~= Previous)
        Previous = Started.Value.Generation
        assert(game:SetMainMenuVisible(false).Ok)
        assert(Run:PauseGameplay():Await().Value and Run:IsGameplayPaused())
        print("[Lifecycle:Acceptance] Parked")
        assert(game:SetMainMenuVisible(true).Ok)
        task.wait(0.035)
        Label.Text = "Paused"
        assert(Run:IsGameplayPaused() and game:IsMainMenuVisible())
        local Resumed = Run:ResumeGameplay():Await()
        assert(Resumed.Ok and not Resumed.Value and not Run:IsGameplayPaused())
        print("[Lifecycle:Acceptance] Resumed")
        task.wait(0.035)
        assert(Sessions:Leave():Await().Ok)
        assert(Sessions:GetSession().State == "Idle")
    end
    PendingAtClose = Sessions:StartLocal()
    assert(game:GetService("ApplicationService"):Quit().Ok)
    assert(Sessions:StartLocal():Await().Code == "WrongState")
    print("[Lifecycle:Acceptance] Offline cycles complete")
end)
"""


LeaveOnQuit = """
local App = game:GetService("ApplicationService")
local Sessions = game:GetService("SessionService")
local Title = Instance.new("ScreenGui")
Title.Parent = script.Parent
assert(game:SetMainMenu(Title).Ok)
task.defer(function()
    assert(Sessions:StartLocal():Await().Ok)
    local Leaving = Sessions:Leave()
    local Same = Sessions:Leave()
    Leaving.Completed:Connect(function(Result)
        assert(Result.Ok and Result.Value.State == "Idle" and App:GetState() == "Closing")
        assert(Same:GetResult().Value.Generation == Result.Value.Generation)
        print("[Lifecycle:Acceptance] Leave confirmed during close")
    end)
    assert(App:Quit().Ok)
end)
"""


def PrepareProject(Options, Directory, Case):
    Project = Directory / "Project"
    shutil.copytree(Options.project, Project)
    ScenePath = Project / ".gargantuan" / "project.instance.json"
    Scene = json.loads(ScenePath.read_bytes())
    if Case != "Direct":
        Folder = next(Item for Item in Scene["Children"] if Item["Name"] == "GameScripts")
        Entry = next(Item for Item in Folder["Children"] if Item["ClassName"] == "Script")
        Gameplay = copy.deepcopy(Folder)
        Observer = copy.deepcopy(Entry)
        Observer["Name"] = "GameplayObserver"
        Observer["Properties"]["Source"] = {"String": """
            print("[Lifecycle:Acceptance] Gameplay startup")
            game:GetService("RunService").PostSimulation:Connect(function()
                print("[Lifecycle:Acceptance] Gameplay tick")
            end)
        """}
        Gameplay["Children"] = [Observer]
        Folder["Name"] = "Application"
        Folder["Children"] = [Entry]
        SourceId = "841d147af21b4547a1f515cb672cab5f"
        Entry["Name"] = "Entry"
        Entry["Properties"].update(SourceId={"String": SourceId}, Enabled={"Bool": True},
                                   RunContext={"EnumItem": ["RunContext", "Client"]},
                                   Source={"String": EntrySource.replace("CYCLES", str(Options.cycles))
                                           if Case == "Menu" else LeaveOnQuit if Case == "LeaveOnQuit" else "return"})
        Scene["Properties"]["ApplicationEntry"] = {"String": SourceId}
        Scene["Children"] = [Item for Item in Scene["Children"] if Item["ClassName"] in ("Workspace", "AssetService")] + [Gameplay, Folder]
        WriteJson(ScenePath, Scene)
    return Project


def Exercise(Options, Directory, Case):
    Project = PrepareProject(Options, Directory, Case)
    Stage = Directory / "Stage"
    Run([Options.packager, "build", "--project", Project, "--output", Stage,
         "--runtime", Options.runtime, "--configuration", "Release"], Directory)
    Installed = Directory / "Installed"
    Stage.rename(Installed)
    Run([Options.packager, "validate", Installed], Directory)
    Player = Installed / ("GargantuanPlayer.exe" if os.name == "nt" else "GargantuanPlayer")
    Arguments = [Player] if Options.graphical else [Player, "--headless"]
    Arguments.extend(["--profile-root", Directory.resolve()])
    Environment = ManagedEnvironment()
    if Options.graphical:
        Environment.pop("SDL_AUDIODRIVER", None)
    Output = Run([*Arguments, "--max-frames", str(Options.cycles * 20 + 120) if Case in ("Menu", "LeaveOnQuit") else "4"],
                 Directory, Environment)
    Expected = "[Lifecycle:Acceptance] Offline cycles complete" if Case == "Menu" else "[Runtime:Player] StartupWallMilliseconds="
    if Case == "LeaveOnQuit":
        Expected = "[Lifecycle:Acceptance] Leave confirmed during close"
    if Expected not in Output or (Case not in ("Menu", "LeaveOnQuit") and "[Lifecycle:Player] Application entry registered" in Output):
        raise RuntimeError(f"{Case} startup did not reach its expected terminal marker\n{Output[-4000:]}")
    if Case == "Menu" and "[Lifecycle:Player] Reached requested frame limit" in Output:
        raise RuntimeError("Application:Quit did not close the installed Player")
    if Case == "Menu":
        assert "[Lifecycle:Acceptance] Closing committed" in Output
        RuntimeLog = "\n".join(Line for Line in Output.splitlines() if "[Runtime:Luau]" in Line)
        Parked = RuntimeLog.split("[Lifecycle:Acceptance] Parked")
        if len(Parked) != Options.cycles + 1 or RuntimeLog.count("[Lifecycle:Acceptance] Gameplay startup") != Options.cycles:
            raise RuntimeError("Gameplay session startup or pause count differs from the requested cycles\n" + Output[-3000:])
        for Cycle in Parked[1:]:
            Before, Boundary, After = Cycle.partition("[Lifecycle:Acceptance] Resumed")
            Tick = "[Lifecycle:Acceptance] Gameplay tick"
            if not Boundary or Tick in Before or Tick not in After:
                raise RuntimeError("Gameplay simulation crossed a pause boundary incorrectly\n" + Cycle[-3000:])
    print(f"[Lifecycle:Acceptance] {Case} installed game passed", flush=True)


def Main():
    Parser = argparse.ArgumentParser()
    for Name in ("packager", "runtime", "project"):
        Parser.add_argument("--" + Name, type=lambda Value: Path(Value).resolve(), required=True)
    Parser.add_argument("--cycles", type=int, choices=range(1, 101), default=2)
    Parser.add_argument("--graphical", action="store_true")
    Options = Parser.parse_args()
    for Case in ("Menu", "LeaveOnQuit", "EmptyEntry", "Direct"):
        with tempfile.TemporaryDirectory(prefix="gsl-") as Scratch:
            Exercise(Options, Path(Scratch), Case)


if __name__ == "__main__":
    Main()
