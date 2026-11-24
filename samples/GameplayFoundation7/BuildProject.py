#!/usr/bin/env python3
"""Sync the editable Luau sources into the Studio and packager project."""

import argparse
import json
from pathlib import Path


Root = Path(__file__).resolve().parent
Project = Root / ".gargantuan" / "project.instance.json"
ClassIds = {
    "DataModel": "5ed5470f622082d244a9379d906bbd16",
    "Workspace": "9ae26e41e446cbfed61d91dfcab22b12",
    "Folder": "dfa3ba4ea6909035cee5b32930b48871",
    "Part": "5eab0d5368149cacdcc9dcd4130cd3c8",
    "MeshPart": "6ae6d9714ca7c7d16cff97f58d855f1d",
    "Animator": "7b80b401f8112e6c1cb141a88ff418b0",
    "Script": "c77fa1af2c8ca9d2e04f3d523d8cd7de",
    "StarterGui": "dcd45746f05b46ef1878421e1b584c3b",
    "ScreenGui": "eeb2271af087e2bc4b7daf31c48ca860",
    "TextLabel": "d4e8cf1ffaaeafec999969428a2e32f8",
    "ImageButton": "faf06fa435e7be2ac710c271254a4ae6",
    "ViewportFrame": "ebf35a29db2f03c0ab39bb86cc89e88c",
    "Camera": "8152394d3f103e16a76125bab068cc7a",
}


def Instance(ClassName, Name, Properties=None, Children=()):
    return {
        "Name": Name,
        "ClassName": ClassName,
        "ClassSchemaId": ClassIds[ClassName],
        "ClassDefinitionVersion": 1,
        "Properties": Properties or {},
        "Attributes": {},
        "Extensions": [],
        "CustomProperties": [],
        "Tags": [],
        "Children": list(Children),
    }


def Part(Name, Position, Size, Color, Collides=True, ClassName="Part", Properties=None, Children=()):
    Body = {
        "Anchored": {"Bool": True},
        "CanCollide": {"Bool": Collides},
        "CanTouch": {"Bool": False},
        "CFrame": {"CFrame": [*Position, 1, 0, 0, 0, 1, 0, 0, 0, 1]},
        "Size": {"Vector3": Size},
        "Color": {"Color3": Color},
    }
    Body.update(Properties or {})
    return Instance(ClassName, Name, Body, Children)


def Script(Name, Role, SourceId):
    return Instance("Script", Name, {
        "Enabled": {"Bool": True},
        "RunContext": {"EnumItem": ["RunContext", Role]},
        "SourceId": {"String": SourceId},
        "Source": {"String": (Root / f"{Name}.luau").read_text(encoding="utf-8")},
    })


def Gui(ClassName, Name, Position, Size, Properties=None, Children=()):
    Layout = {"Position": {"UDim2": Position}, "Size": {"UDim2": Size}}
    Layout.update(Properties or {})
    return Instance(ClassName, Name, Layout, Children)


def Nodes(Node):
    yield Node
    for Child in Node["Children"]:
        yield from Nodes(Child)


def BuildProject():
    Course = Instance("Folder", "RelayCourse", Children=[
        Part("Ground", [0, -0.5, 0], [64, 1, 50], [0.12, 0.17, 0.22]),
        Part("LaunchPad", [-9, 0.1, -5], [5, 0.2, 5], [0.08, 0.82, 0.79], False),
        Part("GroundSwitch", [9, 0.1, -6], [5, 0.2, 5], [0.34, 0.52, 0.12], False),
        Part("Hazard", [0, 0.1, -2], [5, 0.2, 4], [0.95, 0.29, 0.09], False),
        Part("AirspaceDivider", [0, 4.5, -10], [4, 9, 2], [0.28, 0.34, 0.42]),
        Part("RelayPlatform", [7, 2.25, -15], [5, 4.5, 5], [0.20, 0.30, 0.39]),
        Part("RelayBeacon", [7, 7.1, -15], [3, 3, 3], [0.39, 0.92, 1], False, "MeshPart",
             {"Mesh": {"String": "asset://d549080bd1e64aaee8041f4ece3e9f75"}},
             [Instance("Animator", "PulseAnimator")]),
        Part("NorthWall", [0, 4.5, -25], [64, 9, 1], [0.22, 0.28, 0.35]),
        Part("CargoDock", [18, 0.3, 5], [12, 0.6, 8], [0.16, 0.39, 0.44],
             Properties={"Friction": {"Float": 1.5}, "Restitution": {"Float": 0.75}}),
        Part("CargoMarker", [15, 4, 5], [1.4, 1.4, 1.4], [1, 0.66, 0.15],
             Properties={"Shape": {"EnumItem": ["PartType", "Ball"]}}),
    ])
    EquipmentCamera = Instance("Camera", "EquipmentCamera", {
        "CFrame": {"CFrame": [0, 0, 5, 1, 0, 0, 0, 1, 0, 0, 0, 1]},
        "FieldOfView": {"Float": 42},
    })
    Equipment = Gui("ViewportFrame", "EquipmentPreview", [1, -134, 1, -100], [0, 118, 0, 84], {
        "BackgroundColor3": {"Color3": [0.04, 0.09, 0.13]},
        "BackgroundTransparency": {"Float": 0.12},
        "ClipsDescendants": {"Bool": True},
    }, [
        EquipmentCamera,
        Part("Crystal", [0, 0, 0], [2, 2, 2], [0.39, 0.92, 1], False, "MeshPart",
             {"Mesh": {"String": "asset://d549080bd1e64aaee8041f4ece3e9f75"}},
             [Instance("Animator", "PreviewAnimator")]),
    ])
    Hud = Instance("ScreenGui", "RelayHud", {"ResetOnSpawn": {"Bool": False}}, [
        Gui("TextLabel", "RelayStatus", [0, 16, 0, 16], [1, -32, 0, 56], {
            "Text": {"String": "Relay Rescue"},
            "BackgroundColor3": {"Color3": [0.03, 0.08, 0.13]},
            "BackgroundTransparency": {"Float": 0.15},
            "TextColor3": {"Color3": [0.84, 1, 0.94]},
            "TextSize": {"Float": 17},
            "TextWrapped": {"Bool": True},
        }),
        Gui("ImageButton", "RelayMixToggle", [1, -64, 0, 80], [0, 48, 0, 36], {
            "Image": {"String": "asset://d18f6e730a55d41e65e2cc2447294884"},
            "ScaleType": {"EnumItem": ["GuiImageScaleType", "Fit"]},
            "BackgroundColor3": {"Color3": [0.12, 0.22, 0.26]},
            "AccessibleName": {"String": "Mute game audio"},
        }),
        Gui("ImageButton", "EquipmentToggle", [1, -118, 0, 80], [0, 48, 0, 36], {
            "Image": {"String": "asset://d18f6e730a55d41e65e2cc2447294884"},
            "ScaleType": {"EnumItem": ["GuiImageScaleType", "Fit"]},
            "ImageColor3": {"Color3": [0.62, 0.86, 1.0]},
            "AccessibleName": {"String": "Toggle equipment preview"},
        }),
        Equipment,
        Script("RelayEquipment", "Client", "ffb5ccbb7c96448a921874cebb5b9cc9"),
    ])
    Briefing = Instance("ScreenGui", "RelayBriefing", {"ResetOnSpawn": {"Bool": True}}, [
        Gui("TextLabel", "Briefing", [0, 16, 1, -46], [0, 156, 0, 30], {
            "Text": {"String": "Relay crystal equipped"},
            "TextSize": {"Float": 14},
            "TextColor3": {"Color3": [0.60, 0.86, 0.90]},
        }),
    ])
    World = Instance("DataModel", "Relay Rescue", Children=[
        Instance("Workspace", "Workspace", Children=[Course]),
        Instance("StarterGui", "StarterGui", Children=[Hud, Briefing]),
        Instance("Folder", "GameScripts", Children=[
            Script("RelayServer", "Server", "3fc0efec12504cc983789da661cb2817"),
            Script("RelayClient", "Client", "a3af72783f3147098ab0a0f72c610eb5"),
        ]),
    ])
    for Id, Node in enumerate(Nodes(World), start=1):
        Node["NodeId"] = Id
    Equipment["Properties"]["CurrentCamera"] = {"InstanceReference": EquipmentCamera["NodeId"]}
    return {"Version": 5, **World}


if __name__ == "__main__":
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--check", action="store_true", help="Check the snapshot without writing")
    Arguments = Parser.parse_args()
    Encoded = json.dumps(BuildProject(), indent=2, ensure_ascii=False) + "\n"
    if Arguments.check:
        if not Project.exists() or Project.read_text(encoding="utf-8") != Encoded:
            Parser.exit(1, "Project snapshot differs from the Luau sources. Run BuildProject.py.\n")
    else:
        Project.write_text(Encoded, encoding="utf-8", newline="\n")
