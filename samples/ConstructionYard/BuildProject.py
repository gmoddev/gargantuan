#!/usr/bin/env python3
"""Create and reopen the Construction Yard through the production EditorHost."""
import argparse
from pathlib import Path
import sys

Sources = Path(__file__).resolve().parent
sys.path.insert(0, str(Sources.parent / "TerrainQuarry"))
from BuildProject import EditorHost, Stamp


def Build(Engine, Destination):
    if Destination.exists():
        raise ValueError("Choose a new directory for the sample project")
    with EditorHost(Engine) as Host:
        Capabilities = Host.Call("Handshake")["Capabilities"]
        for Method in ("EditBlockGridRegion", "TerrainAuthoringV1", "CreateScriptWithSource"):
            if Method not in Capabilities:
                raise RuntimeError(f"The selected Engine lacks {Method}")
        Root = Host.Call("CreateProject", Destination=Destination.as_posix(), Name="Construction Yard")["Root"]
        Snapshot = Host.Call("GetSnapshot")["Snapshot"]
        Scope = Snapshot["Cursor"]["Scope"]
        Space = next(Item["Id"] for Item in Snapshot["Objects"] if Item["ClassName"] == "Workspace")
        Schema = {Row["Name"]: Row for Row in Host.Call("GetSchema")["Definitions"]
                  if Row["Kind"] == "Class" and Row["Namespace"] == "Engine"}

        def Create(Class, Name, Parent, **Details):
            Entry = Schema[Class]
            return Host.Call("CreateInstance", ClassSchemaId=Entry["SchemaId"],
                             DefinitionVersion=Entry["DefinitionVersion"], Parent=Parent, Name=Name, **Details)["Object"]

        def Fill(Grid, Low, High, Definition):
            Address = dict(Scope=Scope, Object=Grid, Cell=Low)
            Revision = Host.Call("ReadBlockGridCell", **Address)["Revision"]
            return Host.Call("EditBlockGridRegion", **Address, Revision=Revision, MaxExclusive=High, Definition=Definition)

        Outside = Create("BlockGrid", "RetainingWall", Space)
        Interior = Create("SpatialRegion", "Workshop", Space)
        Inside = Create("BlockGrid", "WorkshopBlocks", Interior)
        Fill(Outside, [-5, -2, -5], [6, 2, -4], "Brick")
        Fill(Outside, [-5, -2, -4], [-4, 2, 4], "Stone")
        Fill(Inside, [-7, 0, -7], [8, 1, 8], "Stone")
        Fill(Inside, [-7, 1, -7], [8, 6, -6], "Brick")
        Fill(Inside, [-7, 1, -6], [-6, 5, 6], "Brick")
        State = Host.Call("GetTerrainAuthoringState")
        for X in (-5, 1, 7):
            for Z in (-5, 1, 7):
                State = Stamp(Host, State, dict(Shape="Box", Mode="Add", Center=[X, -2.5, Z],
                                               HalfExtent=[4, 2, 4], Material=3))
        State = Stamp(Host, State, dict(Shape="Sphere", Mode="Add", Center=[7, -1.1, -5], Radius=3, Material=2))
        Stamp(Host, State, dict(Shape="Sphere", Mode="Subtract", Center=[1, -0.7, -3], Radius=2.1))
        Script = Schema["Script"]
        Storage = next(Item["Id"] for Item in Snapshot["Objects"] if Item["ClassName"] == "ReplicatedStorage")
        Create("RemoteEvent", "YardRequest", Storage)
        for Name, Context in (("Yard", "Server"), ("YardInput", "Client")):
            Create("Script", Name, Root, InitialSource=(Sources / f"{Name}.luau").read_text(encoding="utf-8"),
                   InitialProperties=[dict(DeclaringClassSchemaId=Script["SchemaId"],
                                           DeclaringDefinitionVersion=Script["DefinitionVersion"], Property="RunContext",
                                           Value=dict(Type="EnumItem", Enum="RunContext", Value=Context))])
        Host.Call("SaveProject")
        Host.Call("OpenProject", Root=Destination.as_posix())
        Reopened = Host.Call("GetSnapshot")["Snapshot"]
        for Name, Cell, Expected in (("RetainingWall", [-5, 0, -5], "Brick"), ("WorkshopBlocks", [0, 0, 0], "Stone")):
            Grid = next(Item["Id"] for Item in Reopened["Objects"] if Item["Name"] == Name)
            Receipt = Host.Call("ReadBlockGridCell", Scope=Reopened["Cursor"]["Scope"], Object=Grid, Cell=Cell)
            if Receipt["Definition"] != Expected:
                raise RuntimeError(f"Reopened {Name} lost {Expected}")
        if not Host.Call("GetTerrainAuthoringState")["EditingAvailable"]:
            raise RuntimeError("Terrain authoring did not reopen")
    print(f"[Spatial:ConstructionYard] Saved and reopened {Destination}")


if __name__ == "__main__":
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--engine", type=Path, required=True)
    Parser.add_argument("--project", type=Path, required=True)
    Args = Parser.parse_args()
    Build(Args.engine.resolve(strict=True), Args.project.resolve())
