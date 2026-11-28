"""Measure paired installed Players and settled title resources on Windows."""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time

from StandaloneLifecyclePlayer import PrepareProject
from SpatialConstructionPackageSession import ConsoleCapture, ManagedEnvironment, Run, WriteJson

Measure = """
local Run = game:GetService("RunService")
local function Measure(Name)
    local Samples = {}
    Run.PreRender:Wait()
    local Previous = os.clock()
    for Frame = 1, 190 do
        Run.PreRender:Wait()
        local Now = os.clock()
        if Frame > 10 then table.insert(Samples, 1000 * (Now - Previous)) end
        Previous = Now
    end
    table.sort(Samples)
    print(string.format("[Lifecycle:Timing] %s %.3f %.3f %.3f %.3f", Name, Samples[90], Samples[171], Samples[179], Samples[180]))
end
"""
World = Measure + """
print("[Lifecycle:Ready] Gameplay")
task.defer(function() Measure("Gameplay") end)
"""
Menu = Measure + """
local Title = Instance.new("ScreenGui")
Title.Parent = script.Parent
local Label = Instance.new("TextLabel")
Label.Text = "Construction Yard"
Label.Parent = Title
assert(game:SetMainMenu(Title).Ok)
print("[Lifecycle:Ready] Title")
local Sessions = game:GetService("SessionService")
task.defer(function()
    Measure("Title")
    assert(Sessions:StartLocal():Await().Ok)
    game:SetMainMenuVisible(false)
    Measure("Presentation")
    task.wait(0.12)
    assert(Run:PauseGameplay():Await().Ok)
    game:SetMainMenuVisible(true)
    Measure("Held")
    assert(Sessions:Leave():Await().Ok)
    local LastFrame, LongestGap, LongestStart, LongestLeave = os.clock(), 0, 0, 0
    local Watch = Run.PreRender:Connect(function()
        local Now = os.clock()
        LongestGap = math.max(LongestGap, Now - LastFrame)
        LastFrame = Now
    end)
    for Cycle = 1, 100 do
        local Started = os.clock()
        assert(Sessions:StartLocal():Await().Ok)
        local Active = os.clock()
        assert(Sessions:Leave():Await().Ok)
        LongestStart = math.max(LongestStart, Active - Started)
        LongestLeave = math.max(LongestLeave, os.clock() - Active)
        if Cycle == 10 or Cycle == 50 or Cycle == 100 then
            print("[Lifecycle:Settled] " .. Cycle)
            task.wait(0.4)
        end
    end
    Watch:Disconnect()
    print(string.format("[Lifecycle:Transitions] %.3f %.3f %.3f", 1000 * LongestGap, 1000 * LongestStart, 1000 * LongestLeave))
    assert(game:GetService("ApplicationService"):Quit().Ok)
end)
"""


class Memory(ctypes.Structure):
    _fields_ = [("Size", wintypes.DWORD), ("PageFaults", wintypes.DWORD)] + [
        (Name, ctypes.c_size_t) for Name in ("PeakWorkingSet", "WorkingSet", "PeakPaged", "Paged",
                                             "PeakNonPaged", "NonPaged", "Pagefile", "PeakPagefile", "Private")]


def Usage(Process):
    Handle = wintypes.HANDLE(int(Process._handle))
    Data, Handles = Memory(), wintypes.DWORD()
    Data.Size = ctypes.sizeof(Data)
    if not ctypes.windll.psapi.GetProcessMemoryInfo(Handle, ctypes.byref(Data), Data.Size):
        raise ctypes.WinError()
    if not ctypes.windll.kernel32.GetProcessHandleCount(Handle, ctypes.byref(Handles)):
        raise ctypes.WinError()
    return dict(WorkingSet=Data.WorkingSet, PrivateBytes=Data.Private, Handles=Handles.value)


def Observe(Player, Directory, Application, Headless):
    Log = Directory / "timing.log"
    Args = [Player, "--profile-root", Directory, "--max-frames", "3600" if Application else "210"]
    if Headless:
        Args.append("--headless")
    Settled = {}
    ReadySeconds = None
    Started = time.monotonic()
    with Log.open("wb") as Stream, subprocess.Popen(Args, cwd=Directory, env=ManagedEnvironment(),
            stdout=Stream, stderr=subprocess.STDOUT, **ConsoleCapture()) as Process:
        try:
            while Process.poll() is None:
                Output = Log.read_text(errors="replace")
                if ReadySeconds is None and "[Lifecycle:Ready] " in Output:
                    ReadySeconds = round(time.monotonic() - Started, 3)
                for Cycle in (10, 50, 100):
                    if Cycle not in Settled and f"[Lifecycle:Settled] {Cycle}\n" in Output:
                        Settled[Cycle] = Usage(Process)
                if time.monotonic() - Started > 105:
                    raise TimeoutError("Paired Player did not finish")
                time.sleep(0.05)
        finally:
            if Process.poll() is None:
                Process.kill()
                Process.wait()
    Output = Log.read_text(errors="replace")
    if ReadySeconds is None and "[Lifecycle:Ready] " in Output:
        ReadySeconds = round(time.monotonic() - Started, 3)
    Metrics = {Name: dict(zip(("P50", "P95", "P99", "Maximum"), map(float, Values)))
               for Name, *Values in re.findall(r"\[Lifecycle:Timing\] (\w+) ([.\d]+) ([.\d]+) ([.\d]+) ([.\d]+)", Output)}
    Transitions = re.findall(r"^\[Runtime:Luau\] \[Lifecycle:Transitions\] ([.\d]+) ([.\d]+) ([.\d]+)",
                             Output, re.MULTILINE)
    Needed = {"Gameplay", "Presentation", "Held", "Title"} if Application else {"Gameplay"}
    if Process.returncode or ReadySeconds is None or set(Metrics) != Needed or (Application and
            (set(Settled) != {10, 50, 100} or len(Transitions) != 1)):
        raise RuntimeError(f"Incomplete timing run: exit={Process.returncode}, ready={ReadySeconds}, "
                           f"metrics={sorted(Metrics)}, settled={sorted(Settled)}, transitions={len(Transitions)}\n"
                           + Output[-1200:])
    Result = dict(FrameMilliseconds=Metrics, Settled=Settled, ObservedScriptReadySecondsUpperBound=ReadySeconds,
                  ElapsedSeconds=round(time.monotonic()-Started, 3))
    if Application:
        Result["MaximumTransitionMilliseconds"] = dict(zip(("FrameInterval", "Start", "Leave"), map(float, Transitions[0])))
    return Result


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    for Name in ("project", "packager", "runtime", "output"):
        Parser.add_argument("--" + Name, type=lambda Value: Path(Value).resolve(), required=True)
    Parser.add_argument("--only", choices=("Application", "Direct"))
    Options = Parser.parse_args()
    if os.name != "nt":
        Parser.error("The installed Player resource observer requires Windows")
    Options.cycles = 1
    Results = {}
    print("[Lifecycle:Timing] Preparing paired packages", flush=True)
    with tempfile.TemporaryDirectory(prefix="glt-") as Temp:
        Root = Path(Temp)
        Project = PrepareProject(Options, Root, "Menu")
        File = Project / ".gargantuan/project.instance.json"
        Scene = json.loads(File.read_bytes())
        App = next(Row for Row in Scene["Children"] if Row["Name"] == "Application")
        Game = next(Row for Row in Scene["Children"] if Row["Name"] == "GameScripts")
        Game["Children"][0]["Properties"]["Source"]["String"] = World
        App["Children"][0]["Properties"]["Source"]["String"] = Menu
        for Name in ("Application", "Direct"):
            if Options.only and Name != Options.only:
                continue
            print("[Lifecycle:Timing] Packaging " + Name, flush=True)
            if Name == "Direct":
                Scene["Children"].remove(App)
                del Scene["Properties"]["ApplicationEntry"]
            WriteJson(File, Scene)
            Package = Root / Name
            Run([Options.packager, "build", "--project", Project, "--runtime", Options.runtime, "--output", Package,
                 "--configuration", "Release"], Root)
            Player = Package / "GargantuanPlayer.exe"
            Results[Name] = dict(PlayerSha256=hashlib.sha256(Player.read_bytes()).hexdigest(),
                                 PackageBytes=sum(P.stat().st_size for P in Package.rglob("*") if P.is_file()))
            for Mode in ("Headless", "Window"):
                print("[Lifecycle:Timing] Observing " + Name + " " + Mode, flush=True)
                Results[Name][Mode] = [Observe(Player, Root, Name == "Application", Mode == "Headless") for _ in range(2)]
    with Options.output.open("x", encoding="utf-8") as Output:
        json.dump(Results, Output, indent=2)


if __name__ == "__main__":
    Main()
