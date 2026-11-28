"""Project the existing declaration catalog and native registrations into creator pages."""
import argparse
import json
import re
from pathlib import Path
import CheckDocumentation as Docs

Root = Docs.Root
Source = "830f5f26ea5642c9a6d7e281db8df0fb3982e5df"
Content = Docs.Content / "reference"


def Encode(Value):
    return json.dumps(Value, indent=2, ensure_ascii=False) + "\n"


def Page(Title, Body):
    return f'---\ntitle: {Title}\ndescription: Public creator reference for {Title}.\n---\n\n' + Body + "\n"


def Build():
    Api = Docs.ExtractApi()
    Changed = Docs.Git("diff", "--name-only", Source, "--", "assets/classes", "assets/services", "src", "include")
    if Changed.strip():
        raise ValueError("API sources differ from documented revision; select and review a new Source: "+Changed)
    Definitions = {Entry["Name"]: Entry for Entry in Api["Classes"]}
    Outputs = {}
    Counts = {Key: 0 for Key in ("Properties", "Methods", "Signals", "Descriptions")}
    Status = f"Qualified development candidate `{Source}`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API."
    Index = [Status, "", "## Classes and services", ""]
    for Entry in Api["Classes"]:
        Name = Entry["Name"]
        Index.append(f'- [{Name}](./classes/{Name.lower()}.md) — {Entry["Kind"]}')
        Body = [Status, "", Entry["Description"], "", f'Kind `{Entry["Kind"]}`. Abstract `{Entry["Abstract"]}`. Service policy `{Entry["ServicePolicy"]}`.', ""]
        if Entry["Kind"] == "Service":
            Body += [f'Access through `game:GetService("{Name}")` subject to the service policy and runtime context.', ""]
        else:
            Body += ["Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.", ""]
        Parent = Entry["Extends"]
        if Entry.get("DefaultOverrides") != "{}":
            Body += ["## Declared default overrides", "", "Derived defaults override inherited declaration defaults. Expressions below preserve the authoritative declaration.", "", "```text", str(Entry["DefaultOverrides"]), "```", ""]
        Seen = {Name}
        while Parent:
            if Parent in Seen or Parent not in Definitions:
                raise ValueError(f"Invalid inheritance for {Name}: {Parent}")
            Seen.add(Parent)
            Body += [f'Inherited members from [{Parent}](./{Parent.lower()}.md).', ""]
            Parent = Definitions[Parent]["Extends"]
        for Section in ("Properties", "Methods", "Signals"):
            Body += [f"## {Section}", ""]
            for Member in Entry[Section]:
                Counts[Section] += 1
                Counts["Descriptions"] += bool(Member.get("Description"))
                Body += [f'### {Name}.{Member["Name"]}', "", Member.get("Description") or "Behavioral description is missing.", ""]
                Signature = Member.get("LuauSignature")
                if Signature:
                    Body += ["```text", Signature, "```", ""]
                Metadata = {Key: Value for Key, Value in Member.items() if Key not in ("Name", "Description", "LuauSignature")}
                Body += ["```json", Encode(Metadata).strip(), "```", "", "Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.", ""]
        Body += ["## Behavioral guides", "", "Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits."]
        Outputs[Content / "classes" / (Name.lower()+".md")] = Page(Name, "\n".join(Body))

    Native = {"SourceRevision": Source, "Status": "QualifiedDevelopmentCandidate", "Types": [], "Enums": []}
    Index += ["", "## Native datatypes and libraries", ""]
    Engine = Docs.RemoveComments(Docs.Read(Root / "src/scripting/ScriptEngine.cpp"))
    for Name, Open in re.findall(r'\{"(\w+)",\s*(OpenLib\w+)', Engine):
        File = Root / "src/scripting" / ("Lib"+Name+".cpp")
        if not File.exists():
            continue
        Text = Docs.RemoveComments(Docs.Read(File))
        Labels = sorted(set(re.findall(r'\{"([^"\n]+)",\s*[\w:&]+', Text) + re.findall(r'lua_setglobal\(L,\s*"([^"]+)"', Text) + re.findall(r'lua_pushcfunction\(L,\s*\w+,\s*"([^"]+)"', Text)))
        Related = list((Root / "src/datatypes").glob(Name+".cpp"))
        for Binding in Related:
            Labels += re.findall(r'\{"([^"\n]+)",', Docs.RemoveComments(Docs.Read(Binding)))
        Record = {"Name": Name, "Kind": "Globals" if Name in ("Base", "Require", "Task") else "NativeBinding", "EntryPoint": Open, "Source": File.relative_to(Root).as_posix(), "BindingLabels": sorted(set(Labels)), "SignatureCoverage": "Signatures and overloads require behavioral audit; labels alone do not establish call semantics"}
        Native["Types"].append(Record)
        Outputs[Content/"datatypes"/(Name.lower()+".md")] = Page(Name+" native binding", "\n".join([Status, "", "Registered entry points and binding labels follow. Constructor, instance member and metamethod labels retain their original spelling. Consult the datatype guide for supported usage.", "", "```json", Encode(Record).strip(), "```", "", "[Datatype behavior](../../api/datatypes-and-globals.mdx)"]))
        Index.append(f'- [{Name} native binding](./datatypes/{Name.lower()}.md)')
    Index += ["", "## Native enums", ""]
    for File in sorted((Root/"include/gargantuan").rglob("*.hpp")):
        for Match in re.finditer(r'G_ENUM\(\s*(\w+)\s*,([^;]+)\);', Docs.RemoveComments(Docs.Read(File))):
            Items = []
            Number = -1
            for Item in Match[2].split(","):
                Parts = Item.strip().split("=")
                if len(Parts) == 2:
                    Number = int(Parts[1].strip(), 0)
                else:
                    Number += 1
                Items.append({"Name": Parts[0].strip(), "Value": Number})
            Record = {"Name": Match[1], "Items": Items, "Source": File.relative_to(Root).as_posix()}
            Native["Enums"].append(Record)
            Body = [Status, "", "Values follow the native G_ENUM C++ enum registration, including explicit assignments and implicit integer increments. Project-defined PreRun extensions have their own schema lifetime.", ""]
            for Item in Items:
                Body += [f'## Enum.{Match[1]}.{Item["Name"]}', "", f'Numeric value `{Item["Value"]}`.', ""]
            Outputs[Content/"enums"/(Match[1].lower()+".md")] = Page(Match[1], "\n".join(Body))
            Index.append(f'- [Enum.{Match[1]}](./enums/{Match[1].lower()}.md)')
    Native["Enums"].sort(key=lambda E: E["Name"])
    if len({E["Name"] for E in Native["Enums"]}) != len(Native["Enums"]):
        raise ValueError("Duplicate native enum identity")
    for Name, File in {"EnumItem": "src/datatypes/Enum.cpp", "SignalConnection": "src/datatypes/Signal.cpp", "AnimationTrack": "src/animation/AnimationTrack.cpp", "Rect": "src/datatypes/Rect.cpp"}.items():
        Labels = sorted(set(re.findall(r'\{"([^"\n]+)",', Docs.RemoveComments(Docs.Read(Root/File)))))
        Record = {"Name": Name, "Source": File, "BindingLabels": Labels, "Construction": "Returned native object; no standalone constructor established"}
        Native["Types"].append(Record)
        Outputs[Content/"datatypes"/(Name.lower()+".md")] = Page(Name, Status+"\n\n```json\n"+Encode(Record)+"```\n\n[Datatype guide](../../api/datatypes-and-globals.mdx)")
        Index.append(f'- [{Name}](./datatypes/{Name.lower()}.md)')
    Native["SourceHashes"] = {Path.relative_to(Root).as_posix(): Docs.SourceHash(Path) for Path in sorted((Root/"src/scripting").glob("Lib*.cpp"))}
    Outputs[Root/"docs/src/data/NativeApi.json"] = Encode(Native)
    Outputs[Content/"index.md"] = Page("Creator reference", "\n".join(Index))
    Coverage = {"SourceRevision": Source, "Availability": "Qualified unreleased candidate", "Classes": sum(E["Kind"]=="Class" for E in Api["Classes"]), "Services": sum(E["Kind"]=="Service" for E in Api["Classes"]), **Counts, "NativeBindings": len(Native["Types"]), "Enums": len(Native["Enums"]), "EnumItems": sum(len(E["Items"]) for E in Native["Enums"]), "GeneratedPages": sum(P.suffix==".md" for P in Outputs), "StructuralDeclarationCoverage": "All declared members", "BehavioralCoverage": "Descriptions counted separately; native overloads and missing policy metadata remain backlog", "Examples": "See docs/examples/README.md for execution scope"}
    Coverage["GlobalLibraries"] = sum(E.get("Kind")=="Globals" for E in Native["Types"])
    Coverage["StandaloneExampleFiles"] = len(list((Root/"docs/examples").glob("*.luau")))
    Coverage["ExampleTypeChecking"] = "Not performed against generated Engine definitions"
    Outputs[Root/"docs/src/data/ReferenceCoverage.json"] = Encode(Coverage)
    Outputs[Root/"devdocs/DocumentationReconstruction/CreatorReference.md"] = "# Creator reference coverage\n\n```json\n"+Encode(Coverage)+"```\n\nRegenerate with `python -B tools/docs/GenerateReference.py`. Check drift with `--check`. PublicApi.json remains the declaration catalog. NativeApi.json supplies native registration evidence. Page URLs and qualified member headings are reusable by Studio hover and signature help. Unregistered internal headers are excluded. Native binding labels still need an overload and type audit. Source revision is the qualified candidate, not a released version.\n"
    return Outputs


if __name__ == "__main__":
    Parser = argparse.ArgumentParser()
    Parser.add_argument("--check", action="store_true")
    Args = Parser.parse_args()
    Outputs = Build()
    Stale = [str(P.relative_to(Root)) for P, Text in Outputs.items() if not P.exists() or Docs.Read(P) != Text]
    if Args.check:
        Extra = set(Content.rglob("*.md")) - set(Outputs)
        Stale.extend(str(P.relative_to(Root)) for P in sorted(Extra))
        if Stale:
            raise SystemExit("Reference drift: " + ", ".join(Stale))
    else:
        for Path, Text in Outputs.items():
            Path.parent.mkdir(parents=True, exist_ok=True)
            Path.write_text(Text, encoding="utf-8", newline="\n")
    print(f"[Docs:Reference] {len(Outputs)} generated outputs, {len(Stale)} changed")
