"""Read the separately qualified lifecycle candidate without merging its implementation."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tarfile
import io
import tempfile
import CheckDocumentation as Docs

Source = "61f8520d782ea2810f168fa38d3b28027aff7a89"
Root = Docs.Root
Names = {"ApplicationService", "SessionService", "WorldSaveService", "DataModel", "RunService", "Character"}


from DeclarationExpressions import Expand

def Build():
    Archive = subprocess.check_output(["git", "-C", str(Root), "archive", Source, "assets/classes", "assets/services"])
    with tempfile.TemporaryDirectory(prefix="LifecycleDeclarations-") as Temp:
        Target = Path(Temp)
        with tarfile.open(fileobj=io.BytesIO(Archive)) as Tar:
            for Member in Tar.getmembers():
                if Member.isfile() and Path(Member.name).stem in Names:
                    File = Target/Member.name
                    File.parent.mkdir(parents=True, exist_ok=True)
                    File.write_text(Expand(Tar.extractfile(Member).read().decode()), encoding="utf-8")
        Previous = Docs.Root
        try:
            Docs.Root = Target
            Api = Docs.ExtractApi()
        finally:
            Docs.Root = Previous
    Api.pop("NativeHeaders")
    Api.update(SourceRevision=Source, Availability="Separately qualified unreleased lifecycle candidate; not integrated with spatial source", SourceHashFormat="Hashes below identify normalized extracted declarations; original source identity is the pinned Git revision")
    Outputs = {Root/"docs/src/data/LifecycleApi.json": json.dumps(Api, indent=2)+"\n"}
    Index = ['---', 'title: Standalone lifecycle candidate reference', 'description: Separately pinned menu, pause, session and world-slot APIs.', '---', '', f'Qualified lifecycle candidate `{Source}`. These declarations belong to a separate source tree from the spatial reference.', '']
    for Entry in Api["Classes"]:
        Name = Entry["Name"]
        Index.append(f'- [{Name}](./{Name.lower()}.md)')
        Body = ['---', f'title: {Name} lifecycle candidate', f'description: Candidate-only reference for {Name}.', '---', '', f'Source `{Source}`. Qualified unreleased lifecycle candidate.', '', Entry['Description'], '', '[Lifecycle usage guide](../../guides/standalone-application.mdx)', '']
        for Kind in ("Properties", "Methods", "Signals"):
            for Member in Entry[Kind]:
                Body += [f'## {Name}.{Member["Name"]}', '', Member.get('Description','Description unavailable.'), '', '```json', json.dumps(Member, indent=2), '```', '']
        Outputs[Docs.Content/"lifecycle-reference"/(Name.lower()+".md")] = "\n".join(Body)+"\n"
    Outputs[Docs.Content/"lifecycle-reference/index.md"] = "\n".join(Index)+"\n"
    return Outputs


if __name__ == "__main__":
    Parser = argparse.ArgumentParser()
    Parser.add_argument("--check", action="store_true")
    Args = Parser.parse_args()
    for File, Text in Build().items():
        if Args.check:
            if not File.exists() or Docs.Read(File) != Text: raise SystemExit("Lifecycle reference drift: "+str(File))
        else:
            File.parent.mkdir(parents=True, exist_ok=True)
            File.write_text(Text, encoding="utf-8", newline="\n")
    print("[Docs:Lifecycle] Separate pinned candidate reference verified")
