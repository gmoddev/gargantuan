# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Inspect independent edit and Play domains over the actual Studio protocol."""
import argparse
from pathlib import Path
import tempfile
import time

from SpatialCreatorTrial import EditorHost


def Check(Engine, Project):
    with EditorHost(Engine) as Host:
        Host.Call("Handshake")
        Host.Call("CreateProject", Destination=Project.as_posix(), Name="Domain inspection")
        Snapshot = Host.Call("GetSnapshot")["Snapshot"]
        Scope = Snapshot["Cursor"]["Scope"]
        Space = next(Item["Id"] for Item in Snapshot["Objects"] if Item["ClassName"] == "Workspace")
        Schema = {Row["Name"]: Row for Row in Host.Call("GetSchema")["Definitions"]
                  if Row["Kind"] == "Class" and Row["Namespace"] == "Engine"}

        def Create(Kind, Name, Parent, **Params):
            Definition = Schema[Kind]
            return Host.Call("CreateInstance", Parent=Parent, Name=Name, ClassSchemaId=Definition["SchemaId"],
                             DefinitionVersion=Definition["DefinitionVersion"], **Params)["Object"]

        def Refused(Method, Code, **Params):
            try:
                Host.Call(Method, **Params)
            except RuntimeError as Error:
                if Code in str(Error):
                    return
                raise
            raise RuntimeError(f"{Method} accepted a retired or inappropriate scope")

        Room = Create("SpatialRegion", "Workshop", Space)
        Create("BlockGrid", "IndependentGrid", Room)
        Create("Part", "IndependentPart", Room)
        Create("Script", "Preload", Room, InitialSource='''
local Room = script.Parent
workspace.CurrentSpatialRegion = Room
local Hold = assert(Room:RequestCollisionArea(Vector3.new(-4,-4,-4), Vector3.new(4,4,4), 10, "Interactive"))
while Hold:GetStatus().Life == "Active" do task.wait(.05) end
''')
        Outdoor = Host.Call("GetSpatialDomainState", Scope=Scope, Object=Space)
        Indoor = Host.Call("GetSpatialDomainState", Scope=Scope, Object=Room)
        assert Outdoor["Viewed"] and not Indoor["Viewed"] and Outdoor["RegisteredGrids"] == 0
        assert Indoor["RegisteredGrids"] == 1 and Indoor["TrackedParts"] == 1 and Indoor["World"] == "Edit"
        assert Indoor["Requests"] == {"Ready": 0, "Unavailable": 0, "Pending": 0}
        assert Host.Call("SetEditorSpatialView", Scope=Scope, Object=Room)["Viewed"]
        assert not Host.Call("GetSpatialDomainState", Scope=Scope, Object=Space)["Viewed"]
        assert Host.Call("SetEditorSpatialView", Scope=Scope, Object=Space)["Viewed"]
        Host.Call("SaveProject")
        Session = Host.Call("StartPlaySession", PersistenceMode="Disabled")["PlaySessionId"]
        try:
            Deadline = time.monotonic() + 10
            while True:
                Host.Call("PollPlayDiagnostics", PlaySessionId=Session)
                Live = Host.Call("GetSpatialDomainState", PlaySessionId=Session)
                if Live["Name"] == "Workshop" and Live["Requests"]["Ready"] == 1:
                    break
                if time.monotonic() >= Deadline:
                    raise RuntimeError(f"Play collision demand did not materialize: {Live}")
                time.sleep(.04)
            assert Live["World"] == "Play" and Live["Scope"] != Scope and Live["Object"] != Room
            assert Host.Call("GetSpatialDomainState", Scope=Scope, Object=Room)["Requests"] == Indoor["Requests"]
            Refused("SetEditorSpatialView", "ViewUnavailable", Scope=Scope, Object=Room)
            Refused("GetSpatialDomainState", "MalformedRequest", PlaySessionId=Session, Object=Room)
        finally:
            Host.Call("StopPlaySession", PlaySessionId=Session)
        Refused("GetSpatialDomainState", "StalePlaySession", PlaySessionId=Session)
        Host.Call("OpenProject", Root=Project.as_posix())
        Refused("GetSpatialDomainState", "StaleDomain", Scope=Scope, Object=Room)
        print("[Spatial:Studio] Independent edit selection, Play demand and stale scope checks passed")


if __name__ == "__main__":
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--engine", type=Path, required=True)
    with tempfile.TemporaryDirectory(prefix="gda-") as Temp:
        Check(Parser.parse_args().engine.resolve(strict=True), Path(Temp) / "Project")
