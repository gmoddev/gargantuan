"""Build the Construction Yard with an application menu and persistent world selection."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

Sources = Path(__file__).resolve().parent
sys.path.insert(0, str(Sources.parent / "TerrainQuarry"))
from BuildProject import EditorHost


def Build(Engine, Project):
    subprocess.run([sys.executable, "-B", Sources.parent / "ConstructionYard" / "BuildProject.py",
                    "--engine", Engine, "--project", Project], check=True)
    with EditorHost(Engine) as Host:
        Host.Call("OpenProject", Root=Project.as_posix())
        Objects = Host.Call("GetSnapshot")["Snapshot"]["Objects"]
        Root = next(Row["Id"] for Row in Objects if Row["ClassName"] == "DataModel")
        Schema = {Row["Name"]: Row for Row in Host.Call("GetSchema")["Definitions"]
                  if Row["Kind"] == "Class" and Row["Namespace"] == "Engine"}
        Folder, Script, Model = (Schema[Name] for Name in ("Folder", "Script", "DataModel"))
        App = Host.Call("CreateInstance", ClassSchemaId=Folder["SchemaId"], DefinitionVersion=Folder["DefinitionVersion"],
                        Parent=Root, Name="Application")["Object"]
        Host.Call("CreateInstance", ClassSchemaId=Script["SchemaId"], DefinitionVersion=Script["DefinitionVersion"],
                  Parent=App, Name="Entry", InitialSource=(Sources / "Entry.luau").read_text(encoding="utf-8"),
                  InitialProperties=[dict(DeclaringClassSchemaId=Script["SchemaId"],
                                          DeclaringDefinitionVersion=Script["DefinitionVersion"], Property="RunContext",
                                          Value=dict(Type="EnumItem", Enum="RunContext", Value="Client"))])
        Host.Call("SaveProject")
        Scene = json.loads((Project / ".gargantuan" / "project.instance.json").read_bytes())
        Entry = next(Item for Item in Scene["Children"] if Item["Name"] == "Application")["Children"][0]
        Host.Call("SetProperty", Object=Root, Property="ApplicationEntry",
                  Value=dict(Type="String", Value=Entry["Properties"]["SourceId"]["String"]),
                  ClassSchemaId=Model["SchemaId"], ClassDefinitionVersion=Model["DefinitionVersion"],
                  DeclaringClassSchemaId=Model["SchemaId"], DeclaringDefinitionVersion=Model["DefinitionVersion"])
        Host.Call("SaveProject")


if __name__ == "__main__":
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--engine", required=True, type=Path)
    Parser.add_argument("--project", required=True, type=Path)
    Args = Parser.parse_args()
    Build(Args.engine.resolve(strict=True), Args.project.resolve())
