# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Relocate the production Construction Yard and reopen two persistent world slots."""
import argparse
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import time
import tempfile

from SpatialConstructionPackageSession import ConsoleCapture, ManagedEnvironment, Run, WriteJson

Samples = Path(__file__).resolve().parents[1] / "samples"


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


def PlayAuthoredMenu(Root, Project, ScenePath, Scene, Entry, Options):
    import ctypes
    from ctypes import wintypes
    from StandaloneMenuInput import Click, FindPlayerWindow
    User = ctypes.WinDLL("user32", use_last_error=True)
    User.SetThreadDpiAwarenessContext.argtypes = [wintypes.HANDLE]
    User.SetThreadDpiAwarenessContext.restype = wintypes.HANDLE
    if not User.SetThreadDpiAwarenessContext(wintypes.HANDLE(-4)):
        raise ctypes.WinError(ctypes.get_last_error())
    # Observe the authored controls and real session. All actions originate as
    # native pointer/key input to the installed Player's own window.
    Entry["Properties"]["Source"]["String"] += """
PauseKey.Pressed:Connect(function() print("[Lifecycle:YardMenu] Escape pressed") end)
PauseKey.Released:Connect(function() print("[Lifecycle:YardMenu] Escape released") end)
task.defer(function()
    local Last, Previous = "", nil
    local LastStatus = nil
    while true do
        task.wait(0.04)
        if Status.Text ~= LastStatus then
            print("[Lifecycle:YardStatus] " .. Status.Text)
            LastStatus = Status.Text
        end
        if not Busy then
            local State = Sessions:GetSession()
            local View = if Run:IsGameplayPaused() then "Paused" elseif State.State == "Active" then "Playing" else
                if Slot then "Selected" else "Empty"
            if View == Previous and View ~= Last then
                print("[Lifecycle:YardMenu] " .. View .. " " .. (if Slot then Slot.WorldId else "-"))
                Last = View
            end
            Previous = View
        end
    end
end)
"""
    Yard = next(Item for Item in Scene["Children"] if Item["ClassName"] == "Script" and Item["Name"] == "Yard")
    Yard["Properties"]["Source"]["String"] += """
task.defer(function()
    local Last = -1
    while true do
        task.wait(0.04)
        local Player = Players.LocalPlayer
        if Player and Player.Character and Player.Character.RootPart and not Busy and not Faulted and Last ~= #History then
            Last = #History
            print("[Lifecycle:YardMenu] Walls " .. tostring(BlocksAdded[Wall]))
        end
    end
end)
"""
    WriteJson(ScenePath, Scene)
    Stage = Root / "Stage"
    Run([Options.packager, "build", "--project", Project, "--runtime", Options.runtime,
         "--output", Stage, "--configuration", "Release"], Root)
    Installed = Root / "Installed"
    Stage.rename(Installed)
    Args = [Installed / "GargantuanPlayer.exe", "--profile-root", Root, "--max-frames", "2400", "--sample-presented-pixel"]
    WorldId = None
    for Generation in (1, 2):
        Log = Root / f"Menu{Generation}.log"
        with Log.open("wb") as Stream, subprocess.Popen(Args, cwd=Root, env=ManagedEnvironment(), stdout=Stream,
                stderr=subprocess.STDOUT, **ConsoleCapture()) as Player:
            try:
                Window, Post, Place, _ = FindPlayerWindow(Player)
                User.ClientToScreen.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.POINT)]
                User.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
                def Choose(X, Y):
                    # SDL snapshots the system pointer when Windows dispatches a button.
                    # Position only this test window, leaving the desktop pointer alone.
                    Cursor, Origin, Bounds = wintypes.POINT(), wintypes.POINT(), wintypes.RECT()
                    assert User.GetCursorPos(ctypes.byref(Cursor)) and User.ClientToScreen(Window, ctypes.byref(Origin))
                    assert User.GetWindowRect(Window, ctypes.byref(Bounds))
                    assert Place(Window, None, Bounds.left + Cursor.x - Origin.x - X,
                                 Bounds.top + Cursor.y - Origin.y - Y, 0, 0, 0x15)
                    Click(Window, Post, X, Y)
                def Wait(Marker, Count=1):
                    Deadline = time.monotonic() + 15
                    while True:
                        Output = Log.read_text(errors="replace")
                        if Output.count("[Runtime:Luau] [Lifecycle:YardMenu] " + Marker) >= Count:
                            return Output
                        if Player.poll() is not None or time.monotonic() > Deadline:
                            raise RuntimeError(f"Missing {Marker} occurrence {Count}\n" + Output[-3500:])
                        time.sleep(0.04)
                def Key(Virtual, Scan):
                    Parameter = 1 | (Scan << 16)
                    assert Post(Window, 0x100, Virtual, Parameter)
                    time.sleep(0.09)
                    assert Post(Window, 0x101, Virtual, Parameter | 0xC0000000)
                if Generation == 1:
                    Wait("Empty")
                    Choose(80, 120)
                Output = Wait("Selected ")
                CurrentId = re.search(r"\[Lifecycle:YardMenu\] Selected ([0-9a-fA-F]{32})", Output)
                assert CurrentId and (WorldId is None or WorldId == CurrentId[1]), Output[-2300:]
                WorldId = CurrentId[1]
                Choose(120, 164)
                Wait("Playing " + WorldId)
                Wait("Walls " + str(Generation - 1))
                if Generation == 1:
                    Key(ord("B"), 0x30)
                    Wait("Walls 1")
                Key(0x1B, 0x01)
                Wait("Paused " + WorldId)
                Key(ord("B"), 0x30)
                Choose(80, 76)
                Wait("Playing " + WorldId, 2)
                Key(0x1B, 0x01)
                Wait("Paused " + WorldId, 2)
                Choose(245, 76)
                Wait("Selected " + WorldId, 2)
                Choose(120, 208)
                assert Player.wait(timeout=6) == 0
                Output = Log.read_text(errors="replace")
                assert "[Lifecycle:YardMenu] Walls 2" not in Output and "Reached requested frame limit" not in Output
            finally:
                if Player.poll() is None:
                    Player.kill()
                    Player.wait(timeout=5)
    print("[Lifecycle:YardMenu] Created, built, paused, resumed and reopened one durable world through its authored menu")


def Main():
    Parser = argparse.ArgumentParser()
    for Name in ("packager", "runtime", "engine"):
        Parser.add_argument("--" + Name, type=lambda Value: Path(Value).resolve(), required=True)
    Parser.add_argument("--authored-menu", action="store_true")
    Parser.add_argument("--graphical", action="store_true")
    Options = Parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="gly-") as Scratch:
        Root = Path(Scratch)
        Project = Root / "Yard"
        Run([sys.executable, "-B", Samples / "StandaloneConstructionYard" / "BuildProject.py",
             "--engine", Options.engine, "--project", Project], Root)
        ScenePath = Project / ".gargantuan" / "project.instance.json"
        Scene = json.loads(ScenePath.read_bytes())
        Entry = next(Item for Item in Scene["Children"] if Item["Name"] == "Application")["Children"][0]
        assert Scene["Properties"]["ApplicationEntry"] == Entry["Properties"]["SourceId"]
        if Options.authored_menu:
            PlayAuthoredMenu(Root, Project, ScenePath, Scene, Entry, Options)
            return
        Entry["Properties"]["Source"]["String"] = MenuSource
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
        Args = [Player] if Options.graphical else [Player, "--headless"]
        Args.extend(["--profile-root", Root, "--max-frames", "1200"])
        for Fresh in (True, False):
            if not Fresh:
                # Terminate the owned process after it has reopened and verified the
                # committed first world. The next Player must acquire that same writer.
                CrashLog = Root / "Interrupted.log"
                with CrashLog.open("wb") as Stream, subprocess.Popen(
                        [str(Arg) for Arg in Args], cwd=Root, env=Environment, stdout=Stream,
                        stderr=subprocess.STDOUT, **ConsoleCapture()) as Interrupted:
                    try:
                        Deadline = time.monotonic() + 25
                        while b"[Lifecycle:Yard] Hillside initial=1 exterior=1" not in CrashLog.read_bytes():
                            if Interrupted.poll() is not None or time.monotonic() >= Deadline:
                                raise RuntimeError("Construction Yard did not reopen before interruption")
                            time.sleep(0.025)
                    finally:
                        if Interrupted.poll() is None:
                            Interrupted.kill()
                        Interrupted.wait(timeout=8)
            Output = Run(Args, Root, Environment)
            for Name, Count in (("Hillside", 1), ("Riverside", 2)):
                Marker = f"[Lifecycle:Yard] {Name} initial={0 if Fresh else Count} exterior={Count}"
                if Marker not in Output:
                    raise RuntimeError(Marker + " missing\n" + Output[-4500:])
            if "[Lifecycle:Yard] Returned from both worlds" not in Output or "Reached requested frame limit" in Output:
                raise RuntimeError("Construction Yard did not quit successfully\n" + Output[-3000:])
        print("[Lifecycle:Yard] Verified two durable construction histories across standalone Player restarts")


if __name__ == "__main__":
    Main()
