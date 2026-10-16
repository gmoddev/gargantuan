"""Versioned owned-source inventory; external/generated/build inputs stay separate."""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import zipfile

FORMAT = "F1_OWNED_TREE_V2"
INVENTORY = "_f1-source-inventory.json"
MAX_FILES, MAX_BYTES, MAX_FILE_BYTES = 10000, 128 * 1024 * 1024, 32 * 1024 * 1024
# Deliberately includes every reviewed top-level owned tree, rather than a
# hand-maintained include closure that silently misses a new implementation.
ROOTS = frozenset((".clang-format", ".clangd", ".github", ".gitignore", ".gitmodules",
    ".justfile", ".luaurc", ".vscode", ".zed", "AGENTS.md", "AICONTEXT.md", "CLAUDE.md",
    "CMakeLists.txt", "CONTRIBUTING.md", "GEMINI.md", "KNOWN_ISSUES.md", "LICENSE.md",
    "README.md", "assets", "cmake", "devdocs", "docs", "include", "lest.toml",
    "rokit.toml", "samples", "src", "stylua.toml", "tests", "tools", "vendor"))
EXCLUSIONS = ["gitlink dependency contents (exact commits recorded)",
              "fetched dependencies and toolchains (recipes/pins recorded)",
              "ignored/generated source and build configuration/output",
              "binary, DLL and deployed runtime hashes (derive from qualified build)"]
ALIASES = {Name: "docs/src/content/docs/meta/agents.mdx" for Name in ("AGENTS.md", "CLAUDE.md", "GEMINI.md")}
ALIASES.update({"CONTRIBUTING.md": "docs/src/content/docs/developing/contributing-to-gargantuan.mdx",
                "LICENSE.md": "docs/src/content/docs/meta/license.mdx"})


def Revision(Value):
    if not isinstance(Value, str) or not re.fullmatch(r"[0-9a-f]{40}", Value):
        raise ValueError("expected an explicit full source commit")
    return Value


def SafePath(Name):
    if (not isinstance(Name, str) or not Name or "\\" in Name or ":" in Name or
            any(ord(C) < 32 or C in '<>|?*' for C in Name) or Name.startswith("/") or
            any(Part in ("", ".", "..") or Part.endswith((".", " ")) for Part in Name.split("/")) or
            PurePosixPath(Name).parts[0] not in ROOTS):
        raise ValueError("unreviewed/unsafe source path: " + str(Name))
    Devices = {"CON", "PRN", "AUX", "NUL", *("COM" + str(I) for I in range(1, 10)),
               *("LPT" + str(I) for I in range(1, 10))}
    if any(Part.split(".")[0].upper() in Devices for Part in Name.split("/")):
        raise ValueError("device source path")
    return Name


def Git(Root, *Args, Input=None):
    return subprocess.run(["git", *Args], cwd=Root, input=Input, check=True,
                          capture_output=True, timeout=60).stdout


def Canonical(Value):
    return (json.dumps(Value, sort_keys=True, separators=(",", ":")) + "\n").encode()


def ReadRevision(Root, Expected):
    Revision(Expected)
    if Git(Root, "rev-parse", Expected + "^{commit}").decode().strip() != Expected:
        raise ValueError("source revision is not the expected commit")
    Files, Submodules, Seen, Total = [], {}, set(), 0
    for Row in Git(Root, "ls-tree", "-r", "-l", "-z", Expected).split(b"\0"):
        if not Row:
            continue
        Metadata, Name = Row.split(b"\t", 1)
        Mode, Kind, Object, Size = Metadata.decode("ascii").split()
        Name = SafePath(Name.decode("utf-8"))
        if Name.casefold() in Seen:
            raise ValueError("duplicate/case-aliased source path")
        Seen.add(Name.casefold())
        if Mode == "160000" and Kind == "commit":
            Submodules[Name] = Revision(Object)
            if len(Submodules) > 128:
                raise ValueError("too many dependency pins")
            continue
        if (Mode not in ("100644", "100755") and not (Mode == "120000" and Name in ALIASES)) or Kind != "blob":
            raise ValueError("source redirects/symlinks are not supported: " + Name)
        Size = int(Size)
        Total += Size
        if Size > MAX_FILE_BYTES or Total > MAX_BYTES or len(Files) >= MAX_FILES:
            raise ValueError("owned source inventory exceeds reviewed bounds")
        Files.append({"Path": Name, "GitBlob": Object, "Mode": Mode, "Bytes": Size})
    if not Files:
        raise ValueError("empty source inventory")
    Raw = Git(Root, "cat-file", "--batch", Input="".join(F["GitBlob"] + "\n" for F in Files).encode())
    Offset, Sources = 0, {}
    for File in Files:
        End = Raw.index(b"\n", Offset)
        if Raw[Offset:End] != f'{File["GitBlob"]} blob {File["Bytes"]}'.encode():
            raise ValueError("git batch object mismatch")
        Offset = End + 1
        Data = Raw[Offset:Offset + File["Bytes"]]
        if File["Mode"] == "120000" and Data != ALIASES[File["Path"]].encode():
            raise ValueError("reviewed documentation alias target changed")
        Offset += File["Bytes"]
        if Raw[Offset:Offset + 1] != b"\n":
            raise ValueError("truncated git object")
        Offset += 1
        File["SHA256"] = hashlib.sha256(Data).hexdigest().upper()
        Sources[File["Path"]] = Data
    Manifest = {"Format": FORMAT, "BaseHead": Expected, "Files": sorted(Files, key=lambda F: F["Path"]),
                "Submodules": Submodules, "Exclusions": EXCLUSIONS}
    return Manifest, Sources


def Create(Root, Expected, Output):
    Manifest, Sources = ReadRevision(Root, Expected)
    Output = Path(Output)
    Output.parent.mkdir(parents=True, exist_ok=True)
    # Exclusive creation: historical archives and candidate receipts never mutate.
    with Output.open("xb") as Stream:
        with zipfile.ZipFile(Stream, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as Archive:
            Sources[INVENTORY] = Canonical(Manifest)
            for Name in sorted(Sources):
                Entry = zipfile.ZipInfo(Name, (1980, 1, 1, 0, 0, 0))
                Archive.writestr(Entry, Sources[Name], compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    return hashlib.sha256(Output.read_bytes()).hexdigest().upper()


def VerifyArchive(ArchivePath, Expected):
    """Endpoint verification after the caller independently verifies its ZIP pin."""
    Revision(Expected)
    with zipfile.ZipFile(ArchivePath) as Archive:
        Entries = Archive.infolist()
        if (len(Entries) > MAX_FILES + 1 or sum(E.file_size for E in Entries) > MAX_BYTES + MAX_FILE_BYTES or
                any(E.file_size > MAX_FILE_BYTES or E.flag_bits & 1 or E.is_dir() or
                    (E.external_attr >> 16) & 0o170000 == 0o120000 for E in Entries)):
            raise ValueError("candidate archive exceeds reviewed bounds")
        EncodedManifest = Archive.read(INVENTORY)
        Manifest = json.loads(EncodedManifest)
        if not isinstance(Manifest, dict) or Canonical(Manifest) != EncodedManifest:
            raise ValueError("noncanonical/malformed source inventory")
        if (Manifest.get("Format") != FORMAT or Manifest.get("BaseHead") != Expected or
                Manifest.get("Exclusions") != EXCLUSIONS or not isinstance(Manifest.get("Files"), list) or
                not isinstance(Manifest.get("Submodules"), dict) or len(Manifest["Submodules"]) > 128):
            raise ValueError("candidate source format/revision mismatch")
        Files, Seen, Total = Manifest["Files"], set(), 0
        if not Files or len(Files) > MAX_FILES:
            raise ValueError("invalid source inventory count")
        for File in Files:
            Name = SafePath(File["Path"])
            if (Name.casefold() in Seen or (File.get("Mode") not in ("100644", "100755") and
                    not (File.get("Mode") == "120000" and Name in ALIASES)) or
                    not re.fullmatch(r"[0-9a-f]{40}", str(File.get("GitBlob"))) or
                    not re.fullmatch(r"[0-9A-F]{64}", str(File.get("SHA256"))) or
                    type(File.get("Bytes")) is not int or not 0 <= File["Bytes"] <= MAX_FILE_BYTES):
                raise ValueError("malformed source inventory entry")
            Seen.add(Name.casefold())
            Total += File["Bytes"]
        for Name, Pin in Manifest["Submodules"].items():
            SafePath(Name)
            Revision(Pin)
            if Name.casefold() in Seen:
                raise ValueError("submodule/source path collision")
            Seen.add(Name.casefold())
        if Total > MAX_BYTES or [E.filename for E in Entries] != sorted([INVENTORY] + [F["Path"] for F in Files]):
            raise ValueError("candidate archive path list mismatch")
        for File in Files:
            Data = Archive.read(File["Path"])
            if File["Mode"] == "120000" and Data != ALIASES[File["Path"]].encode():
                raise ValueError("reviewed documentation alias target changed")
            Blob = hashlib.sha1(b"blob " + str(len(Data)).encode() + b"\0" + Data).hexdigest()
            if (len(Data) != File["Bytes"] or hashlib.sha256(Data).hexdigest().upper() != File["SHA256"] or
                    Blob != File["GitBlob"]):
                raise ValueError("candidate source bytes mismatch: " + File["Path"])
        return Manifest


def VerifyPinned(Root, ArchivePath, Expected):
    Manifest = VerifyArchive(ArchivePath, Expected)
    Pinned, _ = ReadRevision(Root, Expected)
    if Manifest != Pinned:
        raise ValueError("candidate inventory does not match expected Git revision")
    return Manifest


def VerifyTree(Root, ArchivePath, Expected):
    Root = Path(Root).resolve()
    Manifest = VerifyPinned(Root, ArchivePath, Expected)
    if Git(Root, "rev-parse", "HEAD").decode().strip() != Expected:
        raise ValueError("build source HEAD differs from candidate")
    with zipfile.ZipFile(ArchivePath) as Archive:
        for File in Manifest["Files"]:
            PathValue = Root / File["Path"]
            if File["Mode"] == "120000":
                Actual = os.readlink(PathValue).replace("\\", "/").encode() if PathValue.is_symlink() else PathValue.read_bytes()
                if Actual != Archive.read(File["Path"]):
                    raise ValueError("build documentation alias mismatch")
                continue
            Parents = [Root.joinpath(*PurePosixPath(File["Path"]).parts[:I])
                       for I in range(1, len(PurePosixPath(File["Path"]).parts) + 1)]
            if (any(P.is_symlink() or getattr(P, "is_junction", lambda: False)() for P in Parents) or
                    not PathValue.resolve().is_relative_to(Root)):
                raise ValueError("build source path redirect")
            Actual, Pinned = PathValue.read_bytes(), Archive.read(File["Path"])
            if Actual == Pinned:
                continue
            if b"\0" in Actual or b"\0" in Pinned or Actual.replace(b"\r\n", b"\n") != Pinned.replace(b"\r\n", b"\n"):
                raise ValueError("build source mismatch: " + File["Path"])
    for Name, Pin in Manifest["Submodules"].items():
        Dependency = Root / Name
        if (Dependency.is_symlink() or not Dependency.resolve().is_relative_to(Root) or
                Git(Dependency, "rev-parse", "HEAD").decode().strip() != Pin or
                Git(Dependency, "status", "--porcelain", "--untracked-files=no")):
            raise ValueError("submodule pin/working-tree mismatch: " + Name)
    return Manifest
