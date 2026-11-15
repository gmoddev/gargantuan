#!/usr/bin/env python3
"""Author the ClockworkWisp playable template through the public EditorHost RPC boundary."""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import queue
import secrets
import subprocess
import sys
import threading
import time
from typing import Any


SOURCE_FILES = (
    ("CharacterTemplateAssignment", "ModuleScript", "ReplicatedStorage", None, "CharacterTemplateAssignment.luau"),
    ("CharacterAnimationPolicy", "ModuleScript", "ReplicatedStorage", None, "CharacterAnimationPolicy.luau"),
    ("CharacterTemplateServer", "Script", "Workspace", "Server", "CharacterTemplateServer.server.luau"),
    ("CharacterAnimationDriver", "Script", "Workspace", "Client", "CharacterAnimationDriver.client.luau"),
    ("CharacterActionServer", "Script", "Workspace", "Server", "CharacterActionServer.server.luau"),
    ("CharacterActionClient", "Script", "Workspace", "Client", "CharacterActionClient.client.luau"),
)
REQUIRED_CAPABILITIES = {
    "OpenProject", "Schema", "Snapshot", "CreateInstance", "CreateScriptWithSource",
    "SetProperty", "SetAttribute", "SaveProject", "ReadScriptSource", "AssetCatalog",
    "AnimationRig", "CharacterComposition",
}
MAX_REQUEST_BYTES = 1024 * 1024
MAX_LINE_BYTES = 8 * 1024 * 1024 + 256
RESPONSE_PREFIX = b"GARGANTUAN_EDITOR/1 "


class AuthoringError(Exception):
    pass


def Check(Condition: bool, Message: str) -> None:
    if not Condition:
        raise AuthoringError(Message)


def Wire(Type: str, Value: Any) -> dict[str, Any]:
    return {"Type": Type, "Value": Value}


def Unique(Items: list[dict[str, Any]], Label: str) -> dict[str, Any]:
    Check(len(Items) == 1, f"{Label}: expected one match, found {len(Items)}")
    return Items[0]


def ReadLines(Stream: Any, Lines: queue.Queue[bytes | None]) -> None:
    try:
        while True:
            Line = Stream.readline(MAX_LINE_BYTES + 1)
            if not Line:
                break
            Lines.put(Line)
    finally:
        Lines.put(None)


def DrainErrors(Stream: Any) -> None:
    # Never surface host diagnostics or a session token in command output.
    while Stream.read(8192):
        pass


class EditorClient:
    def __init__(self, Binary: Path, Timeout: float):
        self.Token = secrets.token_hex(32)
        self.Sequence = 0
        self.Timeout = Timeout
        self.Lines: queue.Queue[bytes | None] = queue.Queue(maxsize=8)
        CreationFlags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
        PreviousMode = None
        if os.name == "nt":
            PreviousMode = ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002 | 0x8000)
        try:
            self.Process = subprocess.Popen(
                [str(Binary), "--editor-host", "--editor-token", self.Token, "--no_ansi"],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                creationflags=CreationFlags, bufsize=0,
            )
        finally:
            if PreviousMode is not None:
                ctypes.windll.kernel32.SetErrorMode(PreviousMode)
        threading.Thread(target=ReadLines, args=(self.Process.stdout, self.Lines), daemon=True).start()
        threading.Thread(target=DrainErrors, args=(self.Process.stderr,), daemon=True).start()

    def Close(self) -> None:
        if self.Process.poll() is None:
            self.Process.terminate()
            try:
                self.Process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.Process.kill()
                self.Process.wait(timeout=3)
        self.Token = ""

    def Call(self, Method: str, Params: dict[str, Any] | None = None) -> dict[str, Any]:
        self.Sequence += 1
        RequestId = str(self.Sequence)
        Request = {
            "Version": 1, "RequestId": RequestId, "SessionToken": self.Token,
            "Method": Method, "Params": Params or {},
        }
        Data = json.dumps(Request, ensure_ascii=False, separators=(",", ":")).encode("utf-8") + b"\n"
        Check(len(Data) <= MAX_REQUEST_BYTES, f"{Method}: request exceeds local bound")
        try:
            self.Process.stdin.write(Data)
            self.Process.stdin.flush()
        except (BrokenPipeError, OSError) as Error:
            raise AuthoringError(f"{Method}: EditorHost stopped before reply") from Error
        Deadline = time.monotonic() + self.Timeout
        for Skipped in range(33):
            try:
                Line = self.Lines.get(timeout=max(0, Deadline - time.monotonic()))
            except queue.Empty as Error:
                raise AuthoringError(f"{Method}: EditorHost reply timed out") from Error
            Check(Line is not None, f"{Method}: EditorHost closed stdout")
            Check(len(Line) <= MAX_LINE_BYTES, f"{Method}: response exceeds local bound")
            if Line.startswith(RESPONSE_PREFIX):
                Line = Line[len(RESPONSE_PREFIX):]
                break
            Check(Skipped < 32, f"{Method}: too many non-protocol output lines")
        try:
            Response = json.loads(Line)
        except (ValueError, UnicodeDecodeError) as Error:
            raise AuthoringError(f"{Method}: malformed EditorHost response") from Error
        Check(isinstance(Response, dict) and Response.get("Version") == 1 and
              Response.get("RequestId") == RequestId and isinstance(Response.get("Ok"), bool),
              f"{Method}: mismatched EditorHost response")
        if not Response["Ok"]:
            Error = Response.get("Error", {})
            Code = Error.get("Code", "Unknown") if isinstance(Error, dict) else "Unknown"
            Check(isinstance(Code, str) and len(Code) <= 64, f"{Method}: malformed error code")
            raise AuthoringError(f"{Method}: EditorHost rejected request ({Code})")
        Result = Response.get("Result")
        Check(isinstance(Result, dict), f"{Method}: missing result object")
        return Result


def Definition(Schema: dict[str, Any], Name: str, Construct: bool = True) -> dict[str, Any]:
    Entries = [Value for Value in Schema.get("Definitions", [])
               if Value.get("CanonicalName") == f"Engine.{Name}" and Value.get("Kind") == "Class"]
    Result = Unique(Entries, f"schema Engine.{Name}")
    Check(Result.get("ConstructionKind") == "Native" and
          (not Construct or Result.get("Constructible") is True),
          f"Engine.{Name}: unavailable for public construction")
    Check(isinstance(Result.get("SchemaId"), str) and type(Result.get("DefinitionVersion")) is int,
          f"Engine.{Name}: invalid schema identity")
    return Result


def FindObject(Snapshot: dict[str, Any], Name: str, ClassName: str) -> dict[str, Any]:
    return Unique([Object for Object in Snapshot.get("Objects", [])
                   if Object.get("Name") == Name and Object.get("ClassName") == ClassName],
                  f"{ClassName} {Name}")


def FindAssets(Catalog: dict[str, Any], WispSource: str, CharmSource: str) -> tuple[str, str, dict[str, str], list[str]]:
    Check(Catalog.get("CatalogVersion") == 2, "asset catalog version differs from expected 2")
    Assets = Catalog.get("Assets", [])
    Check(isinstance(Assets, list), "asset catalog has no Assets list")
    def SourceMatch(Asset: dict[str, Any], Source: str) -> bool:
        return str(Asset.get("Source", "")).replace("\\", "/").split("/")[-1] == Source
    Wisp = Unique([Asset for Asset in Assets if Asset.get("Kind") == "Mesh" and
                   SourceMatch(Asset, WispSource) and Asset.get("State") == "Ready" and
                   Asset.get("Available") is True and
                   Asset.get("Metadata", {}).get("JointCount") == 3], "skinned ClockworkWisp Mesh")
    Charm = Unique([Asset for Asset in Assets if Asset.get("Kind") == "Mesh" and
                    SourceMatch(Asset, CharmSource) and Asset.get("State") == "Ready" and
                    Asset.get("Available") is True],
                   "ClockworkCharm Mesh")
    Check(isinstance(Wisp.get("Reference"), str) and Wisp["Reference"].startswith("asset://") and
          isinstance(Charm.get("Reference"), str) and Charm["Reference"].startswith("asset://"),
          "catalog records lack strict references")
    Clips: dict[str, str] = {}
    for Name in ("WispIdle", "WispMove", "WispAction"):
        Clip = Unique([Asset for Asset in Assets if Asset.get("Kind") == "Animation" and
                       SourceMatch(Asset, WispSource) and Asset.get("Name") == Name and
                       Asset.get("State") == "Ready" and
                       Asset.get("Available") is True and
                       Asset.get("Metadata", {}).get("SkeletonCompatibilityId") ==
                       Wisp.get("Metadata", {}).get("SkeletonCompatibilityId")], Name)
        Check(isinstance(Clip.get("Reference"), str) and Clip["Reference"].startswith("asset://"),
              f"{Name}: catalog record lacks strict reference")
        Clips[Name] = Clip["Reference"]
    return Wisp["Reference"], Charm["Reference"], Clips, [Wisp["AssetId"], Charm["AssetId"]]


def SourceText(Repository: Path) -> dict[str, str]:
    Result: dict[str, str] = {}
    for _, _, _, _, Filename in SOURCE_FILES:
        PathValue = Repository / "docs" / "examples" / Filename
        Check(PathValue.is_file(), f"missing source example {Filename}")
        Text = PathValue.read_text(encoding="utf-8").replace("\r\n", "\n").replace("\r", "\n")
        Check(0 < len(Text.encode("utf-8")) < 1024 * 1024, f"{Filename}: source size out of bound")
        Result[Filename] = Text
    return Result


def Author(Client: EditorClient, Project: Path, Sources: dict[str, str],
           WispSource: str, CharmSource: str) -> dict[str, Any]:
    Handshake = Client.Call("Handshake")
    Check(Handshake.get("Engine") == "Gargantuan" and Handshake.get("ProtocolVersion") == 1,
          "EditorHost protocol mismatch")
    Check(Handshake.get("CharacterCompositionVersion") == 1 and
          REQUIRED_CAPABILITIES.issubset(set(Handshake.get("Capabilities", []))),
          "EditorHost lacks required creator authoring capabilities")
    Check({"ReadDataModel", "MutateDataModel", "EditorCommands"}.issubset(
          set(Handshake.get("StudioCapabilities", []))), "EditorHost lacks authoring authority")
    Client.Call("OpenProject", {"Root": str(Project)})
    Schema = Client.Call("GetSchema", {"SchemaDiscoveryVersion": 7})
    Names = ("KinematicCharacter", "Character", "MeshPart", "BasePart", "Animator",
             "Attachment", "AttachmentMesh", "Script", "ModuleScript", "Players")
    Definitions = {Name: Definition(Schema, Name, Name != "Players") for Name in Names}
    Snapshot = Client.Call("GetSnapshot")["Snapshot"]
    Scope = Snapshot.get("Cursor", {}).get("Scope")
    Check(Scope is not None, "snapshot lacks project scope")
    Workspace = FindObject(Snapshot, "Workspace", "Workspace")["Id"]
    Storage = FindObject(Snapshot, "ReplicatedStorage", "ReplicatedStorage")["Id"]
    Players = FindObject(Snapshot, "Players", "Players")["Id"]
    Reserved = {"PlayableCharacterTemplate", "SkinnedRoot", "RigAnimator", "CrownSocket", "WispCharm"}
    Reserved.update(Name for Name, _, _, _, _ in SOURCE_FILES)
    Check(not any(Object.get("Name") in Reserved for Object in Snapshot.get("Objects", [])),
          "an authored object name already exists; use a fresh project or review it in Studio")

    MeshReference, CharmReference, Clips, AssetIds = FindAssets(
        Client.Call("GetAssetCatalog", {"IncludeBuiltIns": False}), WispSource, CharmSource)
    Rig = Client.Call("GetAnimationRig", {"Reference": MeshReference})
    JointPaths = {Joint.get("Path") for Joint in Rig.get("Joints", [])}
    Check({"WispRoot", "WispRoot/WispCrown", "WispRoot/WispCrown/WispAntenna"}.issubset(JointPaths),
          "Wisp rig does not expose the expected canonical joints")
    RigClips = {Clip.get("Reference") for Clip in Rig.get("Clips", [])}
    Check(set(Clips.values()).issubset(RigClips), "Wisp clips are not compatible with inspected rig")

    def Create(ClassName: str, Parent: Any, Name: str, InitialSource: str | None = None) -> Any:
        Def = Definitions[ClassName]
        Params = {"ClassSchemaId": Def["SchemaId"], "DefinitionVersion": Def["DefinitionVersion"],
                  "Parent": Parent, "Name": Name}
        if InitialSource is not None:
            Params["InitialSource"] = InitialSource
        return Client.Call("CreateInstance", Params)["Object"]

    def SetProperty(Object: Any, Concrete: str, Declaring: str, Property: str,
                    Value: dict[str, Any]) -> None:
        Exact = Definitions[Concrete]
        Owner = Definitions[Declaring]
        Check(any(Item.get("Name") == Property and Item.get("Writable") is True and
                  Item.get("Persistence") == "Saved" for Item in Owner.get("Properties", [])),
              f"{Declaring}.{Property} is not a saved writable property")
        Client.Call("SetProperty", {"Object": Object, "ClassSchemaId": Exact["SchemaId"],
            "ClassDefinitionVersion": Exact["DefinitionVersion"],
            "DeclaringClassSchemaId": Owner["SchemaId"],
            "DeclaringDefinitionVersion": Owner["DefinitionVersion"],
            "Property": Property, "Value": Value})

    def SetAttribute(Object: Any, Name: str, Value: str) -> None:
        Client.Call("SetAttribute", {"Object": Object, "Attribute": Name,
                                     "Value": Wire("String", Value)})

    Template = Create("KinematicCharacter", Storage, "PlayableCharacterTemplate")
    Root = Create("MeshPart", Template, "SkinnedRoot")
    Animator = Create("Animator", Root, "RigAnimator")
    Socket = Create("Attachment", Root, "CrownSocket")
    Accessory = Create("AttachmentMesh", Root, "WispCharm")
    SetProperty(Root, "MeshPart", "MeshPart", "Mesh", Wire("String", MeshReference))
    for Property, Value in (("Anchored", True), ("CanCollide", False), ("CanTouch", False)):
        SetProperty(Root, "MeshPart", "BasePart", Property, Wire("Bool", Value))
    SetProperty(Socket, "Attachment", "Attachment", "JointPath",
                Wire("String", "WispRoot/WispCrown"))
    SetProperty(Accessory, "AttachmentMesh", "AttachmentMesh", "Mesh", Wire("String", CharmReference))
    SetProperty(Accessory, "AttachmentMesh", "Attachment", "JointPath",
                Wire("String", "WispRoot/WispCrown/WispAntenna"))
    SetProperty(Template, "KinematicCharacter", "Character", "RootPart",
                Wire("ObjectReference", Root))

    CreatedSources: dict[str, Any] = {}
    SourceObjects: dict[str, Any] = {}
    for Name, ClassName, ParentName, Context, Filename in SOURCE_FILES:
        Parent = Storage if ParentName == "ReplicatedStorage" else Workspace
        Object = Create(ClassName, Parent, Name, Sources[Filename])
        CreatedSources[Name] = Object
        SourceObjects[Name] = {"Object": Object, "ClassName": ClassName,
                               "Parent": ParentName, "RunContext": Context,
                               "File": Filename}
        if Context is not None:
            SetProperty(Object, "Script", "Script", "RunContext",
                        {"Type": "EnumItem", "Enum": "RunContext", "Value": Context})
    Driver = CreatedSources["CharacterAnimationDriver"]
    for Attribute, Clip in (("IdleReference", Clips["WispIdle"]),
                            ("MoveReference", Clips["WispMove"]),
                            ("ActionReference", Clips["WispAction"])):
        SetAttribute(Driver, Attribute, Clip)
    for Name in ("CharacterActionServer", "CharacterActionClient"):
        SetAttribute(CreatedSources[Name], "AnimationReference", Clips["WispAction"])
    for Property in ("DefaultControllerEnabled", "DefaultCameraEnabled", "CharacterAutoLoads"):
        SetProperty(Players, "Players", "Players", Property, Wire("Bool", True))

    Inspection = Client.Call("GetCharacterComposition", {
        "CharacterCompositionVersion": 1, "Scope": Scope, "Object": Template})
    Check(Inspection.get("Context") == "Authoring" and Inspection.get("Scope") == Scope and
          Inspection.get("Character") == Template and Inspection.get("RootPart") == Root and
          Inspection.get("Complete") is True and Inspection.get("Valid") is True,
          "authored Character composition is incomplete or invalid")
    Meshes = Inspection.get("Meshes", [])
    Check(any(Mesh.get("Mesh", {}).get("Reference") == MeshReference and
              Mesh.get("AnimatorCount") == 1 and Mesh.get("IsRootPart") is True for Mesh in Meshes),
          "composition does not bind the skinned root and Animator")
    Check(any(Item.get("JointPath") == "WispRoot/WispCrown" and
              Item.get("JointPathExists") is True for Item in Inspection.get("Attachments", [])),
          "composition does not resolve the Crown socket")
    for Name, _, _, _, Filename in SOURCE_FILES:
        Readback = Client.Call("GetScriptSource", {"Object": CreatedSources[Name]})
        Check(Readback.get("Source") == Sources[Filename] and
              type(Readback.get("SourceVersion")) is int and Readback["SourceVersion"] > 0,
              f"{Name}: source readback differs from authored example")
    FinalSnapshot = Client.Call("GetSnapshot")["Snapshot"]
    Objects = FinalSnapshot.get("Objects", [])
    by_id = {json.dumps(Item.get("Id"), sort_keys=True): Item for Item in Objects}
    def VerifyParent(Object: Any, Parent: Any, Name: str) -> None:
        Item = by_id.get(json.dumps(Object, sort_keys=True))
        Check(Item is not None and Item.get("Parent") == Parent and Item.get("Name") == Name,
              f"{Name}: hierarchy readback differs")
    VerifyParent(Template, Storage, "PlayableCharacterTemplate")
    VerifyParent(Root, Template, "SkinnedRoot")
    VerifyParent(Animator, Root, "RigAnimator")
    VerifyParent(Socket, Root, "CrownSocket")
    VerifyParent(Accessory, Root, "WispCharm")
    for Name, _, ParentName, _, _ in SOURCE_FILES:
        VerifyParent(CreatedSources[Name], Storage if ParentName == "ReplicatedStorage" else Workspace, Name)
    FinalRevision = Client.Call("GetProjectState")["AuthoritativeRevision"]
    Saved = Client.Call("SaveProject", {"ExpectedRevision": FinalRevision})
    Check(Saved.get("PersistedRevision") == FinalRevision, "saved revision differs from verified revision")
    return {
        "Status": "AuthoredAndSaved", "Scope": Scope, "PersistedRevision": FinalRevision,
        "Template": Template, "RootPart": Root, "Animator": Animator,
        "Socket": Socket, "Accessory": Accessory, "Sources": SourceObjects,
        "MeshReference": MeshReference, "CharmReference": CharmReference,
        "ClipReferences": Clips, "AssetIds": AssetIds,
        "CompositionComplete": Inspection["Complete"], "CompositionValid": Inspection["Valid"],
    }


def Main() -> int:
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--binary", required=True, type=Path, help="qualified gargantuan binary with EditorHost")
    Parser.add_argument("--project", required=True, type=Path, help="closed Studio project directory")
    Parser.add_argument("--receipt", required=True, type=Path, help="new receipt JSON outside the project")
    Parser.add_argument("--wisp-source", default="ClockworkWisp.gltf", help="catalog Source basename")
    Parser.add_argument("--charm-source", default="ClockworkCharm.obj", help="catalog Source basename")
    Parser.add_argument("--rpc-timeout", type=float, default=30.0, help="per request timeout, seconds")
    Args = Parser.parse_args()
    try:
        Binary = Args.binary.resolve(strict=True)
        Project = Args.project.resolve(strict=True)
        Receipt = Args.receipt.resolve(strict=False)
        Check(Binary.is_file() and Project.is_dir(), "binary must be a file and project a directory")
        Check(Receipt != Project and Project not in Receipt.parents, "receipt must be outside project")
        Check(not Receipt.exists() and Receipt.parent.is_dir(), "receipt must be a new file in an existing directory")
        Check(1 <= Args.rpc_timeout <= 120, "RPC timeout must be 1 to 120 seconds")
        Check(all("/" not in Value and "\\" not in Value and Value for Value in
                  (Args.wisp_source, Args.charm_source)), "source selectors must be basenames")
        Repository = Path(__file__).resolve().parents[2]
        Sources = SourceText(Repository)
        Client = EditorClient(Binary, Args.rpc_timeout)
        try:
            Result = Author(Client, Project, Sources, Args.wisp_source, Args.charm_source)
        finally:
            Client.Close()
        Result["Project"] = str(Project)
        Result["Binary"] = str(Binary)
        Result["SourceSha256"] = {
            Filename: hashlib.sha256(Text.encode("utf-8")).hexdigest()
            for Filename, Text in Sources.items()
        }
        Result["SourceFileSha256"] = {
            Filename: hashlib.sha256((Repository / "docs" / "examples" / Filename).read_bytes()).hexdigest()
            for Filename in Sources
        }
        with Receipt.open("x", encoding="utf-8") as Stream:
            json.dump(Result, Stream, indent=2, sort_keys=True)
            Stream.write("\n")
        print(f"[Creator:AuthorProject] Saved revision {Result['PersistedRevision']}; receipt: {Receipt}")
        return 0
    except (AuthoringError, OSError, ValueError, KeyError, TypeError) as Error:
        # Error text never contains the session token, source bodies, or host stderr.
        print(f"[Creator:AuthorProject] {str(Error)[:256]}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(Main())
