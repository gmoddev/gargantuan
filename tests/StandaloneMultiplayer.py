# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Two installed menu-driven Players join a real Development loopback Server."""
import argparse
import copy
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
from contextlib import ExitStack

from SpatialConstructionPackageSession import ConsoleCapture, ManagedEnvironment, Run, WriteJson

Authority = """
local Players = game:GetService("Players")
local Tick = 0
local Reported = false
game:GetService("RunService").PostSimulation:Connect(function()
    if #Players:GetPlayers() >= 2 then
        Tick += 1
        if Tick % 4 == 0 then script.Parent:SetAttribute("PeerBeat", Tick) end
        if Tick >= 16 and not Reported then
            Reported = true
            print("[Lifecycle:Multiplayer] Two peers advanced")
        end
    end
end)
"""
Observer = """
local Initial = nil
game:GetService("RunService").PostSimulation:Connect(function()
    local Beat = script.Parent:GetAttribute("PeerBeat")
    if not Beat then return end
    if not Initial then Initial = Beat end
    if Beat - Initial >= 8 then
        print("[Lifecycle:Multiplayer] Remote beat advanced")
        Initial = Beat
    end
end)
"""
Entry = """
local Title = Instance.new("ScreenGui")
Title.Parent = script.Parent
assert(game:SetMainMenu(Title).Ok)
local Sessions = game:GetService("SessionService")
task.defer(function()
    local Joined = Sessions:Join({Kind = "Direct", Address = "ENDPOINT"}):Await()
    assert(Joined.Ok and Joined.Value.Mode == "Remote")
    assert(game:GetService("RunService"):PauseGameplay():Await().Code == "NotAuthorized")
    assert(game:SetGameplayInputEnabled(false).Ok)
    assert(game:IsMainMenuVisible())
    print("[Lifecycle:Multiplayer] Application overlay active")
    task.wait(1.3)
    assert(Sessions:Leave():Await().Ok)
    assert(Sessions:GetSession().State == "Idle")
    assert(game:GetService("ApplicationService"):Quit().Ok)
    print("[Lifecycle:Multiplayer] Left remote session")
end)
"""


def Main():
    Parser = argparse.ArgumentParser()
    for Name in ("packager", "runtime", "server-runtime", "project"):
        Parser.add_argument("--" + Name, type=lambda Value: Path(Value).resolve(), required=True)
    Options = Parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="glm-") as Scratch, ExitStack() as Resources:
        Root = Path(Scratch)
        Project = Root / "Project"
        shutil.copytree(Options.project, Project)
        PathValue = Project / ".gargantuan" / "project.instance.json"
        Scene = json.loads(PathValue.read_bytes())
        Folder = next(Item for Item in Scene["Children"] if Item["Name"] == "GameScripts")
        Prototype = next(Item for Item in Folder["Children"] if Item["ClassName"] == "Script")

        def Script(Name, Context, Source):
            Item = copy.deepcopy(Prototype)
            Item["Name"] = Name
            Item["Properties"].update(Source={"String": Source}, Enabled={"Bool": True}, RunContext={"EnumItem": ["RunContext", Context]})
            return Item

        Folder["Children"] = [Script("Authority", "Server", Authority), Script("Observer", "Client", Observer)]
        Scene["Children"] = [Item for Item in Scene["Children"] if Item["ClassName"] == "AssetService"] + [Folder]
        WriteJson(PathValue, Scene)

        def Package(Runtime, Name):
            Stage = Root / "Stage"
            Run([Options.packager, "build", "--project", Project, "--runtime", Runtime,
                 "--output", Stage, "--configuration", "Development"], Root)
            Installed = Root / Name
            Stage.rename(Installed)
            Run([Options.packager, "validate", Installed], Root)
            return Installed

        Server = Package(Options.server_runtime, "Server")
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as Available:
            Available.bind(("127.0.0.1", 0))
            Endpoint = "127.0.0.1:" + str(Available.getsockname()[1])
        App = copy.deepcopy(Folder)
        App["Name"] = "Application"
        Controller = Script("Entry", "Client", Entry.replace("ENDPOINT", Endpoint))
        Id = {"String": "841d147af21b4547a1f515cb672cab5f"}
        Controller["Properties"]["SourceId"] = Id
        App["Children"] = [Controller]
        Scene["Children"].append(App)
        Scene["Properties"]["ApplicationEntry"] = Id
        WriteJson(PathValue, Scene)
        Client = Package(Options.runtime, "Client")
        Exe = ".exe" if os.name == "nt" else ""
        Env = ManagedEnvironment()

        def Launch(Args, Name):
            Log = Root / (Name + ".log")
            File = Resources.enter_context(Log.open("wb"))
            Process = subprocess.Popen([str(Arg) for Arg in Args], cwd=Root, env=Env,
                                       stdout=File, stderr=subprocess.STDOUT, **ConsoleCapture())
            def Stop():
                if Process.poll() is None:
                    Process.terminate()
                    Process.wait(timeout=8)
            Resources.callback(Stop)
            return Process, Log

        ServerProcess, ServerLog = Launch([Server / ("GargantuanServer" + Exe), "--bind", Endpoint,
                                           "--max-ticks", "720"], "Authority")
        Deadline = time.monotonic() + 15
        while "[Runtime:Server] StartupWallMilliseconds=" not in ServerLog.read_text(errors="replace"):
            if ServerProcess.poll() is not None or time.monotonic() >= Deadline:
                raise RuntimeError("Server did not start\n" + ServerLog.read_text(errors="replace")[-3000:])
            time.sleep(0.03)
        Clients = [Launch([Client / ("GargantuanPlayer" + Exe), "--headless", "--profile-root", Root,
                           "--max-frames", "420"], "Peer" + str(Index)) for Index in range(2)]
        for Process, Log in Clients:
            Process.wait(timeout=30)
            Output = Log.read_text(errors="replace")
            if Process.returncode or any("[Lifecycle:Multiplayer] " + Marker not in Output for Marker in
                    ("Application overlay active", "Remote beat advanced", "Left remote session")):
                raise RuntimeError("Client did not finish the shared session\n" + Output[-4000:] + "\n" + ServerLog.read_text(errors="replace")[-1600:])
        if "[Lifecycle:Multiplayer] Two peers advanced" not in ServerLog.read_text(errors="replace"):
            raise RuntimeError("Missing independent Server authority receipt")
        print("[Lifecycle:Multiplayer] Two menu-driven Players observed live shared authority and left")


if __name__ == "__main__":
    Main()
