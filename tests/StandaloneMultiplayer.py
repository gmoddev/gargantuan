# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Two installed menu-driven Players join a real Development loopback Server."""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
from contextlib import ExitStack

from RuntimeDistributionQualification import CodeNode, Node
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
            Item["Properties"]["SourceId"] = {"String": hashlib.sha256(Name.encode()).hexdigest()[:32]}
            Item["Properties"].update(Source={"String": Source}, Enabled={"Bool": True}, RunContext={"EnumItem": ["RunContext", Context]})
            return Item

        Folder["Children"] = [Script("Authority", "Server", Authority), Script("Observer", "Client", Observer)]
        Scene["Children"] = [Item for Item in Scene["Children"] if Item["ClassName"] == "AssetService"] + [Folder]
        GuiCode = CodeNode("RawGuiScript", "Script", """
assert(require('./RawGuiModule') == 19)
print("[Lifecycle:Multiplayer] Owner GUI hydrated")
""", "Client")
        Screen = Node("GameScreen", "ScreenGui", Children=[
            GuiCode, CodeNode("RawGuiModule", "ModuleScript", "return 19")])
        Scene["Children"].append(Node("StarterGui", "StarterGui", Children=[Screen]))
        WriteJson(Project / ".gargantuan" / "package-audience.json", {"Version": 1, "SharedAssets": []})
        WriteJson(PathValue, Scene)

        def Package(Runtime, Name, Audience="Trusted"):
            Stage = Root / "Stage"
            Run([Options.packager, "build", "--project", Project, "--runtime", Runtime,
                 "--output", Stage, "--configuration", "Development", "--audience", Audience], Root)
            Installed = Root / Name
            Stage.rename(Installed)
            Run([Options.packager, "validate", Installed], Root)
            return Installed

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
        Server = Package(Options.server_runtime, "Server")
        assert not (Server / "content" / "application.json").exists()
        Client = Package(Options.runtime, "Client")
        Distributed = Package(Options.runtime, "Distributed", "DistributedPlayer")
        Manifest = json.loads((Distributed / "game.package.json").read_bytes())
        assert Manifest["PackageFormatVersion"] == 7
        assert not (Distributed / "content" / "game.instance.json").exists()
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
        Clients = [Launch([Installed / ("GargantuanPlayer" + Exe), "--headless", "--profile-root", Root,
                           "--max-frames", "420"], "Peer" + str(Index)) for Index, Installed in enumerate((Client, Distributed))]
        for Process, Log in Clients:
            Process.wait(timeout=30)
            Output = Log.read_text(errors="replace")
            if Process.returncode or any("[Lifecycle:Multiplayer] " + Marker not in Output for Marker in
                    ("Application overlay active", "Remote beat advanced", "Owner GUI hydrated", "Left remote session")):
                raise RuntimeError("Client did not finish the shared session\n" + Output[-4000:] + "\n" + ServerLog.read_text(errors="replace")[-1600:])
        if "[Lifecycle:Multiplayer] Two peers advanced" not in ServerLog.read_text(errors="replace"):
            raise RuntimeError("Missing independent Server authority receipt")
        print("[Lifecycle:Multiplayer] Two menu-driven Players observed live shared authority and left")

        PrivateEntry = Entry[:Entry.index("    local Joined")] + """
    local Saves = game:GetService("WorldSaveService")
    local Page = Saves:List():Await()
    assert(Page.Ok, Page.Code)
    local Slot = Page.Value.Items[1]
    if not Slot then
        local Created = Saves:Create({Name = "Private world", Seed = "Private seed"}):Await()
        assert(Created.Ok, Created.Code)
        Slot = Created.Value
    end
    for Cycle = 1, 2 do
        local Opened = Sessions:StartLocal({PrivateServer = true, SaveId = Slot.SaveId}):Await()
        if not Opened.Ok and Opened.Code == "SaveInUse" then
            print("[Lifecycle:PrivateHost] Concurrent writer denied")
            assert(game:GetService("ApplicationService"):Quit().Ok)
            return
        end
        assert(Opened.Ok and Opened.Value.SaveId == Slot.SaveId and Opened.Value.WorldId == Slot.WorldId, "Start cycle " .. Cycle .. ": " .. tostring(Opened.Code))
        print("[Lifecycle:PrivateHost] Opened " .. Slot.SaveId)
        assert(Opened.Ok and Opened.Value.Mode == "PrivateHost", Opened.Code)
        task.wait(0.25)
        local Run = game:GetService("RunService")
        local Held = Run:PauseGameplay():Await()
        assert(Held.Ok and Held.Value and Run:IsGameplayPaused(), Held.Code)
        print("[Lifecycle:PrivateHost] Held")
        task.wait(Cycle == 1 and 2.5 or 0.15)
        local Resumed = Run:ResumeGameplay():Await()
        assert(Resumed.Ok and not Resumed.Value, Resumed.Code)
        print("[Lifecycle:PrivateHost] Resumed")
        local Left = Sessions:Leave():Await()
        assert(Left.Ok, "Leave cycle " .. Cycle .. ": " .. tostring(Left.Code))
    end
    print("[Lifecycle:PrivateHost] Two private server generations completed")
    assert(game:GetService("ApplicationService"):Quit().Ok)
end)
"""
        Controller["Properties"]["Source"]["String"] = PrivateEntry
        WriteJson(PathValue, Scene)
        PrivatePlayer = Package(Options.runtime, "PrivatePlayer")
        Pin = hashlib.sha256((Server / "game.package.json").read_bytes()).hexdigest()
        Args = [PrivatePlayer / ("GargantuanPlayer" + Exe), "--headless", "--profile-root", Root,
                "--local-server-root", Server, "--local-server-manifest-sha256", Pin, "--max-frames", "1800"]
        Process, Log = Launch(Args, "PrivateHost")
        Deadline = time.monotonic() + 24
        while "[Lifecycle:PrivateHost] Opened " not in Log.read_text(errors="replace"):
            if Process.poll() is not None or time.monotonic() >= Deadline:
                raise RuntimeError("Private server did not acquire its writer\n" + Log.read_text(errors="replace")[-6000:])
            time.sleep(0.02)
        Competing, Other = Launch(Args, "Competing")
        try:
            CompetingStatus = Competing.wait(timeout=24)
        except subprocess.TimeoutExpired as Error:
            raise RuntimeError("Competing Player did not finish\n" + Other.read_text(errors="replace")[-3500:] +
                               "\nOwner\n" + Log.read_text(errors="replace")[-3500:]) from Error
        if CompetingStatus or "[Lifecycle:PrivateHost] Concurrent writer denied" not in Other.read_text(errors="replace"):
            raise RuntimeError("Concurrent writer was not denied\n" + Other.read_text(errors="replace")[-6000:])
        def CheckOwner(Native, File):
            Status = Native.wait(timeout=35)
            Output = File.read_text(errors="replace")
            Opened = [Line.split("Opened ", 1)[1][:32] for Line in Output.splitlines() if "[Lifecycle:Application] Information [Lifecycle:PrivateHost] Opened " in Line]
            if Status or len(Opened) != 2 or Opened[0] != Opened[1] or "[Lifecycle:PrivateHost] Two private server generations completed" not in Output:
                raise RuntimeError("Private server lifecycle failed\n" + Output[-6000:])
            return Opened[0]
        OriginalSlot = CheckOwner(Process, Log)
        Restart, RestartLog = Launch(Args, "FreshPlayer")
        if CheckOwner(Restart, RestartLog) != OriginalSlot:
            raise RuntimeError("New installed Player opened a different private world")



if __name__ == "__main__":
    Main()
