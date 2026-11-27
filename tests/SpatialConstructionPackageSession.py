# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Build, relocate and execute a managed spatial Server/Player package pair."""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
from uuid import uuid4


def WriteJson(PathValue, Value):
    PathValue.write_text(json.dumps(Value), encoding="utf-8")


def Run(Arguments, Directory, Environment=None, Expected=0):
    Process = subprocess.run(
        [str(Argument) for Argument in Arguments], cwd=Directory, env=Environment,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=45,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
    )
    Output = Process.stdout.decode(errors="replace")
    if Process.returncode != Expected:
        raise RuntimeError(f"{Path(Arguments[0]).name} exited {Process.returncode}\n{Output[-4000:]}")
    return Output


def Build(Packager, Project, Distribution, Target, Directory):
    Request = Directory / f"{Target}.json"
    WriteJson(Request, {"Version": 1, "Target": Target, "Preset": "Open",
                        "PublisherTrust": "DigestOnly", "ModLoading": "Disabled"})
    Staged = Directory / f"staging-{Target}"
    Run([Packager, "build", "--project", Project, "--output", Staged,
         "--runtime", Distribution, "--configuration", "Release", "--distribution", Request], Directory)
    Moved = Directory / Target
    Staged.rename(Moved)
    Run([Packager, "validate", Moved], Directory)
    Manifest = json.loads((Moved / "game.package.json").read_text(encoding="utf-8"))
    Metadata = Manifest["RuntimeDistribution"]
    if Metadata["Target"] != Target or Metadata.get("SpatialContent") != "BlockGrid1":
        raise RuntimeError(f"{Target} package is missing its negotiated spatial profile")
    return Moved, Metadata["ClientCodeIdentity"]


def VerifyPair(Fixture, Packager, PlayerRuntime, ServerRuntime, Directory):
    Project = Directory / "Project"
    Run([Fixture, "--write-session-project", Project], Directory)
    Settings = Project / ".gargantuan"
    WriteJson(Settings / "runtime-distribution.json", {"Version": 1, "SessionIdentity": uuid4().hex})
    WriteJson(Settings / "package-audience.json", {"Version": 1, "SharedAssets": []})
    Server, ServerIdentity = Build(Packager, Project, ServerRuntime, "DedicatedServer", Directory)
    Player, PlayerIdentity = Build(Packager, Project, PlayerRuntime, "DistributedPlayer", Directory)
    if ServerIdentity != PlayerIdentity:
        raise RuntimeError("The package pair disagrees on its client code identity")

    Private = b"project:private_sealed_package_tile"
    def ShipsPrivateMarker(Package):
        return any(Private in Entry.read_bytes() for Entry in (Package / "content").rglob("*") if Entry.is_file())
    if not ShipsPrivateMarker(Server) or ShipsPrivateMarker(Player):
        raise RuntimeError("The server-only authored tile marker crossed the package audience boundary")

    Environment = os.environ.copy()
    if os.name == "nt":
        Windows = Path(os.environ["SystemRoot"])
        Environment["PATH"] = os.pathsep.join([str(Windows / "System32"), str(Windows)])
    Environment["SDL_AUDIODRIVER"] = "dummy"
    Suffix = ".exe" if os.name == "nt" else ""
    ServerExe, PlayerExe = Server / ("GargantuanServer" + Suffix), Player / ("GargantuanPlayer" + Suffix)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as Available:
        Available.bind(("127.0.0.1", 0))
        Endpoint = f"127.0.0.1:{Available.getsockname()[1]}"
    LogFile = Directory / "server.log"
    with LogFile.open("wb") as Log:
        Process = subprocess.Popen(
            [str(ServerExe), "--bind", Endpoint, "--max-ticks", "360"], cwd=Directory,
            env=Environment, stdout=Log, stderr=subprocess.STDOUT,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        try:
            Deadline = time.monotonic() + 15
            while b"[Runtime:Server] StartupWallMilliseconds=" not in LogFile.read_bytes():
                if Process.poll() is not None or time.monotonic() >= Deadline:
                    raise RuntimeError("Packaged Server did not begin serving\n" + LogFile.read_text(errors="replace")[-2500:])
                time.sleep(0.025)
            ClientOutput = Run([PlayerExe, "--headless", "--connect", Endpoint, "--max-frames", "165"], Directory, Environment)
            if "[Spatial:PackageObserver] Public grid, channel, ray and authoritative edit verified" not in ClientOutput:
                raise RuntimeError("Packaged client did not observe the edited field\n" + ClientOutput[-2200:]
                                   + "\n" + LogFile.read_text(errors="replace")[-2200:])
            if Process.wait(timeout=20) != 0:
                raise RuntimeError("Packaged Server failed\n" + LogFile.read_text(errors="replace")[-2400:])
        finally:
            if Process.poll() is None:
                Process.terminate()
                try:
                    Process.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    Process.kill()
                    Process.wait(timeout=4)
    ServerOutput = LogFile.read_text(errors="replace")
    if "[Spatial:PackageAuthority] Client acknowledged the edited grid" not in ServerOutput:
        raise RuntimeError("Packaged Server did not receive the client's final acknowledgment\n" + ServerOutput[-2000:])
    print("[Spatial:PackageSession] Relocated managed Server and distributed Player verified the authorized grid,"
          " private state exclusion, live edit and client acknowledgment over GNS")


def Main():
    Args = argparse.ArgumentParser()
    for Name in ("fixture", "packager", "player-runtime", "server-runtime"):
        Args.add_argument("--" + Name, type=Path, required=True)
    Options = Args.parse_args()
    with tempfile.TemporaryDirectory(prefix="spg-") as Scratch:
        VerifyPair(Options.fixture.resolve(), Options.packager.resolve(), Options.player_runtime.resolve(),
                   Options.server_runtime.resolve(), Path(Scratch))


if __name__ == "__main__":
    Main()
