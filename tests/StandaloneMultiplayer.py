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
import signal
import socket
import subprocess
import tempfile
import time
from contextlib import ExitStack

from RuntimeDistributionQualification import CodeNode, Node
from StandaloneHostInvitations import InvitationChannel
from SpatialConstructionPackageSession import ConsoleCapture, ManagedEnvironment, Run, WriteJson

def ControlOwnedServer(Player, Root, Suspend=False):
    if os.name == "nt":
        Shell = Path(os.environ["SystemRoot"]) / "System32/WindowsPowerShell/v1.0/powershell.exe"
        Script = """
$ErrorActionPreference='Stop'
$Rows=@(Get-CimInstance Win32_Process -Filter ("ParentProcessId="+$env:QUALIFICATION_PLAYER_PID) |
    Where-Object Name -eq 'GargantuanServer.exe')
if($Rows.Count -ne 1){throw 'Owned Server count differs'}
$Process=Get-Process -Id $Rows[0].ProcessId
try {
    if(!$Process.Path.StartsWith($env:QUALIFICATION_PROFILE_ROOT+'\\',[StringComparison]::OrdinalIgnoreCase) -or
       $Process.Path -cne $Rows[0].ExecutablePath){throw 'Owned Server image differs'}
    if($env:QUALIFICATION_CHILD_ACTION -eq 'Suspend'){
        Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public static class OwnedThreads { [DllImport("kernel32")] public static extern IntPtr OpenThread(uint rights, bool inherit, uint thread); [DllImport("kernel32")] public static extern uint SuspendThread(IntPtr handle); [DllImport("kernel32")] public static extern bool CloseHandle(IntPtr handle); }'
        foreach($Thread in $Process.Threads){
            $Handle=[OwnedThreads]::OpenThread(2,$false,$Thread.Id)
            if($Handle -eq [IntPtr]::Zero){throw 'Owned thread unavailable'}
            try {if([OwnedThreads]::SuspendThread($Handle) -eq [uint32]::MaxValue){throw 'Owned thread suspension failed'}}
            finally {[void][OwnedThreads]::CloseHandle($Handle)}
        }
    }else{
        $Process.Kill()
        if(!$Process.WaitForExit(5000)){throw 'Owned Server is still running'}
    }
} finally {$Process.Dispose()}
"""
        subprocess.run([Shell, "-NoProfile", "-NonInteractive", "-Command", Script], check=True, timeout=12,
                       env=dict(os.environ, QUALIFICATION_PLAYER_PID=str(Player.pid), QUALIFICATION_PROFILE_ROOT=str(Root),
                                QUALIFICATION_CHILD_ACTION="Suspend" if Suspend else "Stop"),
                       creationflags=subprocess.CREATE_NO_WINDOW)
    else:
        Children = Path(f"/proc/{Player.pid}/task/{Player.pid}/children").read_text().split()
        Owned = [int(Pid) for Pid in Children if (Image := Path(f"/proc/{Pid}/exe").resolve()).name == "GargantuanServer"
                 and Image.is_relative_to(Root)]
        if len(Owned) != 1:
            raise RuntimeError("Expected one owned Server in the test profile")
        Handle = os.pidfd_open(Owned[0])
        try:
            signal.pidfd_send_signal(Handle, signal.SIGSTOP if Suspend else signal.SIGKILL)
        finally:
            os.close(Handle)

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
    local Rejoined = Sessions:Reconnect():Await()
    assert(Rejoined.Ok and Rejoined.Value.Generation ~= Joined.Value.Generation and Rejoined.Value.Mode == "Remote")
    assert(Sessions:Leave():Await().Ok)
    print("[Lifecycle:Multiplayer] Reconnected with a fresh session generation")
    assert(game:GetService("ApplicationService"):Quit().Ok)
    print("[Lifecycle:Multiplayer] Left remote session")
end)
"""


def Main():
    Parser = argparse.ArgumentParser()
    for Name in ("packager", "runtime", "server-runtime", "project"):
        Parser.add_argument("--" + Name, type=lambda Value: Path(Value).resolve(), required=True)
    Parser.add_argument("--export-host-fixture", type=Path, help="stage relocated packages for a separate two-device acceptance run")
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

        def Launch(Args, Name, Extra=None, Capture=None):
            Log = Root / (Name + ".log")
            File = Resources.enter_context(Log.open("wb"))
            Process = subprocess.Popen([str(Arg) for Arg in Args], cwd=Root, env=Env | (Extra or {}),
                                       stdout=File, stderr=subprocess.STDOUT, **(Capture if Capture is not None else ConsoleCapture()))
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
                    ("Application overlay active", "Remote beat advanced", "Owner GUI hydrated", "Reconnected with a fresh session generation", "Left remote session")):
                raise RuntimeError("Client did not finish the shared session\n" + Output[-4000:] + "\n" + ServerLog.read_text(errors="replace")[-1600:])
        if "[Lifecycle:Multiplayer] Two peers advanced" not in ServerLog.read_text(errors="replace"):
            raise RuntimeError("Missing independent Server authority receipt")
        print("[Lifecycle:Multiplayer] Two menu-driven Players observed live shared authority and left")

        PrivateEntry = Entry[:Entry.index("    local Joined")] + """
    local Saves = game:GetService("WorldSaveService")
    local Page = Saves:List():Await()
    assert(Page.Ok, Page.Code)
    local Slot
    for _, Item in Page.Value.Items do
        if Item.Name == "Private world" then Slot = Item end
    end
    if not Slot then
        local Created = Saves:Create({Name = "Private world", Seed = "Private seed"}):Await()
        assert(Created.Ok, Created.Code)
        Slot = Created.Value
    end
    for Cycle = 1, 2 do
        local Opened = Sessions:StartLocal({PrivateServer = true, SaveId = Slot.SaveId}):Await()
        if not Opened.Ok and Opened.Code == "SaveInUse" then
            local Ack = Saves:Create({Name = "Writer denied", Seed = Slot.SaveId}):Await()
            assert(Ack.Ok, Ack.Code)
            print("[Lifecycle:PrivateHost] Concurrent writer denied")
            assert(game:GetService("ApplicationService"):Quit().Ok)
            return
        end
        assert(Opened.Ok and Opened.Value.SaveId == Slot.SaveId and Opened.Value.WorldId == Slot.WorldId, "Start cycle " .. Cycle .. ": " .. tostring(Opened.Code))
        print("[Lifecycle:PrivateHost] Opened " .. Slot.SaveId)
        assert(Opened.Ok and Opened.Value.Mode == "PrivateHost", Opened.Code)
        local Run = game:GetService("RunService")
        local Held = Run:PauseGameplay():Await()
        assert(Held.Ok and Held.Value and Run:IsGameplayPaused(), Held.Code)
        print("[Lifecycle:PrivateHost] Held")
        if Cycle == 1 then
            local Observed = false
            for Attempt = 1, 160 do
                local Records = Saves:List():Await()
                assert(Records.Ok, Records.Code)
                for _, Item in Records.Value.Items do
                    if Item.Name == "Writer denied" and Item.Seed == Slot.SaveId then Observed = true end
                end
                if Observed then break end
                task.wait(0.1)
            end
            assert(Observed, "Competing writer did not acknowledge denial")
        end
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
        def InstallTitle(Name, Body):
            Controller["Properties"]["Source"]["String"] = Entry[:Entry.index("    local Joined")] + Body + "\nend)"
            WriteJson(PathValue, Scene)
            return Package(Options.runtime, Name, "DistributedPlayer") / ("GargantuanPlayer" + Exe)
        HostPlayer = InstallTitle("SharedHost", """
    local Run = game:GetService("RunService")
    local Saves = game:GetService("WorldSaveService")
    local Created = Saves:Create({Name = "Hosted world", Seed = "0041"}):Await()
    assert(Created.Ok, Created.Code)
    local Hosted = Sessions:Host({SaveId = Created.Value.SaveId, Discovery = "LAN", MaxPlayers = 2}):Await()
    assert(Hosted.Ok and Hosted.Value.Mode == "MultiplayerHost" and Hosted.Value.WorldId == Created.Value.WorldId, Hosted.Code)
    assert(Run:PauseGameplay():Await().Code == "NotAuthorized" and not Run:IsGameplayPaused())
    local Done = false
    while not Done do
        local Page = Saves:List():Await()
        assert(Page.Ok, Page.Code)
        for _, Item in Page.Value.Items do if Item.Name == "Guests finished" then Done = true end end
        task.wait(0.05)
    end
    assert(Sessions:Leave():Await().Ok)
    print("[Lifecycle:Hosting] Owner retired shared world")
    assert(game:GetService("ApplicationService"):Quit().Ok)
""")
        GuestBody = """
    local Page = Sessions:FindServers("LAN"):Await()
    assert(Page.Ok and #Page.Value.Items == 1)
    local Joined = Sessions:Join({Kind = "Listing", Id = Page.Value.Items[1].Id}):Await()
"""
        GuestSession = """
    assert(Joined.Ok and Joined.Value.Mode == "Remote", Joined.Code)
    assert(game:GetService("RunService"):PauseGameplay():Await().Code == "NotAuthorized")
    task.wait(0.9)
    assert(Sessions:Leave():Await().Ok)
    for Attempt = 1, 120 do
        local Available = Sessions:FindServers("LAN"):Await()
        if #Available.Value.Items == 1 then break end
        task.wait(0.05)
    end
    local Rejoined = Sessions:Reconnect():Await()
    assert(Rejoined.Ok and Rejoined.Value.Generation ~= Joined.Value.Generation, Rejoined.Code)
    task.wait(0.4)
    assert(Sessions:Leave():Await().Ok)
    print("[Lifecycle:Hosting] Reconnected with fresh native admission")
    assert(game:GetService("ApplicationService"):Quit().Ok)
"""
        GuestPlayer = InstallTitle("SharedGuest", GuestBody + GuestSession)
        ReplayPlayer = InstallTitle("SharedReplay", GuestBody + """
    assert(not Joined.Ok and Joined.Code == "NotAuthorized" and Sessions:GetSession().State == "Idle", Joined.Code)
    assert(game:GetService("WorldSaveService"):Create({Name = "Guests finished"}):Await().Ok)
    print("[Lifecycle:Hosting] Spent guest invitation denied")
    assert(game:GetService("ApplicationService"):Quit().Ok)
""")
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as Available:
            Available.bind(("127.0.0.1", 0))
            HostAddress = f"127.0.0.1:{Available.getsockname()[1]}"
        if Options.export_host_fixture:
            Output = Options.export_host_fixture.resolve()
            Output.mkdir(parents=True, exist_ok=False)
            for PackageRoot in (Server, HostPlayer.parent, GuestPlayer.parent, ReplayPlayer.parent):
                shutil.copytree(PackageRoot, Output / PackageRoot.name)
            WriteJson(Output / "HostFixture.json", {"Version": 1, "ServerManifestSha256": Pin,
                      "GuestManifestSha256": hashlib.sha256((GuestPlayer.parent / "game.package.json").read_bytes()).hexdigest()})
            return
        DirectoryPlayer = InstallTitle("DirectoryGuest", """
    local Target = nil
    local CancelId, WithdrawId
    for Attempt = 1, 150 do
        local Page = Sessions:FindServers("LAN"):Await()
        assert(Page.Ok, Page.Code)
        if Page.Value.NextCursor then
            assert(#Page.Value.Items == 16)
            local Last = Sessions:FindServers("LAN", Page.Value.NextCursor):Await()
            if #Last.Value.Items == 1 and Last.Value.Items[1].Name == "Playable session" then
                Target = Last.Value.Items[1]
                CancelId, WithdrawId = Page.Value.Items[1].Id, Page.Value.Items[2].Id
                assert(not Target.Endpoint and not Target.ServerPublicKeyHex and not Target.GuestProof)
                break
            end
        end
        task.wait(0.02)
    end
    assert(Target and Target.Id)
    assert(Sessions:FindServers("Node"):Await().Code == "Unavailable")
    local Pending = Sessions:Join({Kind = "Listing", Id = CancelId})
    for Attempt = 1, 150 do
        local Page = Sessions:FindServers("LAN"):Await()
        if Page.Value.Items[1].Name == "Grant pending" then break end
        task.wait(0.02)
    end
    assert(Pending:Cancel() and Pending:Await().Code == "Cancelled")
    local Removed = Sessions:Join({Kind = "Listing", Id = WithdrawId}):Await()
    assert(Removed.Code == "NotAuthorized")
    assert(Sessions:Join({Kind = "Listing", Id = WithdrawId}):Await().Code == "NotFound")
    local Joined = Sessions:Join({Kind = "Listing", Id = Target.Id}):Await()
""" + GuestSession.replace("if #Available.Value.Items == 1", "if #Available.Value.Items == 16"))
        with InvitationChannel() as (Handle, Capture, Receive, _):
            for Wildcard in ("0.0.0.0", "[0:0:0:0:0:0:0:0]", "[::ffff:0.0.0.0]"):
                Rejected, RejectedLog = Launch([HostPlayer, *Args[1:], "--local-host-bind", Wildcard + ":35000",
                                                "--local-host-invitations", Handle], "Wildcard" + str(len(Wildcard)), Capture=Capture)
                assert Rejected.wait(timeout=5) == 2 and "[Lifecycle:Hosting]" in RejectedLog.read_text(errors="replace")
            Hosting, HostingLog = Launch([HostPlayer, *Args[1:], "--local-host-bind", HostAddress,
                                          "--local-host-invitations", Handle], "Hosting", Capture=Capture)
            Invitation = Receive()
            assert Invitation["Action"] == "Publish" and Invitation["Source"] == "LAN"
            assert Invitation["PackageSHA256"] == Pin and len(Invitation["GuestProofs"]) == 1
            Extra = {"LIFECYCLE_GUEST": Invitation["GuestProofs"][0], "LIFECYCLE_SERVER": Invitation["ServerPublicKeyHex"]}
            NoKey, NoKeyLog = Launch([GuestPlayer, *Args[1:], "--connect", Invitation["Endpoint"],
                                   "--join-proof-env", "LIFECYCLE_GUEST"], "MissingServerKey",
                                  {"LIFECYCLE_GUEST": Extra["LIFECYCLE_GUEST"]})
            assert NoKey.wait(timeout=5) == 2 and "pinned endpoint and native join proof" in NoKeyLog.read_text(errors="replace")
            JoinArgs = [*Args[1:], "--connect", Invitation["Endpoint"], "--join-proof-env", "LIFECYCLE_GUEST",
                        "--server-key-env", "LIFECYCLE_SERVER", "--join-listing-source", "LAN",
                        "--join-listing-id", Invitation["SessionID"], "--join-expires-ms", Invitation["ExpiresUnixMillis"]]
            with InvitationChannel() as (GuestHandle, GuestCapture, _, Deliver):
                Guest, GuestLog = Launch([GuestPlayer, *JoinArgs, "--join-refresh-channel", GuestHandle],
                                         "HostedGuest", Extra, GuestCapture)
                Fresh = Receive()
                assert Fresh["Action"] == "Refresh" and Fresh["SessionID"] == Invitation["SessionID"] and Fresh["Seat"] == 1
                assert Fresh["GuestProof"] != Extra["LIFECYCLE_GUEST"]
                Deliver(Fresh)
                assert Guest.wait(timeout=15) == 0
                GuestOutput = GuestLog.read_text(errors="replace")
                assert "Remote beat advanced" in GuestOutput and "Reconnected with fresh native admission" in GuestOutput
            with InvitationChannel() as (DirectoryHandle, GuestCapture, GetRequest, Deliver):
                Directory, DirectoryLog = Launch([DirectoryPlayer, *Args[1:], "--session-directory-source", "LAN",
                                                  "--session-directory-channel", DirectoryHandle], "Directory", Capture=GuestCapture)
                Manifest = hashlib.sha256((DirectoryPlayer.parent / "game.package.json").read_bytes()).hexdigest()
                Offer = {Key: Invitation[Key] for Key in ("Version", "Source", "SessionID", "ProjectID", "Endpoint", "ServerPublicKeyHex")}
                Offer.update(Action="Offer", Name="Playable session", PlayerPackageSHA256=Manifest,
                             ExpiresUnixMillis=int(time.time() * 1000) + 50_000)
                # IDs below the real session exercise cursor ordering without allocating a guest seat.
                Prefix = int(Offer["SessionID"], 16)
                for Index in range(1, 17):
                    Deliver(Offer | {"SessionID": f"{Prefix - Index:032x}", "Name": "Other host"})
                Deliver(Offer | {"SessionID": "0" * 31 + "1", "ProjectID": "wrong-project"})
                Deliver(Offer)
                Cancelled = GetRequest()
                assert Cancelled["SessionID"] == f"{Prefix - 16:032x}"
                Deliver(Offer | {"SessionID": Cancelled["SessionID"], "Name": "Grant pending"})
                Removed = GetRequest()
                assert Removed["SessionID"] == f"{Prefix - 15:032x}"
                Deliver(Offer | {"Action": "Withdraw", "SessionID": Removed["SessionID"]})
                Requests = set()
                for Attempt in range(2):
                    Request = GetRequest()
                    assert Request["Action"] == "Request" and Request["SessionID"] == Offer["SessionID"]
                    assert Request["PlayerPackageSHA256"] == Manifest and Request["RequestID"] not in Requests
                    Requests.add(Request["RequestID"])
                    Seat = Receive()
                    assert Seat["Action"] == "Refresh" and Seat["SessionID"] == Offer["SessionID"]
                    Grant = Offer | {"Action": "Grant", "GuestProof": Seat["GuestProof"], "RequestID": Request["RequestID"],
                                     "ExpiresUnixMillis": Seat["ExpiresUnixMillis"]}
                    Deliver(Grant | {"RequestID": "0" * 32, "GuestProof": Extra["LIFECYCLE_GUEST"]})
                    Deliver(Grant)
                assert Directory.wait(timeout=15) == 0
                Output = DirectoryLog.read_text(errors="replace")
                assert all(Marker in Output for Marker in ("Remote beat advanced", "Owner GUI hydrated", "Reconnected with fresh native admission"))
            Replay, ReplayLog = Launch([ReplayPlayer, *JoinArgs], "HostedReplay", Extra)
            assert Replay.wait(timeout=15) == 0 and "Spent guest invitation denied" in ReplayLog.read_text(errors="replace")
            assert Hosting.wait(timeout=15) == 0 and "Owner retired shared world" in HostingLog.read_text(errors="replace")
            Next = Receive()
            if Next["Action"] == "Refresh":
                assert Next["SessionID"] == Invitation["SessionID"] and Next["GuestProof"] != Fresh["GuestProof"]
                Next = Receive()
            assert Next == {"Version": 1, "Action": "Withdraw", "Source": "LAN", "SessionID": Invitation["SessionID"]}
        Controller["Properties"]["Source"]["String"] = PrivateEntry
        WriteJson(PathValue, Scene)
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

        if os.name != "nt" and not (Path("/proc/self/task").is_dir() and hasattr(os, "pidfd_open") and hasattr(signal, "pidfd_send_signal")):
            print("[Lifecycle:PrivateHost] Owned child interruption requires a supported process handle")
            return
        Controller["Properties"]["Source"]["String"] = Entry[:Entry.index("    local Joined")] + """
    local Saves = game:GetService("WorldSaveService")
    local Run = game:GetService("RunService")
    local Saved = Saves:Create({Name = "Distributed recovery", Seed = "paired"}):Await()
    assert(Saved.Ok, Saved.Code)
    local Options = {SaveId = Saved.Value.SaveId, PrivateServer = true}
    local Opened = Sessions:StartLocal(Options):Await()
    assert(Opened.Ok and Opened.Value.SaveId == Saved.Value.SaveId, Opened.Code)
    assert(Run:PauseGameplay():Await().Value)
    local States = {}
    local Terminal
    local Observer = Sessions.Changed:Connect(function(Info)
        assert(Sessions:GetSession().State == Info.State and table.isfrozen(Info))
        table.insert(States, Info.State)
        if Info.State == "Leaving" and Info.LastError then
            assert(Info.LastError.Code == "Disconnected" and Info.WorldId == Saved.Value.WorldId)
            assert(Sessions:StartLocal(Options):Await().Code == "Busy")
        end
        if Info.State == "Idle" then Terminal = Info.LastError end
    end)
    print("[Lifecycle:PrivateHost] Ready for owned child interruption")
    assert(Run.GameplayPauseChanged:Wait() == false)
    assert(not Run:IsGameplayPaused() and Sessions:GetSession().State == "Idle")
    assert(table.concat(States, ",") == "Leaving,Idle")
    assert(Terminal and Terminal.Code == Sessions:GetSession().LastError.Code)
    Observer:Disconnect()
    local Restart = Sessions:StartLocal(Options)
    assert(Sessions:GetSession().LastError == nil)
    local Again = Restart:Await()
    assert(Again.Ok and Again.Value.WorldId == Saved.Value.WorldId, Again.Code)
    assert(Sessions:Leave():Await().Ok)
    print("[Lifecycle:PrivateHost] Reopened paired world after held child exit")
    assert(game:GetService("ApplicationService"):Quit().Ok)
end)
"""
        WriteJson(PathValue, Scene)
        DistributedPrivate = Package(Options.runtime, "DistributedPrivate", "DistributedPlayer")
        Process, Log = Launch([DistributedPrivate / ("GargantuanPlayer" + Exe), *Args[1:]], "DistributedRecovery")
        Deadline = time.monotonic() + 20
        while "[Lifecycle:PrivateHost] Ready for owned child interruption" not in Log.read_text(errors="replace"):
            if Process.poll() is not None or time.monotonic() >= Deadline:
                raise RuntimeError("Distributed private Server did not enter hold\n" + Log.read_text(errors="replace")[-3000:])
            time.sleep(0.025)
        ControlOwnedServer(Process, Root)
        InterruptedStatus = Process.wait(timeout=25)
        Interrupted = Log.read_text(errors="replace")
        if InterruptedStatus != 8 or not all(Marker in Interrupted for Marker in (
            "[Lifecycle:PrivateHost] Reopened paired world after held child exit",
            "[Lifecycle:Storage] Retired private server write status UnknownOutcome",
        )):
            raise RuntimeError("Interrupted private world did not recover\n" + Log.read_text(errors="replace")[-3000:])

        Stalled = Controller["Properties"]["Source"]["String"].replace(
            'print("[Lifecycle:PrivateHost] Ready for owned child interruption")\n    assert(Run.GameplayPauseChanged:Wait() == false)',
            """
    print("[Lifecycle:PrivateHost] Ready for incomplete drain")
    local Received = false
    while not Received do
        local Listed = Saves:List():Await()
        assert(Listed.Ok)
        for _, Item in Listed.Value.Items do
            if Item.Name == "Release drain test" then Received = true end
        end
        if not Received then task.wait(0.05) end
    end
    local Left = Sessions:Leave():Await()
    assert(not Left.Ok and Left.Code == "UnknownOutcome", Left.Code)
    assert(Terminal and Terminal.Code == Left.Code and Terminal.Message == Left.Message)
""")
        assert "Ready for incomplete drain" in Stalled
        Controller["Properties"]["Source"]["String"] = Stalled
        WriteJson(PathValue, Scene)
        StalledPlayer = Package(Options.runtime, "StalledPrivate", "DistributedPlayer")
        Controller["Properties"]["Source"]["String"] = Entry[:Entry.index("    local Joined")] + """
    assert(game:GetService("WorldSaveService"):Create({Name="Release drain test"}):Await().Ok)
    assert(game:GetService("ApplicationService"):Quit().Ok)
end)
"""
        WriteJson(PathValue, Scene)
        Release = Package(Options.runtime, "DrainRelease", "DistributedPlayer")
        Process, Log = Launch([StalledPlayer / ("GargantuanPlayer" + Exe), *Args[1:]], "InterruptedDrain")
        def WaitMarker(Marker):
            Deadline = time.monotonic() + 20
            while Marker not in Log.read_text(errors="replace"):
                if Process.poll() is not None or time.monotonic() >= Deadline:
                    raise RuntimeError("Missing " + Marker + "\n" + Log.read_text(errors="replace")[-3000:])
                time.sleep(0.025)
        WaitMarker("Ready for incomplete drain")
        ControlOwnedServer(Process, Root, Suspend=True)
        Helper, _ = Launch([Release / ("GargantuanPlayer" + Exe), *Args[1:]], "WakeDrain")
        WaitMarker("Awaiting authoritative write drain")
        ControlOwnedServer(Process, Root)
        HelperStatus = Helper.wait(timeout=15)
        StalledStatus = Process.wait(timeout=25)
        StalledOutput = Log.read_text(errors="replace")
        if HelperStatus or StalledStatus != 8 or not all(Marker in StalledOutput for Marker in (
            "Reopened paired world after held child exit",
            "[Lifecycle:Storage] Retired private server write status UnknownOutcome",
        )):
            raise RuntimeError("Unacknowledged private drain did not recover\n" + Log.read_text(errors="replace")[-3000:])


if __name__ == "__main__":
    Main()
