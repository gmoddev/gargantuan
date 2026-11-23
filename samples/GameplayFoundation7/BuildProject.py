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
    ])
    World = Instance("DataModel", "Relay Rescue", Children=[
        Instance("Workspace", "Workspace", Children=[Course]),
        Instance("Folder", "GameScripts", Children=[
            Script("RelayServer", "Server", "3fc0efec12504cc983789da661cb2817"),
            Script("RelayClient", "Client", "a3af72783f3147098ab0a0f72c610eb5"),
        ]),
    ])
    return {"Version": 4, **World}


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
