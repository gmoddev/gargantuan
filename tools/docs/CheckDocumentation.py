"""Repository documentation checks; stdlib only, no Engine or network execution."""
from __future__ import annotations

import argparse
import hashlib
import html
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
from urllib.parse import unquote, urlsplit

Root = pathlib.Path(__file__).resolve().parents[2]
Content = Root / "docs/src/content/docs"
Artifacts = Root / "devdocs/DocumentationReconstruction"
ApiPath = Root / "docs/src/data/PublicApi.json"
BaseSource = "2a4c1d2b03e40178b97334bb6f1f2eeedd51fe9f"
RoutingAliases = {"AGENTS.md", "CLAUDE.md", "GEMINI.md", "CONTRIBUTING.md", "LICENSE.md"}
Categories = {
    "Current": "Current and authoritative",
    "Incomplete": "Current but incomplete",
    "Outdated": "Outdated and requiring reconstruction",
    "Historical": "Historical evidence requiring preservation",
    "Superseded": "Superseded",
    "Duplicated": "Duplicated",
    "Unverified": "Unverified",
}


def Git(*Arguments: str) -> str:
    return subprocess.check_output(["git", "-C", str(Root), *Arguments], text=True, encoding="utf-8")


def Documents() -> list[pathlib.Path]:
    Paths = Git("ls-files", "--cached", "-z").split("\0")
    Paths += [Path for Path in Git("ls-files", "--others", "--exclude-standard", "-z").split("\0")
              if Path.startswith(("docs/", "devdocs/", "tools/docs/"))]
    return sorted({Root / Path for Path in Paths if Path and (Root / Path).is_file()
                   and ((Root / Path).suffix.lower() in {".md", ".mdx", ".rst", ".adoc"}
                        or ((Root / Path).suffix.lower() == ".txt" and (Root / Path).name != "CMakeLists.txt")
                        or (Root / Path).name.lower().startswith(("readme", "license", "authors", "copying")))}, key=lambda Path: Path.as_posix())


def Read(Path: pathlib.Path) -> str:
    return Path.read_text(encoding="utf-8-sig")


def SourceHash(Path: pathlib.Path) -> str:
    # Git's text checkout policy can use CRLF on Windows. Hash canonical text,
    # not platform-specific worktree bytes; qualification binary hashes are separate.
    return hashlib.sha256(Read(Path).encode("utf-8")).hexdigest()


def AliasTarget(Path: pathlib.Path) -> str:
    # Git materializes mode-120000 files as symlinks on Linux and as their
    # one-line link text under Windows core.symlinks=false.
    return Path.readlink().as_posix() if Path.is_symlink() else Read(Path).strip()


def WithoutCode(Text: str) -> str:
    Text = re.sub(r"(?ms)^\s*(`{3,}|~{3,})[^\n]*\n.*?^\s*\1\s*$", "", Text)
    return re.sub(r"`[^`\n]*`", "", Text)


def Anchors(Text: str) -> set[str]:
    Result = set(re.findall(r'(?:id|name)=[\"\']([^\"\']+)', Text))
    Counts: dict[str, int] = {}
    for Heading in re.findall(r"(?m)^#{1,6}\s+(.+)", Text):
        Heading = re.sub(r"\[([^]]+)\]\([^)]*\)", r"\1", Heading)
        Heading = re.sub(r"<[^>]*>", "", Heading).strip().rstrip("#").strip().lower()
        Slug = re.sub(r"[^\w\- ]", "", html.unescape(Heading)).replace(" ", "-")
        Count = Counts.get(Slug, 0)
        Counts[Slug] = Count + 1
        Result.add(Slug if not Count else f"{Slug}-{Count}")
    return Result


def Resolve(Document: pathlib.Path, Target: str) -> pathlib.Path | None:
    Path = unquote(urlsplit(Target).path)
    if not Path:
        return Document
    if Path.startswith("/"):
        Path = re.sub(r"^/gargantuan(?:/|$)", "/", Path)
        Candidate = Content / Path.lstrip("/")
    else:
        Candidate = Document.parent / Path
    # Existing documents mix checkout-relative links and route-relative links
    # (a published page has its own trailing-slash directory). Support both.
    Candidates = [Candidate]
    if Document.is_relative_to(Content) and not Path.startswith("/"):
        Route = Document.parent if Document.stem == "index" else Document.with_suffix("")
        Candidates.append(Route / Path)
    # A virtual page directory does not exist on disk. Normalize '..' before
    # asking the OS about it; POSIX otherwise tries to traverse that missing
    # directory while Windows normalizes the same path implicitly.
    Candidates = [Candidate.resolve() for Candidate in Candidates]
    for Candidate in Candidates:
        for Option in (Candidate, Candidate.with_suffix(".mdx"), Candidate.with_suffix(".md"),
                       Candidate / "index.mdx", Candidate / "index.md"):
            if Option.is_file():
                return Option.resolve()
    for Candidate in Candidates:
        if Candidate.is_dir():
            return Candidate.resolve()
    return None


def Links(Document: pathlib.Path) -> list[dict[str, str]]:
    if Document.relative_to(Root).as_posix() in RoutingAliases:
        Target = AliasTarget(Document)
        if not (Document.parent / Target).is_file():
            return [{"Path": Document.relative_to(Root).as_posix(), "Target": Target, "Reason": "Missing canonical routing target"}]
        # The target is checked in its own directory, not relative to this alias.
        return []
    Text = WithoutCode(Read(Document))
    Targets = re.findall(r"\]\(<?([^\s)>]+)>?(?:\s+[\"'][^\n]*?[\"'])?\)", Text)
    Targets += re.findall(r"(?m)^\s*\[[^]]+\]:\s*<?([^\s>]+)>?", Text)
    Findings = []
    for Target in Targets:
        Url = urlsplit(Target)
        if Url.scheme or Target.startswith("//"):
            continue
        Destination = Resolve(Document, Target)
        Reason = "Missing target" if Destination is None else ""
        if Destination and Url.fragment and Destination.suffix in {".md", ".mdx"}:
            if unquote(Url.fragment) not in Anchors(Read(Destination)):
                Reason = "Missing heading anchor"
        if Reason:
            Findings.append({"Path": Document.relative_to(Root).as_posix(), "Target": Target, "Reason": Reason})
    return Findings


def SplitExpressions(Text: str) -> list[str]:
    """Split the declarative Luau subset without executing project code."""
    Result, Start, Depth, Index = [], 0, 0, 0
    while Index < len(Text):
        Character = Text[Index]
        if Character in "\"'`":
            Quote = Character
            Index += 1
            while Index < len(Text):
                if Text[Index] == "\\":
                    Index += 2
                    continue
                if Text[Index] == Quote:
                    break
                Index += 1
        elif Character in "({[":
            Depth += 1
        elif Character in ")}]":
            Depth -= 1
        elif Character == "," and Depth == 0:
            Result.append(Text[Start:Index].strip())
            Start = Index + 1
        Index += 1
    if Text[Start:].strip():
        Result.append(Text[Start:].strip())
    return Result


def RemoveComments(Text: str) -> str:
    Pattern = r'("(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\')|--\[\[.*?\]\]|--[^\n]*'
    return re.sub(Pattern, lambda Match: Match[1] or "", Text, flags=re.S)


def Fields(Text: str) -> dict[str, str]:
    Text = Text.strip()
    if Text.startswith("{") and Text.endswith("}"):
        Text = Text[1:-1]
    Result = {}
    for Expression in SplitExpressions(Text):
        Match = re.match(r"^([A-Za-z_][A-Za-z_0-9]*)\s*=\s*(.*)$", Expression, re.S)
        if not Match:
            raise ValueError(f"Unsupported declaration expression: {Expression[:100]}")
        if Match[1] in Result:
            raise ValueError(f"Duplicate declaration key: {Match[1]}")
        Result[Match[1]] = Match[2].strip()
    return Result


def Value(Expression: str) -> object:
    if Expression in {"true", "false"}:
        return Expression == "true"
    if Expression.startswith('"') and Expression.endswith('"'):
        return json.loads(Expression, strict=False)
    return re.sub(r"\s+", " ", Expression).strip()


def ExtractApi() -> dict:
    Classes = []
    for Folder in (Root / "assets/classes", Root / "assets/services"):
        for Path in sorted(Folder.glob("*.luau"), key=lambda Item: Item.as_posix()):
            if Path.name.startswith("."):
                continue
            Text = RemoveComments(Read(Path))
            Match = re.search(r"return\s+c\(\s*(\{.*\})\s*\)\s*$", Text, re.S)
            if not Match:
                raise ValueError(f"Unsupported class declaration format: {Path}")
            Definition = Fields(Match[1])
            Record = {"Name": Value(Definition["name"]), "Kind": "Service" if Folder.name == "services" else "Class",
                      "Source": Path.relative_to(Root).as_posix(), "SourceSha256": SourceHash(Path),
                      "Description": Value(Definition.get("description", '""')),
                      "Extends": Value(Definition.get("extends", '"Instance"')) if Definition["name"] != '"Instance"' else None,
                      "Abstract": Value(Definition.get("abstract", "false")),
                      "ServicePolicy": Value(Definition.get("servicePolicy", '"Default"')),
                      "ImplementationStatus": "Declared; consult native binding, capability checks and tests for behavior"}
            for Section in ("properties", "methods", "signals"):
                Members = []
                for Name, Body in Fields(Definition.get(Section, "{}")).items():
                    Entry = {"Name": Name}
                    for Key, Expression in Fields(Body).items():
                        Entry[Key[0].upper() + Key[1:]] = Value(Expression)
                    Members.append(Entry)
                Record[Section.capitalize()] = sorted(Members, key=lambda Item: Item["Name"])
            Classes.append(Record)
    Headers = [{"Path": Path.relative_to(Root).as_posix(),
                "Types": sorted(set(re.findall(r"(?m)^\s*(?:class|struct|enum class)\s+([A-Za-z_]\w*)", Read(Path))))}
               for Path in sorted((Root / "include/gargantuan").rglob("*.hpp"), key=lambda Item: Item.as_posix()) if "generated" not in Path.parts]
    return {"SchemaVersion": 1, "SourceHashFormat": "SHA-256 of UTF-8 text with LF newlines and without BOM; not a binary qualification hash",
            "Coverage": "All tracked native class/service Luau declarations; native headers indexed separately. Not proof of implementation or capability grants.",
            "Classes": sorted(Classes, key=lambda Item: Item["Name"]), "NativeHeaders": Headers}


def Classify(Path: str, Text: str) -> tuple[str, str]:
    if Path.startswith(("devdocs/DocumentationReconstruction/", "tools/docs/")):
        return "Current", "Maintain campaign provenance, independent review and integrity contract"
    if Path.startswith("docs/adr/"):
        if re.search(r"^status:\s*historical", Text, re.M):
            return "Historical", "Preserve superseded proposal and follow accepted D03 amendments"
        return "Current", "Preserve accepted decision authority and explicit amendments; acceptance is not implementation proof"
    if Path in RoutingAliases:
        return "Duplicated", "Preserve routing alias to canonical policy/license"
    if Path.startswith(("devdocs/Validation/", "cmake/notices/", "vendor/")) or re.search(
            r"(Validation|Qualification|Preflight|Attribution|Evidence|Checkpoint|Audit|WorkerCoordination|Proof|GateReview|Stage1Plan)", Path):
        return "Historical", "Preserve decision/evidence, source identity and original outcome; follow current guide for behavior"
    if Path in {"devdocs/README.md", "devdocs/CurrentArchitecture/SubsystemAudit.md", "devdocs/CurrentArchitecture/ImplementationGaps.md",
                "devdocs/CurrentArchitecture/SecurityAudit.md", "devdocs/CurrentArchitecture/EditorViewportPresentation2.md"}:
        return "Historical", "Preserve dated snapshot; route readers to reconstructed current references"
    if Path.startswith("devdocs/FutureArchitecture/") or Path.endswith(("/future-architecture.mdx", "/future-architecture-concerns.mdx", "/roadmap.mdx", "/networking-architecture.mdx")):
        return "Unverified", "Retain as non-current design input; accepted decisions constrain design, not implementation proof"
    if Path.startswith("docs/src/content/docs/") or Path in {"README.md", "devdocs/Compiling.md", "AICONTEXT.md", "docs/architecture/README.md", "docs/invariants/Core.md", "docs/AGENTS.md"}:
        return "Current", "Maintain source-linked developer entry point and validate links/examples"
    if Path.startswith(("tests/", "tools/physical-qualifier/")):
        return "Incomplete", "Retain exact qualification/tool contract; scope certification to recorded source and evidence"
    if Path.startswith("samples/") or Path == "docs/examples/README.md":
        return "Current", "Maintain setup, authority and sample-specific execution limits"
    if Path == "KNOWN_ISSUES.md":
        return "Incomplete", "Maintain verified scoped issues; historical updates do not override later closure"
    if Path.startswith("devdocs/CurrentArchitecture/"):
        if re.search(r"^status:\s*historical", Text, re.M):
            return "Historical", "Preserve historical source-specific record"
        return "Unverified", "Retain subsystem contract; reverify complete prose before treating all claims as current"
    return "Unverified", "Inspect owning source before promoting to current authority"


def Inventory(Paths: list[pathlib.Path]) -> dict:
    Records = []
    for Path in Paths:
        Relative = Path.relative_to(Root).as_posix()
        Alias = AliasTarget(Path) if Relative in RoutingAliases else None
        Text = Alias if Alias is not None else Read(Path)
        Title = re.search(r"(?m)^title:\s*(.+)$|^#\s+(.+)$", Text)
        Status, Action = Classify(Relative, Text)
        Sources = sorted(set(re.findall(r"(?:include/gargantuan|src|assets|tests|tools|cmake)/[\w./-]+", Text)))
        Last = Git("log", "-1", "--format=%H%x09%cI%x09%s", BaseSource, "--", Relative).strip()
        Parts = Last.split("\t", 2) if Last else []
        Verified = re.search(r"(?m)^(?:last_verified|reviewed):\s*(\d{4}-\d{2}-\d{2})", Text)
        Records.append({"Path": Relative, "Purpose": f"Routing alias to {Alias}" if Alias is not None else next((Group for Group in Title.groups() if Group), Relative) if Title else Text.splitlines()[0] if Text else "Empty document",
                        "LastMeaningfulUpdate": {"Date": Verified[1], "Basis": "Document-declared verification/review date; scope remains revision-specific"} if Verified else None,
                        "LastPathCommit": {"Sha": Parts[0], "Date": Parts[1], "Subject": Parts[2]} if len(Parts) == 3 else None,
                        "RelevantSourceSubsystem": [f"Canonical document: {Alias}"] if Alias is not None else Sources[:16] or ["Owning source not established; consult traceability matrix"],
                        "Status": Status, "StatusLabel": Categories[Status], "ProposedAction": Action,
                        "ObsoleteTermMentions": sorted(set(re.findall(r"Avalonia|OpenGL|Filament|InstanceClass|compat-roblox|Chromium", Text, re.I)))})
    return {"SchemaVersion": 1, "IntegratedEngineSource": BaseSource,
            "ProvenanceNote": "Last path commit is not proof of meaningful architectural review. LastMeaningfulUpdate uses a declared review date where available; unknown values remain null. New campaign files have no baseline path commit.",
            "Scope": "Every repository-tracked document, license and README plus intended new documentation; Git submodule internals excluded and retain their own ownership.",
            "ClassificationNote": "Current applies to scoped developer guidance, not exhaustive API completeness. Unverified is deliberate; directory names and accepted future designs do not prove implementation. Superseded and outdated are available categories, not assumed from age.",
            "Categories": Categories, "Documents": Records}


def ApiReferences(Paths: list[pathlib.Path], Api: dict) -> list[dict[str, str]]:
    Classes = {Definition["Name"]: Definition for Definition in Api["Classes"]}
    Findings = []

    def HasMember(ClassName: str, Member: str, Seen: set[str]) -> bool:
        if ClassName not in Classes or ClassName in Seen:
            return False
        Definition = Classes[ClassName]
        return any(Item["Name"] == Member for Section in ("Properties", "Methods", "Signals") for Item in Definition[Section]) or HasMember(Definition["Extends"], Member, Seen | {ClassName})

    for Path in Paths:
        if not Path.is_relative_to(Content / "api") and not Path.is_relative_to(Root / "docs/examples"):
            continue
        Text = Read(Path)
        # Dot access can name a dynamically authored child (Storage.Ping).
        # Only class-qualified method references are mechanically unambiguous.
        for ClassName, Member in re.findall(r"\b([A-Z][A-Za-z0-9_]*):([A-Za-z_][A-Za-z0-9_]*)", Text):
            if ClassName in Classes and not (ClassName == "Instance" and Member == "new") and not HasMember(ClassName, Member, set()):
                Findings.append({"Path": Path.relative_to(Root).as_posix(), "Reason": "Unknown declared API member", "Target": f"{ClassName}.{Member}"})
        for Pattern in (r'Instance\.new\("([A-Za-z0-9_]+)"\)', r'game:GetService\("([A-Za-z0-9_]+)"\)'):
            for Name in re.findall(Pattern, Text):
                if Name not in Classes:
                    Findings.append({"Path": Path.relative_to(Root).as_posix(), "Reason": "Unknown declared class/service", "Target": Name})
    return Findings


def WriteJson(Path: pathlib.Path, Data: object) -> None:
    Path.parent.mkdir(parents=True, exist_ok=True)
    Path.write_text(json.dumps(Data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")


def Check(Arguments: argparse.Namespace) -> int:
    Paths = Documents()
    Api = ExtractApi()
    if Arguments.update_api:
        WriteJson(ApiPath, Api)
    if Arguments.inventory:
        WriteJson(Artifacts / "Inventory.json", Inventory(Paths))
    Errors, Legacy = [], []
    for Path in Paths:
        Relative = Path.relative_to(Root).as_posix()
        Status, _ = Classify(Relative, Read(Path))
        Findings = Links(Path)
        (Errors if Status in {"Current", "Incomplete"} else Legacy).extend(Findings)
        if Status == "Current":
            Text = WithoutCode(Read(Path))
            for Match in re.finditer(r"(?i)(?:is a separate C#/Avalonia|C#/Avalonia (?:editor|authoring)|Most games.*will run fine|documentation website is empty)", Text):
                Errors.append({"Path": Relative, "Reason": "Obsolete current architecture assertion", "Target": Match[0]})
            for Source in re.findall(r"`((?:src|include/gargantuan|assets|tests|tools|cmake|\.github)/[^`\n]+)`", Read(Path)):
                Source = Source.split("::", 1)[0]
                if not Relative.startswith("devdocs/DocumentationReconstruction/") and not re.search(r"[<>*{}| ,]", Source) and not (Root / Source.rstrip("/")).exists():
                    Errors.append({"Path": Relative, "Reason": "Stale source path", "Target": Source})
    Configuration = Read(Root / "docs/astro.config.mjs")
    for Slug in re.findall(r'slug:\s*["\']([^"\']+)', Configuration):
        if not any((Content / f"{Slug}{Suffix}").is_file() for Suffix in (".mdx", ".md", "/index.mdx", "/index.md")):
            Errors.append({"Path": "docs/astro.config.mjs", "Reason": "Missing navigation target", "Target": Slug})
    if not ApiPath.exists() or json.loads(Read(ApiPath)) != Api:
        Errors.append({"Path": ApiPath.relative_to(Root).as_posix(), "Reason": "Declaration catalog drift; run --update-api", "Target": "assets/classes and assets/services"})
    Errors.extend(ApiReferences(Paths + list((Root / "docs/examples").glob("*.luau")), Api))
    TracePath = Artifacts / "Traceability.json"
    if TracePath.exists():
        for Claim in json.loads(Read(TracePath))["Claims"]:
            for Target in [Claim["Document"], *Claim["Source"], *Claim["Tests"]]:
                if not (Root / Target).exists():
                    Errors.append({"Path": TracePath.relative_to(Root).as_posix(), "Reason": "Missing traceability source", "Target": Target})
    else:
        Errors.append({"Path": "Traceability.json", "Reason": "Missing source traceability", "Target": "Traceability.json"})
    Manifest = Artifacts / "Inventory.json"
    if Manifest.exists():
        Expected = {Path.relative_to(Root).as_posix() for Path in Paths}
        Records = {Record["Path"]: Record for Record in json.loads(Read(Manifest))["Documents"]}
        Actual = set(Records)
        for Missing in sorted(Expected ^ Actual):
            Errors.append({"Path": Missing, "Reason": "Inventory coverage drift; run --inventory", "Target": "Inventory.json"})
        for Path in Paths:
            Relative = Path.relative_to(Root).as_posix()
            if Relative in Records and Records[Relative]["Status"] != Classify(Relative, Read(Path))[0]:
                Errors.append({"Path": Relative, "Reason": "Inventory classification drift; run --inventory", "Target": "Inventory.json"})
    else:
        Errors.append({"Path": "Inventory.json", "Reason": "Missing document inventory", "Target": "--inventory"})
    if Arguments.luau_compile:
        Compiler = shutil.which(Arguments.luau_compile) or Arguments.luau_compile
        Examples = [(Path.relative_to(Root).as_posix(), Read(Path)) for Path in sorted((Root / "docs/examples").glob("*.luau"))]
        for Path in Paths:
            if Classify(Path.relative_to(Root).as_posix(), Read(Path))[0] != "Current":
                continue
            for Index, Match in enumerate(re.finditer(r"(?ms)^```(?:luau|lua)\s*\n(.*?)^```\s*$", Read(Path)), 1):
                Examples.append((f"{Path.relative_to(Root).as_posix()} example {Index}", Match[1]))
        with tempfile.TemporaryDirectory(prefix="GargantuanDocs-") as Temporary:
            for Index, (Name, Source) in enumerate(Examples):
                Candidate = pathlib.Path(Temporary) / f"Example{Index}.luau"
                Candidate.write_text(Source, encoding="utf-8")
                Process = subprocess.run([Compiler, "--null", str(Candidate)], capture_output=True, text=True)
                if Process.returncode:
                    Errors.append({"Path": Name, "Reason": "Luau compilation failed", "Target": Process.stderr or Process.stdout})
        print(f"[Docs:Examples] compiled {len(Examples)} standalone and current-document Luau examples")
    ReportPath = Artifacts / "LinkFindings.json"
    if Arguments.report:
        WriteJson(ReportPath, {"LegacyFindings": Legacy, "CurrentFindings": Errors})
    elif not ReportPath.exists() or json.loads(Read(ReportPath)).get("LegacyFindings") != Legacy:
        Errors.append({"Path": "LinkFindings.json", "Reason": "Historical link findings changed; repair links or review --report update", "Target": "non-current documents"})
        for Finding in Legacy:
            print(f"[Docs:HistoricalLink] {Finding['Path']}: {Finding['Reason']}: {Finding['Target']}")
    for Error in Errors:
        print(f"[Docs:Integrity] {Error['Path']}: {Error['Reason']}: {Error['Target']}")
    print(f"[Docs:Integrity] {len(Paths)} documents, {len(Api['Classes'])} declarations, {len(Errors)} errors, {len(Legacy)} retained non-current link findings")
    return 1 if Errors else 0


if __name__ == "__main__":
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--update-api", action="store_true", help="Regenerate deterministic declaration catalog")
    Parser.add_argument("--inventory", action="store_true", help="Refresh complete classified document inventory")
    Parser.add_argument("--report", action="store_true", help="Write current and historical link findings")
    Parser.add_argument("--luau-compile", help="Path or command for a real Luau compiler; examples compile without running")
    try:
        sys.exit(Check(Parser.parse_args()))
    except (ValueError, OSError, subprocess.SubprocessError) as Error:
        print(f"[Docs:Integrity] {Error}", file=sys.stderr)
        sys.exit(1)
