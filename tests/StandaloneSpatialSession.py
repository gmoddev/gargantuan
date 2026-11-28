# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Run the spatial package workload through an authored multiplayer title."""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
from uuid import uuid4

from RuntimeDistributionQualification import CodeNode, Node
from SpatialConstructionPackageSession import Build, ConsoleCapture, ManagedEnvironment, Run, WriteJson


Title = """
local Menu = Instance.new("ScreenGui")
Menu.Parent = script.Parent
assert(game:SetMainMenu(Menu).Ok)
local Sessions = game:GetService("SessionService")
task.defer(function()
    local Joined = Sessions:Join({Kind = "Direct", Address = "ENDPOINT"}):Await()
    assert(Joined.Ok and Joined.Value.Mode == "Remote", Joined.Code)
    assert(game:SetGameplayInputEnabled(false).Ok)
    assert(game:GetService("RunService"):PauseGameplay():Await().Code == "NotAuthorized")
    task.wait(3)
    assert(game:IsMainMenuVisible() and Sessions:GetSession().World == "Ready")
    assert(Sessions:Leave():Await().Ok)
    assert(Sessions:GetSession().World == "Absent")
    print("[Lifecycle:Spatial] Left remote world from the overlay")
    assert(game:GetService("ApplicationService"):Quit().Ok)
end)
"""


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    for Name in ("fixture", "packager", "player-runtime", "server-runtime"):
        Parser.add_argument("--" + Name, type=lambda Value: Path(Value).resolve(strict=True), required=True)
    Options = Parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="glsp-") as Scratch:
        Root = Path(Scratch)
        Project = Root / "Project"
        Run([Options.fixture, "--write-terrain-session-project", Project], Root)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as Available:
            Available.bind(("127.0.0.1", 0))
            Endpoint = f"127.0.0.1:{Available.getsockname()[1]}"
        Settings = Project / ".gargantuan"
        SceneFile = Settings / "project.instance.json"
        Scene = json.loads(SceneFile.read_bytes())
        Entry = CodeNode("SpatialTitleEntry", "Script", Title.replace("ENDPOINT", Endpoint), "Client")
        Entry["Properties"]["Enabled"] = {"Bool": True}
        def Walk(Item):
            yield Item
            for Child in Item["Children"]:
                yield from Walk(Child)
        NextId = max(Item.get("NodeId", 0) for Item in Walk(Scene)) + 1
        Menu = Node("Application", "Folder", Children=[Entry])
        Menu["NodeId"], Entry["NodeId"] = NextId, NextId + 1
        Scene["Children"].append(Menu)
        Scene["Properties"]["ApplicationEntry"] = Entry["Properties"]["SourceId"]
        WriteJson(SceneFile, Scene)
        WriteJson(Settings / "runtime-distribution.json", {"Version": 1, "SessionIdentity": uuid4().hex})
        WriteJson(Settings / "package-audience.json", {"Version": 1, "SharedAssets": []})
        Server, Host = Build(Options.packager, Project, Options.server_runtime, "DedicatedServer", Root, "Development")
        Player, Guest = Build(Options.packager, Project, Options.player_runtime, "DistributedPlayer", Root, "Development")
        assert Host["ClientCodeIdentity"] == Guest["ClientCodeIdentity"] and Host["WorkspaceTerrain"] == Guest["WorkspaceTerrain"]
        assert any((Server / "content").rglob("*.gtch")) and not any((Player / "content").rglob("*.gtch"))
        Suffix = ".exe" if os.name == "nt" else ""
        Environment = ManagedEnvironment()
        Log = Root / "server.log"
        with Log.open("wb") as Stream, subprocess.Popen([Server / ("GargantuanServer" + Suffix),
                "--bind", Endpoint, "--max-ticks", "540"], cwd=Root, env=Environment, stdout=Stream,
                stderr=subprocess.STDOUT, **ConsoleCapture()) as Process:
            try:
                Deadline = time.monotonic() + 10
                while b"[Runtime:Server] StartupWallMilliseconds=" not in Log.read_bytes():
                    if Process.poll() is not None or time.monotonic() > Deadline:
                        raise RuntimeError(Log.read_text(errors="replace")[-2500:])
                    time.sleep(0.025)
                Output = Run([Player / ("GargantuanPlayer" + Suffix), "--headless", "--profile-root", Root,
                              "--max-frames", "450"], Root, Environment)
                for Marker in ("Public grid, channel, ray and authoritative edit verified",
                               "Streamed Workspace terrain collision verified", "Left remote world from the overlay"):
                    if Marker not in Output:
                        raise RuntimeError(Marker + "\n" + Output[-3500:])
                assert Process.wait(timeout=15) == 0
                assert "Client acknowledged the edited grid" in Log.read_text(errors="replace")
            finally:
                if Process.poll() is None:
                    Process.kill()
                    Process.wait(timeout=5)
        print("[Lifecycle:Spatial] Verified the packaged spatial edit and terrain collision behind a responsive multiplayer title")


if __name__ == "__main__":
    Main()
