"""Exercise the Studio region clipboard and definition assets through the actual EditorHost."""
import argparse
import base64
import json
from pathlib import Path
import struct
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "samples" / "TerrainQuarry"))
from BuildProject import EditorHost


def Reject(Action, Reason):
    try:
        Action()
    except RuntimeError as Error:
        if Reason not in str(Error):
            raise
    else:
        raise AssertionError(f"Expected {Reason}")


def WriteMesh(Directory):
    Payload = struct.pack("<9f3H", -.4, -.3, 0, .4, -.3, 0, 0, .4, 0, 0, 1, 2)
    Uri = "data:application/octet-stream;base64," + base64.b64encode(Payload).decode("ascii")
    Mesh = {
        "asset": {"version": "2.0"}, "buffers": [{"byteLength": len(Payload), "uri": Uri}],
        "bufferViews": [{"buffer": 0, "byteLength": 36}, {"buffer": 0, "byteOffset": 36, "byteLength": 6}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
                       "min": [-.4, -.3, 0], "max": [.4, .4, 0]},
                      {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}],
        "materials": [{"name": "Copper", "pbrMetallicRoughness": {"baseColorFactor": [.9, .4, .2, 1]}}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1, "material": 0}]}],
        "nodes": [{"mesh": 0}], "scenes": [{"nodes": [0]}], "scene": 0,
    }
    Directory.mkdir(exist_ok=True)
    (Directory / "panel.gltf").write_text(json.dumps(Mesh), encoding="utf-8")


def Run(Engine, Project):
    with EditorHost(Engine) as Host:
        Call = Host.Call
        assert "PasteBlockGridRegion" in Call("Handshake")["Capabilities"]
        Call("CreateProject", Destination=Project.as_posix(), Name="Grid stamp")
        Snapshot = Call("GetSnapshot")["Snapshot"]
        Scope = Snapshot["Cursor"]["Scope"]
        Space = next(Item["Id"] for Item in Snapshot["Objects"] if Item["ClassName"] == "Workspace")
        Schema = next(Row for Row in Call("GetSchema")["Definitions"] if Row["Name"] == "BlockGrid" and Row["Kind"] == "Class")

        def Grid(Name):
            return Call("CreateInstance", ClassSchemaId=Schema["SchemaId"], DefinitionVersion=Schema["DefinitionVersion"],
                        Name=Name, Parent=Space)["Object"]

        def Read(Object, Cell):
            return Call("ReadBlockGridCell", Scope=Scope, Object=Object, Cell=Cell)

        def Fill(Object, Low, High, Name):
            return Call("EditBlockGridRegion", Scope=Scope, Object=Object, Cell=Low, MaxExclusive=High, Definition=Name,
                        Revision=Read(Object, Low)["Revision"])

        Source, Target = Grid("Source"), Grid("Destination")
        WriteMesh(Project / "assets")
        assert Call("ImportAsset", Source="assets/panel.gltf", Kind="Mesh")["OperationSucceeded"]
        Resources = {Row["Kind"]: Row["Reference"] for Row in Call("GetAssetCatalog", IncludeBuiltIns=False)["Assets"]}

        def Define(Name, Shape, **Assets):
            return Call("DefineBlockGridBlock", Scope=Scope, Object=Source, Definition=Name, Shape=Shape, Color=[220, 180, 140],
                        Revision=Read(Source, [0, 0, 0])["Revision"], **Assets)

        Reject(lambda: Define("project:bad", "Mesh", Mesh=Resources["Material"]), "wrong role")
        Define("project:stair", "Wedge", Material=Resources["Material"])
        Define("project:panel", "Mesh", Mesh=Resources["Mesh"], Collision=Resources["Collision"])
        Fill(Source, [0, 0, 0], [1, 1, 1], "project:stair")
        Fill(Source, [2, 0, 1], [3, 1, 2], "project:panel")
        Copy = Call("CopyBlockGridRegion", Scope=Scope, Object=Source, Cell=[0, 0, 0], MaxExclusive=[3, 1, 2])
        assert Copy["Cells"] == 6
        Fill(Source, [0, 0, 0], [3, 1, 2], "Air")
        Min = [-8, 3, 5]
        Request = dict(Scope=Scope, Object=Target, Cell=Min, QuarterTurns=1, Clipboard=Copy["Clipboard"])
        Fill(Target, Min, [-6, 4, 8], "Stone")
        Preview = Call("PreviewBlockGridPaste", **Request)
        assert Preview["Size"] == [2, 1, 3] and Preview["Occupied"] == 2 and Preview["Replaced"] == 6
        assert len(Preview["Outline"]) == 12 and 3 <= len(Preview["Geometry"]) <= 192
        assert all(len(Edge) == 6 for Edge in Preview["Geometry"]) and not Preview["Clipped"]
        Reject(lambda: Call("PasteBlockGridRegion", **dict(Request, QuarterTurns=2), Revision=Preview["Revision"]), "RevisionConflict")
        Fill(Target, [-8, 3, 5], [-7, 4, 6], "Brick")
        Reject(lambda: Call("PasteBlockGridRegion", **Request, Revision=Preview["Revision"]), "RevisionConflict")
        Preview = Call("PreviewBlockGridPaste", **Request)
        Call("PasteBlockGridRegion", **Request, Revision=Preview["Revision"])
        assert Read(Target, [-8, 3, 7])["Definition"] == "project:stair"
        assert Read(Target, [-7, 3, 5])["Definition"] == "project:panel"
        assert Read(Target, [-8, 3, 5])["Definition"] == "Air" and Read(Target, [-8, 3, 7])["Orientation"] != 0
        Call("Undo")
        assert Read(Target, Min)["Definition"] == "Brick" and Read(Target, [-8, 3, 7])["Definition"] == "Stone"
        assert "project:stair" not in Call("ReadBlockGridPalette", Scope=Scope, Object=Target)["Definitions"]
        Call("Redo")
        assert Read(Target, [-7, 3, 5])["Definition"] == "project:panel"
        Call("SaveProject")
        Call("OpenProject", Root=Project.as_posix())
        Snapshot = Call("GetSnapshot")["Snapshot"]
        Target = next(Item["Id"] for Item in Snapshot["Objects"] if Item["Name"] == "Destination")
        Scope = Snapshot["Cursor"]["Scope"]
        assert Read(Target, [-8, 3, 7])["Definition"] == "project:stair"
        Reject(lambda: Call("PreviewBlockGridPaste", **dict(Request, Scope=Scope, Object=Target)), "StaleClipboard")
    print("[Studio:BlockGrid] Copied air, oriented wedges and pinned mesh/material assets across grids. Preview, undo, redo and reopen passed.")


if __name__ == "__main__":
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--engine", type=Path, required=True)
    with tempfile.TemporaryDirectory(prefix="GargantuanGridClipboard-") as Temp:
        Run(Parser.parse_args().engine, Path(Temp) / "Project")

