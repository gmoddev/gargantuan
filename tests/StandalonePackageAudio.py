# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Measure rendered PCM from two authored Sounds in a relocated graphical Player."""
import argparse
from array import array
import copy
import json
import math
import os
from pathlib import Path
import sys
import tempfile
import wave

from StandaloneLifecyclePlayer import PrepareProject
from SpatialConstructionPackageSession import ManagedEnvironment, Run, WriteJson

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "samples" / "TerrainQuarry"))
from BuildProject import EditorHost


def MakeTone(File, Channel):
    Samples = array("h")
    for Frame in range(4800):
        Pair = [0, 0]
        Pair[Channel] = int(2800 * math.sin(Frame * math.tau / 100))
        Samples.extend(Pair)
    if sys.byteorder != "little":
        Samples.byteswap()
    with wave.open(str(File), "wb") as Stream:
        Stream.setparams((2, 2, 48000, 0, "NONE", "not compressed"))
        Stream.writeframes(Samples.tobytes())


def Main():
    Parser = argparse.ArgumentParser()
    for Name in ("packager", "runtime", "engine", "project"):
        Parser.add_argument("--" + Name, type=lambda Arg: Path(Arg).resolve(), required=True)
    Options = Parser.parse_args()
    Options.cycles = 1
    with tempfile.TemporaryDirectory(prefix="gpa-") as Scratch:
        Root = Path(Scratch)
        Project = PrepareProject(Options, Root, "Menu")
        Refs = []
        with EditorHost(Options.engine) as Host:
            Host.Call("OpenProject", Root=Project.as_posix())
            for Channel, Name in enumerate(("MenuTone", "WorldTone")):
                Source = "assets/" + Name + ".wav"
                MakeTone(Project / Source, Channel)
                Result = Host.Call("ImportAsset", Source=Source, Kind="Audio")
                assert Result["OperationSucceeded"], Result["Diagnostic"]
                Refs.append(Result["Asset"]["Reference"])
            Host.Call("SaveProject")
        File = Project / ".gargantuan" / "project.instance.json"
        Scene = json.loads(File.read_bytes())
        App = next(Node for Node in Scene["Children"] if Node["Name"] == "Application")
        Game = next(Node for Node in Scene["Children"] if Node["Name"] == "GameScripts")
        Original = json.loads((Options.project / ".gargantuan" / "project.instance.json").read_bytes())
        Pending = [Original]
        while Pending:
            Prototype = Pending.pop()
            if Prototype["ClassName"] == "Sound":
                break
            Pending.extend(Prototype["Children"])
        assert Prototype["ClassName"] == "Sound"
        Nodes = [Scene]
        NodeIds = []
        while Nodes:
            Node = Nodes.pop()
            NodeIds.append(Node.get("NodeId", 0))
            Nodes.extend(Node["Children"])
        NextId = max(NodeIds) + 1
        for Folder, Ref in zip((App, Game), Refs):
            Sound = copy.deepcopy(Prototype)
            if Scene["Version"] >= 5:
                Sound["NodeId"] = NextId
                NextId += 1
            Sound.update(Name="Tone", Children=[])
            Sound["Properties"].update(SoundId={"String": Ref}, Looped={"Bool": True},
                                       Volume={"Float": 0.7}, PlaybackSpeed={"Float": 1.0})
            Folder["Children"].append(Sound)
        App["Children"][0]["Properties"]["Source"] = {"String": """
local Title = Instance.new("ScreenGui")
Title.Parent = script.Parent
assert(game:SetMainMenu(Title).Ok)
task.defer(function()
    local Run = game:GetService("RunService")
    local Sessions = game:GetService("SessionService")
    script.Parent.Tone:Play()
    task.wait(0.4)
    assert(Sessions:StartLocal():Await().Ok)
    task.wait(0.4)
    assert(Run:PauseGameplay():Await().Value)
    task.wait(0.4)
    assert(Run:ResumeGameplay():Await().Ok)
    task.wait(0.4)
    assert(Sessions:Leave():Await().Ok)
    task.wait(0.4)
    print("[Lifecycle:Sound] Retired")
    assert(game:GetService("ApplicationService"):Quit().Ok)
end)
"""}
        Game["Children"][0]["Properties"]["Source"] = {"String": "script.Parent.Tone:Play()"}
        WriteJson(File, Scene)
        Stage = Root / "Stage"
        Run([Options.packager, "build", "--project", Project, "--runtime", Options.runtime,
             "--output", Stage, "--configuration", "Release"], Root)
        Installed = Root / "Installed"
        Stage.rename(Installed)
        Run([Options.packager, "validate", Installed], Root)
        Pcm = Root / "output.pcm"
        Env = ManagedEnvironment()
        Env.update(SDL_AUDIODRIVER="disk", SDL_AUDIO_DISK_OUTPUT_FILE=str(Pcm),
                   SDL_AUDIO_FORMAT="F32LE", SDL_AUDIO_CHANNELS="2", SDL_AUDIO_FREQUENCY="48000")
        Output = Run([Installed / ("GargantuanPlayer.exe" if os.name == "nt" else "GargantuanPlayer"),
                      "--max-frames", "600"], Root, Env)
        assert "[Lifecycle:Sound] Retired" in Output, Output[-2000:]
        Samples = array("f")
        Samples.frombytes(Pcm.read_bytes())
        if sys.byteorder != "little":
            Samples.byteswap()
        # Ignore boundary blocks. Require five consecutive 10 ms blocks at each
        # stage, including resumed gameplay and menu-only output after Leave.
        Stages = [False, True, False, True, False]
        StageIndex = Consecutive = 0
        for Offset in range(0, len(Samples) - 960, 960):
            Left = max(map(abs, Samples[Offset:Offset + 960:2]))
            Right = max(map(abs, Samples[Offset + 1:Offset + 960:2]))
            Matches = Left > 0.01 and (Right > 0.01 if Stages[StageIndex] else Right < 0.0001)
            Consecutive = Consecutive + 1 if Matches else 0
            if Consecutive >= 5:
                Consecutive = 0
                StageIndex += 1
                if StageIndex == len(Stages):
                    break
        assert StageIndex == len(Stages), (StageIndex, len(Samples), Output[-2000:])
        print("[Lifecycle:Sound] Five source-isolated PCM stages passed in installed Player")


if __name__ == "__main__":
    Main()
