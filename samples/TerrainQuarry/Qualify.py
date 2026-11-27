#!/usr/bin/env python3
"""Package the quarry and replay the same route on native loopback clients."""

import argparse
from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import time

from BuildProject import BuildProject


Flags = getattr(subprocess, "CREATE_NO_WINDOW", 0) | getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0)
Environment = dict(os.environ, SDL_AUDIODRIVER="dummy", SDL_LOGGING="*=info")


def Executable(Root, Name):
    return Root / (Name + (".exe" if os.name == "nt" else ""))


def Digest(PathValue):
    return hashlib.sha256(PathValue.read_bytes()).hexdigest()


def RunPackage(Build, Project, Root, Role, Distribution):
    Output = Root / Role
    with (Root / f"package-{Role}.log").open("wb") as Log:
        subprocess.run(
            [Executable(Build, "gargantuan-packager"), "build", "--project", Project, "--output", Output,
             "--runtime", Build / Distribution, "--configuration", "Development"],
            cwd=Build, env=Environment, creationflags=Flags, stdout=Log, stderr=Log, timeout=45, check=True,
        )
    return Output


def Start(Stack, Program, Log, *Arguments):
    Output = Stack.enter_context(Log.open("wb"))
    Process = subprocess.Popen([Program, *Arguments], cwd=Program.parent, env=Environment,
                               stdout=Output, stderr=Output, creationflags=Flags)

    def Close():
        if Process.poll() is None:
            Process.terminate()
            try:
                Process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                Process.kill()
                Process.wait(timeout=5)

    Stack.callback(Close)
    return Process


def Wait(Process, Log, Pattern, Limit=28):
    Deadline = time.monotonic() + Limit
    while time.monotonic() < Deadline:
        Output = Log.read_text(encoding="utf-8", errors="replace")
        Match = re.search(Pattern, Output)
        if Match is not None:
            return Match
        if Process.poll() is not None or "route failed player=" in Output or "course halted" in Output:
            break
        time.sleep(0.05)
    raise RuntimeError(f"Missing {Pattern} in {Log}\n{Output[-3500:]}")


def Route(Stack, Player, Root, Label, Address):
    Log = Root / f"{Label}.log"
    Started = time.monotonic()
    Process = Start(Stack, Player, Log, "--headless", "--connect", Address, "--max-frames", "900")
    return Process, Log, Started


def ReadRoute(Active):
    Process, Log, Started = Active
    Match = Wait(Process, Log, r"\[Terrain:Quarry\] route complete player=(\d+)")
    Output = Log.read_text(encoding="utf-8", errors="replace")
    Player = int(Match[1])
    Phases = [(int(Step), int(Material)) for Step, Material in re.findall(
        rf"\[Terrain:Quarry\] client phase=(\d+) server_material=(\d+) collision=ready player={Player}\b", Output)]
    if Phases != [(1, 0), (2, 4), (3, 5)] or "Client released movement after the server confirmed admitted collision" not in Output:
        raise RuntimeError(f"Incomplete native collision receipts in {Log}")
    return {"player": Player, "phases": Phases, "observed_seconds": round(time.monotonic() - Started, 3)}


def RunSession(Server, Player, Root, Durable, Replay=False):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as Reservation:
        Reservation.bind(("127.0.0.1", 0))
        Address = f"127.0.0.1:{Reservation.getsockname()[1]}"
    Label = "replay" if Replay else "initial"
    Log = Root / f"server-{Label}.log"
    Provider = ["--persistence-provider", "sqlite", "--persistence-local-db", Root / "quarry.sqlite"] if Durable else []
    with ExitStack() as Stack:
        Session = Start(Stack, Server, Log, "--bind", Address, "--max-ticks", "1800", *Provider)
        InitialPhase = 3 if Replay else 0
        Persistence = "durable" if Durable else "ephemeral"
        Wait(Session, Log, rf"\[Terrain:Quarry\] ready phase={InitialPhase} persistence={Persistence}", 12)
        First = Route(Stack, Player, Root, f"player-{Label}-one", Address)
        if Replay:
            Records = {"restored": ReadRoute(First)}
        else:
            Second = Route(Stack, Player, Root, "player-initial-two", Address)
            Records = {"first": ReadRoute(First), "second": ReadRoute(Second)}
            if First[0].wait(timeout=20) != 0:
                raise RuntimeError(f"First player did not disconnect cleanly. See {First[1]}")
            Records["reconnected"] = ReadRoute(Route(Stack, Player, Root, "player-reconnected", Address))
            if len({Record["player"] for Record in Records.values()}) != 3:
                raise RuntimeError("The two players and reconnected session did not have distinct identities")
        Output = Log.read_text(encoding="utf-8", errors="replace")
        Tokens = re.findall(r"\[Terrain:Quarry\] server phase=(\d+) density=([-\d]+) material=(\d+) "
                            r"ray=([.\d]+) revision=(\d+) replay=(true|false)", Output)
        if [(int(Step), int(Material), WasReplay) for Step, _, Material, _, _, WasReplay in Tokens] != [
            (1, 0, str(Replay).lower()), (2, 4, str(Replay).lower()), (3, 5, str(Replay).lower())
        ]:
            raise RuntimeError(f"Server did not commit or restore each operation exactly once. See {Log}")
        if Durable:
            if Session.wait(timeout=35) != 0:
                raise RuntimeError(f"Dedicated server did not shut down cleanly. See {Log}")
        return {"clients": Records, "server_samples": Tokens}


if __name__ == "__main__":
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--build", type=Path, required=True, help="Matching Engine, packager and runtime builds")
    Parser.add_argument("--output", type=Path, required=True, help="New evidence directory")
    Parser.add_argument("--sqlite", action="store_true", help="Also replay durable edits after a clean server restart")
    Args = Parser.parse_args()
    Build = Args.build.resolve(strict=True)
    Root = Args.output.resolve()
    Root.mkdir()
    Project = Root / "Project"
    BuildProject(Executable(Build, "gargantuan"), Project, True)
    Scene = Project / ".gargantuan/project.instance.json"
    SceneDigest = Digest(Scene)
    Server = Executable(RunPackage(Build, Project, Root, "Server", "ServerRuntimeDistribution"), "GargantuanServer")
    Player = Executable(RunPackage(Build, Project, Root, "Player", "RuntimeDistribution"), "GargantuanPlayer")
    Result = {"server_sha256": Digest(Server), "player_sha256": Digest(Player),
              "terrain_manifest": json.loads(Scene.read_text(encoding="utf-8"))["TerrainManifest"],
              "session": RunSession(Server, Player, Root, Args.sqlite)}
    if Args.sqlite:
        Result["restarted"] = RunSession(Server, Player, Root, True, Replay=True)
    if SceneDigest != Digest(Scene):
        raise RuntimeError("Gameplay unexpectedly modified the authored scene")
    (Root / "receipt.json").write_text(json.dumps(Result, indent=2) + "\n", encoding="utf-8")
    print(f"[Terrain:Quarry] Qualified native packages in {Root}")
