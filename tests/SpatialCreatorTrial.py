# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Run the public-reader Scripts and optional creator library through ordinary Engine entry points."""
import argparse
import os
from pathlib import Path
import socket
import sys
import tempfile
import time
from uuid import uuid4

from SpatialConstructionPackageSession import Build, ManagedEnvironment, WriteJson
from SpatialConstructionYardMultiplayer import Await, EditorHost, Launch


Scripts = Path(__file__).parent / "creator" / "spatial"


def Create(Host, Project, Sources, Network=False):
    Host.Call("Handshake")
    Root = Host.Call("CreateProject", Destination=Project.as_posix(), Name="Creator Trial")["Root"]
    Snapshot = Host.Call("GetSnapshot")["Snapshot"]
    Workspace = next(Row["Id"] for Row in Snapshot["Objects"] if Row["ClassName"] == "Workspace")
    Schema = {Row["Name"]: Row for Row in Host.Call("GetSchema")["Definitions"]
              if Row["Kind"] == "Class" and Row["Namespace"] == "Engine"}
    if Network:
        Grid = Schema["BlockGrid"]
        Floor = Host.Call("CreateInstance", ClassSchemaId=Grid["SchemaId"], DefinitionVersion=Grid["DefinitionVersion"],
                          Parent=Workspace, Name="StationarySpawnFloor")["Object"]
        Address = dict(Scope=Snapshot["Cursor"]["Scope"], Object=Floor, Cell=[-6, -1, -6])
        Revision = Host.Call("ReadBlockGridCell", **Address)["Revision"]
        Host.Call("EditBlockGridRegion", **Address, Revision=Revision, MaxExclusive=[7, 0, 7], Definition="Stone")
    for File in Sources:
        Context = {".client": "Client", ".server": "Server"}.get(Path(File.stem).suffix)
        Definition = Schema["Script" if Context else "ModuleScript"]
        Properties = []
        if Context:
            Properties.append(dict(DeclaringClassSchemaId=Definition["SchemaId"],
                                   DeclaringDefinitionVersion=Definition["DefinitionVersion"], Property="RunContext",
                                   Value=dict(Type="EnumItem", Enum="RunContext", Value=Context)))
        Host.Call("CreateInstance", ClassSchemaId=Definition["SchemaId"], DefinitionVersion=Definition["DefinitionVersion"],
                  Parent=Root, Name=File.name.split(".")[0] + (Context or ""), InitialSource=File.read_text(encoding="utf-8"),
                  InitialProperties=Properties)
    Host.Call("SaveProject")


def Play(Engine, Project, Doors, Masonry=False):
    Names = ("TransferDoor.server", "TransferView.client") if Doors else (
        "Platform.server", "OverlappingDomains.server", "ReusableBlocks.server", "JointDomains.server")
    Sources = [Scripts / (Name + ".luau") for Name in Names]
    if Masonry:
        Example = Scripts.parents[2] / "samples" / "BlockGridGameplay"
        Sources = [Example / "Masonry.luau", Example / "MasonryExample.server.luau"]
    with EditorHost(Engine) as Host:
        Create(Host, Project, Sources)
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
            print(next(Message for Message in Messages if Marker in Message), flush=True)

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
            elif Masonry:
                Wait("[BlockGrid:Masonry] Built the workshop")
            else:
                for Tag in ("Platform", "Reuse", "Domains", "Joints"):
                    Wait(f"[Reader:{Tag}] PASS")
        finally:
            Host.Call("StopPlaySession", PlaySessionId=Session)


def Network(Args, Directory):
    Project = Directory / "Project"
    with EditorHost(Args.engine) as Host:
        Create(Host, Project, [Scripts / "NetworkField.server.luau", Scripts / "NetworkField.client.luau"], Network=True)
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
    Env = ManagedEnvironment()
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
    Mode.add_argument("--masonry", action="store_true")
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
            Play(Args.engine, Directory / "Project", Args.doors, Args.masonry)
