"""Run the ordinary packaged PlayerGui against two owning Players, a respawn and a reconnect."""
import argparse
import json
import re
import socket
import tempfile
import uuid
from contextlib import ExitStack
from pathlib import Path

import RuntimeDistributionQualification as RD
from SpatialConstructionYardMultiplayer import Await, Launch


ClientSource = """
local Players = game:GetService('Players')
local Owner = Players.LocalPlayer
assert(Owner and Owner.PlayerGui == script.Parent.Parent.Parent and Owner.PlayerGui.Parent == Owner)
assert(script.Parent.Name == 'GuiServerContainer')
assert(not game:FindFirstChildOfClass('StarterGui', false))
for _, Other in Players:GetPlayers() do
    if Other ~= Owner then assert(Other.PlayerGui == nil) end
end
assert(script:GetAttribute('StartedOnce') == nil)
script:SetAttribute('StartedOnce', true)
for Attempt = 1, 180 do
    if script.Parent:FindFirstChild('RD1GuiModule', false) then break end
    task.wait()
end
assert(require('./RD1GuiModule').Answer == 42)
local Event = game:GetService('ReplicatedStorage'):FindFirstChild('GuiLifecycleReady', false)
for Attempt = 1, 180 do
    if Event and Event:TryFireServer('boot') then
        warn('[Qualification:GuiBoot] OWNER_' .. tostring(Owner.PlayerId))
        return
    end
    task.wait()
    Event = game:GetService('ReplicatedStorage'):FindFirstChild('GuiLifecycleReady', false)
end
error('GUI readiness did not reach the authority')
"""

ServerSource = """
local Players = game:GetService('Players')
assert(game:GetService('StarterGui').Name == 'AuthoredHudTemplates')
local Event = game:GetService('ReplicatedStorage'):FindFirstChild('GuiLifecycleReady', false)
local First, Second = nil, nil
local Runs = {}
local Reset = false
Event.OnPlayerEvent:Connect(function(Player, Message)
    if Message ~= 'boot' then return end
    assert(Player.PlayerGui and Player.PlayerGui.Parent == Player)
    Runs[Player.PlayerId] = (Runs[Player.PlayerId] or 0) + 1
    if not First then First = Player
    elseif Player ~= First and not Second then Second = Player end
    if Player ~= First and Player ~= Second then
        for _, Current in Players:GetPlayers() do assert(Current ~= First and Current ~= Second) end
        assert(Runs[Player.PlayerId] == 1)
        warn('[Qualification:GuiRejoin] OWNER_' .. tostring(Player.PlayerId))
        return
    end
    assert(Runs[Player.PlayerId] <= 2)
    if First and Second and not Reset then
        Reset = true
        task.defer(function()
            First:LoadCharacter()
            Second:LoadCharacter()
        end)
    elseif First and Second and Runs[First.PlayerId] == 2 and Runs[Second.PlayerId] == 2 then
        warn('[Qualification:GuiPair] OWNER_' .. tostring(First.PlayerId) .. ' OWNER_' .. tostring(Second.PlayerId))
    end
end)
warn('[Qualification:GuiServer] READY')
"""


def Author(Options, Root):
    Project = Root / "Origin"
    _, Document = RD.MakeFixture(Options.Sample, Project, 0)
    Identity = uuid.uuid4().hex
    RD.WriteJson(Document.parent / "project.json", {"Version": 1, "ProjectId": Identity})
    World = RD.ReadJson(Document)
    Nodes = {Node["Name"]: Node for Node, _ in RD.WalkNodes(World)}
    Nodes["ReplicatedStorage"]["Children"].append(RD.Node("GuiLifecycleReady", "RemoteEvent"))
    Nodes["RD1Gui"]["Properties"]["ResetOnSpawn"] = {"Bool": True}
    Nodes["StarterGui"]["Name"] = "AuthoredHudTemplates"
    Container = RD.CodeNode("GuiServerContainer", "Script", "local PrivateValue = 'gui-container-private-source'", "Server")
    Container["Properties"]["RunContext"] = Nodes["RD1AuthoritativeInventory"]["Properties"]["RunContext"]
    Container["Children"] = Nodes["RD1Gui"]["Children"]
    Nodes["RD1Gui"]["Children"] = [Container]
    Nodes["RD1GuiScript"]["Properties"]["Source"]["String"] = ClientSource
    Nodes["RD1AuthoritativeInventory"]["Properties"]["Source"]["String"] = ServerSource
    RD.WriteJson(Document, World)
    RD.WritePolicy(Document.parent / "runtime-distribution.json", uuid.uuid4().hex, False)
    return Project, Identity


def Qualify(Options, Root, Signed, Prior):
    Root.mkdir()
    Project, Identity = Author(Options, Root)
    Preset = "Compiled" if Signed else "Open"
    Security = {}
    for Role, Mode in (("Server", "Host"), ("Player", "Local")):
        BuildFile, RuntimeFile = Root / (Role + "Build.tsv"), Root / (Role + "Runtime.tsv")
        RD.WriteEnrollment(BuildFile, Mode, Identity, Signing=Signed)
        RD.WriteEnrollment(RuntimeFile, Mode, Identity)
        Package = Root / Role
        Receipt, _ = RD.Build(Options, Role, "DedicatedServer" if Role == "Server" else "DistributedPlayer",
                              Preset, Project, Package, Root / "request.json", Root, BuildFile,
                              Signed=Signed, Configuration="Release" if Signed else "Development")
        Env = RD.Environment(Runtime=RuntimeFile)
        Env["SDL_LOGGING"] = "info"
        Trust = RD.TrustArguments(Receipt, Signed)
        RD.Validate(Options.Packager, Package, Role, Root, Env, Trust)
        Security[Role] = (RD.PackageExecutable(Package), Trust, Env, Receipt)
    Player, Flags, PlayerEnv, PlayerReceipt = Security["Player"]
    Server, ServerFlags, ServerEnv, ServerReceipt = Security["Server"]
    Catalog = RD.ReadJson(Player.parent / "content" / "runtime-scripts.json")
    Entries = {Entry["Id"]: Entry for Entry in Catalog["Scripts"]}
    for Name in ("RD1GuiScript", "RD1GuiModule"):
        Entry = Entries.get(RD.ResourceId(Name))
        RD.Require(Entry and Entry["Representation"] == ("Compiled" if Signed else "Source") and
                   Entry["Path"] == ["StarterGui", "RD1Gui", "GuiServerContainer", Name], Name)
    RD.Require(all(RD.ResourceId(Name) not in Entries for Name in
                   ("GuiServerContainer", "PrivateGuiModule", "RD1AuthoritativeInventory", "RD1InventoryRules")),
               "private identity entered client code")
    RD.Require(all(b"gui-container-private-source" not in File.read_bytes()
                   for File in (Player.parent / "content").rglob("*") if File.is_file()), "private GUI source entered content")
    if Signed:
        File = Player.parent / "game.package.sig"
        Signature = File.read_bytes()
        try:
            File.write_bytes(Signature[:-1] + bytes([Signature[-1] ^ 1]))
            RD.Validate(Options.Packager, Player.parent, "altered-publisher", Root, PlayerEnv, Flags, Deny=True)
        finally:
            File.write_bytes(Signature)

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as Probe:
        Probe.bind(("127.0.0.1", 0))
        Endpoint = f"127.0.0.1:{Probe.getsockname()[1]}"
    Controller = [Player, "--connect", Endpoint, "--headless", "--max-frames", "300", *Flags]
    with ExitStack() as Processes:
        Host = Processes.enter_context(Launch([Server, "--bind", Endpoint, "--max-ticks", "1800", *ServerFlags],
                                             Root, ServerEnv, "authority"))
        Await(Host, "[Qualification:GuiServer] READY")
        if Prior:
            OldPlayer, OldFlags, OldEnv = Prior
            _, Log = RD.Run([OldPlayer, "--connect", Endpoint, "--headless", "--max-frames", "120", *OldFlags],
                            "retired-project", Root, Root, OldEnv, Deny=True, Timeout=15)
            RD.Require(b"[Qualification:GuiBoot]" not in Log, "retired project executed a GUI")
        Peers = [Processes.enter_context(Launch(Controller, Root, PlayerEnv, Name)) for Name in ("first", "second")]
        Await(Host, "[Qualification:GuiPair]", Peers)
        OwnerIds = []
        for Process, File in Peers:
            RD.Require(Process.wait(timeout=15) == 0, "owner Player exited abnormally")
            Seen = re.findall(rb"\[Runtime:Luau\] \[Qualification:GuiBoot\] OWNER_(\d+)", File.read_bytes())
            RD.Require(len(Seen) == 2 and Seen[0] == Seen[1], f"expected exactly two owner clone starts: {Seen}")
            OwnerIds.append(int(Seen[0]))
        RD.Require(len(set(OwnerIds)) == 2, "the two clients used one owner")
        Rejoin = Processes.enter_context(Launch(Controller, Root, PlayerEnv, "rejoin"))
        Await(Host, "[Qualification:GuiRejoin]", [Rejoin])
        RD.Require(Rejoin[0].wait(timeout=15) == 0, "rejoined Player exited abnormally")
        Seen = re.findall(rb"\[Runtime:Luau\] \[Qualification:GuiBoot\] OWNER_(\d+)", Rejoin[1].read_bytes())
        RD.Require(len(Seen) == 1 and int(Seen[0]) not in OwnerIds, f"reconnected owner lifetime: {Seen}")
        print("[Packaging:PlayerGui] " + json.dumps({"Project": Identity, "Signed": Signed, "Owners": OwnerIds,
              "Replacement": int(Seen[0]), "PlayerManifest": PlayerReceipt["ManifestSha256"],
              "ServerManifest": ServerReceipt["ManifestSha256"]}), flush=True)
    return Player, Flags, PlayerEnv


def Main():
    Parser = argparse.ArgumentParser()
    for Name in ("Packager", "PlayerDistribution", "ServerDistribution", "Sample"):
        Flag = re.sub(r"(?<!^)(?=[A-Z])", "-", Name).lower()
        Parser.add_argument("--" + Flag, dest=Name, type=Path, required=True)
    Parser.add_argument("--reports", type=Path)
    Options = Parser.parse_args()
    for Name in ("Packager", "PlayerDistribution", "ServerDistribution", "Sample"):
        setattr(Options, Name, getattr(Options, Name).resolve())
    with ExitStack() as Context:
        Root = Options.reports or Path(Context.enter_context(tempfile.TemporaryDirectory(prefix="GGui")))
        Root.mkdir(exist_ok=True)
        Prior = Qualify(Options, Root / "Signed", True, None)
        Qualify(Options, Root / "Development", False, Prior)


if __name__ == "__main__":
    Main()
