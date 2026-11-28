#!/usr/bin/env python3
"""Refresh only the two authored freight/door scripts in a new copy of a Studio Yard."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys

Sources = Path(__file__).resolve().parent
sys.path.insert(0, str(Sources.parent / "TerrainQuarry"))
from BuildProject import EditorHost

Names = ("Yard", "YardPlatform")
Document = Path(".gargantuan/project.instance.json")


def Files(Root):
    return {File.relative_to(Root): hashlib.sha256(File.read_bytes()).hexdigest()
            for File in Root.rglob("*") if File.is_file()}


def ExpectedDocument(Root, Texts):
    Value = json.loads((Root / Document).read_bytes())
    for Name, Source in Texts.items():
        Nodes = [Node for Node in Value["Children"] if Node["ClassName"] == "Script" and Node["Name"] == Name]
        if len(Nodes) != 1 or set(Nodes[0]["Properties"]["Source"]) != {"String"}:
            raise ValueError(f"Missing unique authored {Name} root Script")
        Nodes[0]["Properties"]["Source"]["String"] = Source
    return Value


def ReadScripts(Host):
    Objects = Host.Call("GetSnapshot")["Snapshot"]["Objects"]
    Root, = [Object["Id"] for Object in Objects if Object["ClassName"] == "DataModel"]
    Result = {}
    for Name in Names:
        Object, = [Row["Id"] for Row in Objects if Row["Parent"] == Root and
                   Row["ClassName"] == "Script" and Row["Name"] == Name]
        Result[Name] = (Object, Host.Call("GetScriptBinding", Object=Object)["Binding"],
                        Host.Call("GetScriptSource", Object=Object))
    return Result


def Refresh(Engine, Original, Destination):
    Original = Original.resolve(strict=True)
    Destination = Destination.resolve()
    if Destination.exists() or Destination.is_relative_to(Original):
        raise ValueError("Specify a new destination outside the original project")
    Texts = {Name: (Sources / (Name + ".luau")).read_text(encoding="utf-8") for Name in Names}
    Expected = ExpectedDocument(Original, Texts)
    OriginalFiles = Files(Original)
    shutil.copytree(Original, Destination)
    with EditorHost(Engine.resolve(strict=True)) as Host:
        Host.Call("Handshake")
        Host.Call("OpenProject", Root=Destination.as_posix())
        Before = ReadScripts(Host)
        for Name, (Object, _, Current) in Before.items():
            if Current["Source"] != Texts[Name]:
                Host.Call("SetScriptSource", Object=Object, ExpectedSourceVersion=Current["SourceVersion"], Source=Texts[Name])
        Host.Call("SaveProject")
        Host.Call("OpenProject", Root=Destination.as_posix())
        for Name, (_, Binding, Current) in ReadScripts(Host).items():
            if Binding != Before[Name][1] or Current["Source"] != Texts[Name]:
                raise ValueError(f"{Name} binding or source changed on reopen")
    After = Files(Destination)
    if json.loads((Destination / Document).read_bytes()) != Expected:
        raise ValueError("Authored document differs outside the two script source strings")
    if {K: V for K, V in After.items() if K != Document} != {K: V for K, V in OriginalFiles.items() if K != Document}:
        raise ValueError("A file outside the authored document changed")
    if Files(Original) != OriginalFiles:
        raise ValueError("Original project changed during the refresh")
    print("[Spatial:YardFreight] " + json.dumps({"Original": OriginalFiles[Document], "Refreshed": After[Document],
          "Files": len(After), "Bindings": {Name: Before[Name][1] for Name in Names}}, sort_keys=True))


if __name__ == "__main__":
    Parser = argparse.ArgumentParser(description=__doc__)
    for Name in ("engine", "project", "destination"):
        Parser.add_argument("--" + Name, type=Path, required=True)
    Args = Parser.parse_args()
    Refresh(Args.engine, Args.project, Args.destination)
