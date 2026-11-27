#!/usr/bin/env python3
"""Build an editable Terrain Quarry project with the local EditorHost."""

import argparse
import json
import os
from pathlib import Path
import queue
import secrets
import subprocess
import tempfile
import threading
import time


Sources = Path(__file__).resolve().parent
Prefix = "GARGANTUAN_EDITOR/1 "


class EditorHost:
    def __init__(self, Engine):
        self.Engine = Engine
        self.Token = secrets.token_hex(16)
        self.Request = 0
        self.Replies = queue.Queue()

    def __enter__(self):
        self.Log = tempfile.TemporaryFile()
        self.Process = subprocess.Popen(
            [self.Engine, "--editor-host", "--editor-token", self.Token],
            cwd=self.Engine.parent,
            env=dict(os.environ, SDL_AUDIODRIVER="dummy"),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.Log,
            encoding="utf-8",
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        self.Reader = threading.Thread(target=self.Read, daemon=True)
        self.Reader.start()
        return self

    def Read(self):
        try:
            for Line in self.Process.stdout:
                if Line.startswith(Prefix):
                    self.Replies.put(json.loads(Line[len(Prefix):]))
        finally:
            self.Replies.put(None)

    def Call(self, Method, **Params):
        self.Request += 1
        Message = {"Version": 1, "RequestId": str(self.Request), "SessionToken": self.Token,
                   "Method": Method, "Params": Params}
        self.Process.stdin.write(json.dumps(Message, separators=(",", ":")) + "\n")
        self.Process.stdin.flush()
        try:
            Reply = self.Replies.get(timeout=15)
        except queue.Empty:
            raise RuntimeError(f"{Method} did not respond within 15 seconds") from None
        if not Reply or Reply["RequestId"] != Message["RequestId"]:
            raise RuntimeError(f"{Method} did not receive its EditorHost response")
        if not Reply["Ok"]:
            Error = Reply["Error"]
            raise RuntimeError(f"{Method} failed ({Error['Code']}) {Error['Message']}")
        return Reply["Result"]

    def __exit__(self, ErrorType, Error, Traceback):
        self.Process.stdin.close()
        try:
            ExitCode = self.Process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.Process.kill()
            self.Process.wait(timeout=5)
            if Error is None:
                raise RuntimeError("EditorHost did not close")
        else:
            if ExitCode and Error is None:
                self.Log.seek(0)
                raise RuntimeError(self.Log.read().decode("utf-8", errors="replace")[-4000:])
        finally:
            self.Reader.join(timeout=2)
            self.Process.stdout.close()
            self.Log.close()


def TerrainBrushes():
    for X in range(-4, 106, 7):
        for Z in (5, 12, 19, 26):
            yield {"Shape": "Box", "Mode": "Add", "Center": [X, 1.6, Z],
                   "HalfExtent": [4.0, 2.0, 4.0], "Material": 3}
    for X in (59, 66, 73, 80, 87):
        for Y in (5.1, 7.2):
            yield {"Shape": "Box", "Mode": "Add", "Center": [X, Y, 23],
                   "HalfExtent": [4.0, 2.0, 4.0], "Material": 2 if X < 80 else 6}
    for X, Y, Z, Radius, Material in (
        (13, 4.6, 23, 3.3, 2), (18, 5.5, 24, 3.3, 2),
        (30, 6.3, 23, 3.3, 6), (33, 8.6, 21, 3.2, 6),
        (59, 8.9, 24, 3.3, 2), (65, 9.6, 21, 3.2, 2),
        (71, 9.0, 24, 3.3, 2), (77, 9.5, 22, 3.2, 2),
        (83, 9.7, 23, 3.3, 6), (89, 9.0, 24, 3.1, 6),
        (94, 5.8, 24, 3.3, 2), (100, 4.3, 23, 3.3, 2),
    ):
        yield {"Shape": "Sphere", "Mode": "Add", "Center": [X, Y, Z],
               "Radius": Radius, "Material": Material}
    for X, Y, Z, Radius in (
        (62, 4.2, 17, 2.6), (64, 4.3, 20, 2.8),
        (68, 4.4, 22, 2.9), (72, 4.5, 23, 3.2),
        (75, 4.7, 25, 3.0), (77, 4.7, 28, 2.7),
        (76, 4.6, 21, 2.8), (80, 4.8, 20, 2.9),
        (83, 4.9, 18, 2.7),
    ):
        yield {"Shape": "Sphere", "Mode": "Subtract", "Center": [X, Y, Z], "Radius": Radius}


def Stamp(Host, State, Brush):
    Scope = State["Scope"]
    Ticket = Host.Call("SubmitTerrainBrush", Scope=Scope, Brush=Brush,
                       ExpectedTerrainRevision=State["TerrainRevision"],
                       ExpectedRevision=State["ProjectState"]["AuthoritativeRevision"])["Ticket"]
    Deadline = time.monotonic() + 20
    while time.monotonic() < Deadline:
        Receipt = Host.Call("GetTerrainEditStatus", Scope=Scope, Ticket=Ticket)
        if Receipt["Status"] == "Committed":
            return Receipt
        if Receipt["Status"] != "Pending":
            raise RuntimeError(f"Terrain brush ended with {Receipt['Status']} {Receipt['Diagnostic']}")
        time.sleep(0.01)
    Host.Call("CancelTerrainEdit", Scope=Scope, Ticket=Ticket)
    raise RuntimeError("Terrain brush did not commit within 20 seconds")


def BuildProject(Engine, Destination, Qualification, MixedLod=False):
    with EditorHost(Engine) as Host:
        Capabilities = Host.Call("Handshake")["Capabilities"]
        if "TerrainAuthoringV1" not in Capabilities or "CreateScriptWithSource" not in Capabilities:
            raise RuntimeError("Use an Engine with terrain authoring and script creation support")
        Created = Host.Call("CreateProject", Destination=Destination.as_posix(), Name="Terrain Quarry")
        Root = Created["Root"]
        Snapshot = Host.Call("GetSnapshot")["Snapshot"]
        Objects = {Object["ClassName"]: Object["Id"] for Object in Snapshot["Objects"] if Object["Parent"] == Root}
        Definitions = {Definition["Name"]: Definition for Definition in Host.Call("GetSchema")["Definitions"]
                       if Definition["Kind"] == "Class" and Definition["Namespace"] == "Engine"}

        def Create(ClassName, Name, Parent, **Details):
            Definition = Definitions[ClassName]
            return Host.Call("CreateInstance", ClassSchemaId=Definition["SchemaId"],
                             DefinitionVersion=Definition["DefinitionVersion"],
                             Name=Name, Parent=Parent, **Details)["Object"]

        Course = Create("Folder", "QuarryCourse", Objects["Workspace"])
        Create("RemoteEvent", "WorkRequest", Course)
        if Qualification:
            Host.Call("SetAttribute", Object=Course, Attribute="QualificationRoute",
                      Value={"Type": "Bool", "Value": True})
        if MixedLod:
            Host.Call("SetAttribute", Object=Objects["Workspace"], Attribute="TerrainVisualDensity",
                      Value={"Type": "Bool", "Value": True})
        ScriptClass = Definitions["Script"]
        for Name, Context in (("QuarryServer", "Server"), ("QuarryClient", "Client")):
            Create("Script", Name, Root, InitialSource=(Sources / f"{Name}.luau").read_text(encoding="utf-8"),
                   InitialProperties=[{"DeclaringClassSchemaId": ScriptClass["SchemaId"],
                                       "DeclaringDefinitionVersion": ScriptClass["DefinitionVersion"],
                                       "Property": "RunContext",
                                       "Value": {"Type": "EnumItem", "Enum": "RunContext", "Value": Context}}])

        State = Host.Call("GetTerrainAuthoringState")
        for Brush in TerrainBrushes():
            try:
                State = Stamp(Host, State, Brush)
            except RuntimeError as Error:
                raise RuntimeError(f"Terrain brush at {Brush['Center']} failed") from Error
        Saved = Host.Call("SaveProject")
        Before = json.loads((Destination / ".gargantuan/project.instance.json").read_text(encoding="utf-8"))
        Host.Call("OpenProject", Root=Destination.as_posix())
        After = Host.Call("GetTerrainAuthoringState")
        Hit = Host.Call("RaycastTerrainSurface", Scope=After["Scope"], ExpectedTerrainRevision=After["TerrainRevision"],
                        Origin=[6, 16, 8], Direction=[0, -16, 0])["Hit"]
        if not Before.get("TerrainManifest") or not After["EditingAvailable"] or not Hit:
            raise RuntimeError("The saved project did not reopen with its authored terrain")
    print(f"[Terrain:Quarry] Saved revision {Saved['PersistedRevision']} to {Destination}")


if __name__ == "__main__":
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--engine", type=Path, required=True, help="Path to the Engine executable")
    Parser.add_argument("--project", type=Path, required=True, help="New project directory")
    Parser.add_argument("--qualification", action="store_true", help="Enable the scripted two-client traversal")
    Parser.add_argument("--mixed-lod", action="store_true", help="Enable camera-driven terrain detail during Play")
    Args = Parser.parse_args()
    BuildProject(Args.engine.resolve(strict=True), Args.project.resolve(), Args.qualification, Args.mixed_lod)
