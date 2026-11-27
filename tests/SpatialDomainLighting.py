# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
"""Compare coincident main-camera domains using real EditorHost shadow and light pixels."""
import argparse
import base64
from pathlib import Path
import tempfile

from SpatialCreatorTrial import EditorHost


def Differences(First, Second):
    return sum(First[I:I + 3] != Second[I:I + 3] for I in range(0, len(First), 3))


def Check(Engine, Project):
    with EditorHost(Engine) as Host:
        Host.Call("Handshake")
        Host.Call("CreateProject", Destination=Project.as_posix(), Name="Domain Lighting")
        Snapshot = Host.Call("GetSnapshot")["Snapshot"]
        Scope = Snapshot["Cursor"]["Scope"]
        Workspace = next(Row["Id"] for Row in Snapshot["Objects"] if Row["ClassName"] == "Workspace")
        Schema = {Row["Name"]: Row for Row in Host.Call("GetSchema")["Definitions"]
                  if Row["Kind"] == "Class" and Row["Namespace"] == "Engine"}

        def Create(Class, Name, Parent):
            Definition = Schema[Class]
            return Host.Call("CreateInstance", Parent=Parent, Name=Name, ClassSchemaId=Definition["SchemaId"],
                             DefinitionVersion=Definition["DefinitionVersion"])["Object"]

        def Set(Object, Class, Owner, Name, Type, Value):
            Host.Call("SetProperty", Object=Object, Property=Name, Value={"Type": Type, "Value": Value},
                      ClassSchemaId=Schema[Class]["SchemaId"], ClassDefinitionVersion=Schema[Class]["DefinitionVersion"],
                      DeclaringClassSchemaId=Schema[Owner]["SchemaId"], DeclaringDefinitionVersion=Schema[Owner]["DefinitionVersion"])

        def Part(Name, Parent, Position, Size):
            Object = Create("Part", Name, Parent)
            Set(Object, "Part", "BasePart", "Anchored", "Bool", True)
            Set(Object, "Part", "BasePart", "Size", "Vector3", Size)
            Set(Object, "Part", "BasePart", "CFrame", "CFrame", Position + [1, 0, 0, 0, 1, 0, 0, 0, 1])
            return Object

        def Capture(Domain):
            Host.Call("SetEditorSpatialView", Scope=Scope, Object=Domain)
            Frame = Host.Call("CaptureViewport")
            Pixels = base64.b64decode(Frame["Pixels"], validate=True)
            assert Frame["Format"] == "RGB8" and len(Pixels) == 192 * 144 * 3
            return Pixels

        First = Create("SpatialRegion", "First", Workspace)
        Second = Create("SpatialRegion", "Second", Workspace)
        for Room in (First, Second):
            Part("Receiver", Room, [0, -0.5, 0], [16, 1, 16])
        Host.Call("ConfigureViewport", Width=192, Height=144)
        Host.Call("SetViewportCamera", Position=[8, 8, 12], Target=[0, 0, 0], FieldOfView=48)
        FirstBaseline = Capture(First)
        assert Capture(Second) == FirstBaseline, "coincident identical receivers differ across domains"

        Caster = Part("Caster", Second, [1, 3, 0], [3, 2, 2])
        Shadow = Capture(Second)
        assert Capture(First) == FirstBaseline, "the adjacent domain received the caster or its shadow"
        Set(Caster, "Part", "BasePart", "CastShadow", "Bool", False)
        Unshadowed = Capture(Second)
        ShadowPixels = Differences(Shadow, Unshadowed)
        assert ShadowPixels > 0, "disabling only the selected caster produced no shadow difference"
        assert Capture(First) == FirstBaseline, "the foreign caster or shadow changed the first domain"
        Set(Caster, "Part", "BasePart", "CastShadow", "Bool", True)
        assert Capture(Second) == Shadow, "restored shadow did not reproduce its original pixels"

        Light = Create("PointLight", "Local lamp", Caster)
        Set(Light, "PointLight", "LocalLight", "LightColor", "Color3", [1, 0, 0])
        Lit = Capture(Second)
        LightPixels = Differences(Shadow, Lit)
        assert LightPixels > 0, "adding the admitted light produced no local illumination difference"
        assert Capture(First) == FirstBaseline, "the foreign lamp changed the first domain"
        print(f"[Spatial:Lighting] Local shadow {ShadowPixels} pixels, local lamp {LightPixels} pixels, "
              "foreign differences 0")


if __name__ == "__main__":
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--engine", type=Path, required=True)
    with tempfile.TemporaryDirectory(prefix="gdl-") as Directory:
        Check(Parser.parse_args().engine.resolve(strict=True), Path(Directory) / "Project")
