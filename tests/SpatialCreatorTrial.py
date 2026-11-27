# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Execute the independent documentation reader's unchanged Scripts in ordinary Engine entry points."""
import argparse
import os
from pathlib import Path
import socket
import sys
import tempfile
import time
from uuid import uuid4

from SpatialConstructionPackageSession import Build, WriteJson
from SpatialConstructionYardMultiplayer import Await, EditorHost, Launch


Scripts = Path(__file__).parent / "creator" / "spatial"


def Create(Host, Project, Names, Network=False):
    Host.Call("Handshake")
    Root = Host.Call("CreateProject", Destination=Project.as_posix(), Name="Creator Trial")["Root"]
    Snapshot = Host.Call("GetSnapshot")["Snapshot"]
    Workspace = next(Row["Id"] for Row in Snapshot["Objects"] if Row["ClassName"] == "Workspace")
    Schema = {Row["Name"]: Row for Row in Host.Call("GetSchema")["Definitions"]
              if Row["Kind"] == "Class" and Row["Namespace"] == "Engine"}
    if Network:
        Region = Schema["SpatialRegion"]
        Host.Call("CreateInstance", ClassSchemaId=Region["SchemaId"], DefinitionVersion=Region["DefinitionVersion"],
                  Parent=Workspace, Name="ReaderPackageSpatialProfile")
    Script = Schema["Script"]
    for Name in Names:
        Context = "Client" if Name.endswith(".client") else "Server"
        Host.Call("CreateInstance", ClassSchemaId=Script["SchemaId"], DefinitionVersion=Script["DefinitionVersion"],
                  Parent=Root, Name=Name.split(".")[0] + Context, InitialSource=(Scripts / (Name + ".luau")).read_text(),
                  InitialProperties=[dict(DeclaringClassSchemaId=Script["SchemaId"],
                                          DeclaringDefinitionVersion=Script["DefinitionVersion"], Property="RunContext",
                                          Value=dict(Type="EnumItem", Enum="RunContext", Value=Context))])
    Host.Call("SaveProject")


def Play(Engine, Project, Doors):
    Names = ("TransferDoor.server", "TransferView.client") if Doors else (
        "Platform.server", "OverlappingDomains.server", "ReusableBlocks.server")
    with EditorHost(Engine) as Host:
        Create(Host, Project, Names)
        Host.Call("OpenProject", Root=Project.as_posix())
        Started = Host.Call("StartPlaySession", PersistenceMode="Disabled")
        Session = Started["PlaySessionId"]
        Messages = []

        def Collect(Packet):
            for Row in Packet.get("Diagnostics", []):
                Message = Row.get("Message", "")
                Messages.append(Message)
                if Row.get("Severity") == "Error":
                    raise RuntimeError(f"Creator Script failed: {Message}")

        def Wait(Marker):
            Deadline = time.monotonic() + 12
            while not any(Marker in Message for Message in Messages):
                if time.monotonic() >= Deadline:
                    raise RuntimeError(f"Missing {Marker}. Play diagnostics: {Messages[-22:]}")
                time.sleep(.04)
                Collect(Host.Call("PollPlayDiagnostics", PlaySessionId=Session))
            print(Marker, flush=True)

        try:
            Collect(Started)
            if Doors:
                # Use the real shipped Interact action. Wait for authored prompts and controller startup.
                time.sleep(.7)
                for Confirmation in ("interior", "outside"):
                    for KeyState in ("Pressed", "Released"):
                        Host.Call("SendPlayInput", PlaySessionId=Session, Type="Key", Logical=ord("e"),
                                  Physical=8, State=KeyState)
                        time.sleep(.12)
                    Wait("[Reader:View] PASS confirmed " + Confirmation + " view")
                if sum("[Reader:Door] PASS Applied" in Message for Message in Messages) < 2:
                    raise RuntimeError("Both server admission confirmations were required")
            else:
                for Tag in ("Platform", "Reuse", "Domains"):
                    Wait(f"[Reader:{Tag}] PASS")
        finally:
            Host.Call("StopPlaySession", PlaySessionId=Session)


def Network(Args, Directory):
    Project = Directory / "Project"
    with EditorHost(Args.engine) as Host:
        Create(Host, Project, ("NetworkField.server", "NetworkField.client"), Network=True)
    Settings = Project / ".gargantuan"
    Settings.mkdir(exist_ok=True)
    WriteJson(Settings / "runtime-distribution.json", {"Version": 1, "SessionIdentity": uuid4().hex})
    WriteJson(Settings / "package-audience.json", {"Version": 1, "SharedAssets": []})
    Server, _ = Build(Args.packager, Project, Args.server_runtime, "DedicatedServer", Directory, "Development")
    Player, _ = Build(Args.packager, Project, Args.player_runtime, "DistributedPlayer", Directory, "Development")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as Available:
        Available.bind(("127.0.0.1", 0))
        Endpoint = f"127.0.0.1:{Available.getsockname()[1]}"
    Suffix = ".exe" if os.name == "nt" else ""
    Env = dict(os.environ, SDL_AUDIODRIVER="dummy")
    with Launch([Server / ("GargantuanServer" + Suffix), "--bind", Endpoint], Directory, Env, "creator-server") as Host:
        Await(Host, "[Runtime:Server] StartupWallMilliseconds=")
        with Launch([Player / ("GargantuanPlayer" + Suffix), "--headless", "--connect", Endpoint,
                     "--max-frames", "3500"], Directory, Env, "creator-player") as Client:
            Await(Client, "[Reader:NetworkReplica] PASS", (Host,))
            Await(Host, "[Reader:NetworkAuthority] PASS", (Client,))
            print("[Reader:Network] Authoritative and partial-replica assertions passed")


if __name__ == "__main__":
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--engine", type=Path, required=True)
    Mode = Parser.add_mutually_exclusive_group()
    Mode.add_argument("--doors", action="store_true")
    Mode.add_argument("--network", action="store_true")
    for Name in ("packager", "player-runtime", "server-runtime"):
        Parser.add_argument("--" + Name, type=Path)
    Args = Parser.parse_args()
    Args.engine = Args.engine.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="gct-") as Scratch:
        Directory = Path(Scratch).resolve()
        if Args.network:
            if not all((Args.packager, Args.server_runtime, Args.player_runtime)):
                Parser.error("--network requires the packager and both runtime distributions")
            Network(Args, Directory)
        else:
            Play(Args.engine, Directory / "Project", Args.doors)
