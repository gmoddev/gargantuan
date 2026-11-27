"""Capture or verify an unpacked release against a reviewed, role-specific inventory."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
from runtime_source import Marker, ReadArchive

MicrosoftRuntime = re.compile(
    r"(?:(?:(?:msvcp|msvcr|vcruntime|concrt|vccorlib|vcomp|vcamp|mfc)\d[a-z0-9_]*|"
    r"ucrtbase|api-ms-win-crt-[a-z0-9-]+)\.dll|vc[_-]?redist[._-][a-z0-9._-]+\.exe)", re.IGNORECASE)


def Digest(PathName):
    with PathName.open("rb") as Stream:
        Hasher = hashlib.sha256()
        for Chunk in iter(lambda: Stream.read(1024 * 1024), b""):
            Hasher.update(Chunk)
        return Hasher.hexdigest()


def Inventory(Root):
    if not Root.is_dir() or Root.is_symlink() or getattr(Root.lstat(), "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT:
        raise ValueError("Expected an unpacked release directory")
    Files = {}
    def Fail(Error):
        raise Error
    for PathName, Children, Leaves in os.walk(Root, onerror=Fail, followlinks=False):
        Directory = Path(PathName)
        for Name in (*Children, *Leaves):
            Entry = Directory / Name
            if Entry.is_symlink() or getattr(Entry.lstat(), "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT:
                raise ValueError("Linked package entry: " + str(Entry))
        for Name in Leaves:
            Entry = Directory / Name
            if not Entry.is_file():
                raise ValueError("Non-file package entry: " + str(Entry))
            Files[Entry.relative_to(Root).as_posix()] = Digest(Entry)
    if not Files:
        raise ValueError("Empty release")
    return dict(sorted(Files.items()))


def Verify(Root, Role, Review, RetrievedSource=None):
    Actual = Inventory(Root)
    if Review.get("Role") != Role or Actual != Review.get("Files"):
        raise ValueError("Review role or exact delivered files differ")
    Covered = set()
    for Component in Review.get("Components", []):
        if not all(isinstance(Component.get(K), str) and Component[K].strip()
                   for K in ("Name", "Version", "Origin", "License", "DistributionBasis")):
            raise ValueError("Incomplete component review")
        Paths = Component.get("Files", [])
        if not Paths or not set(Paths) <= Actual.keys() or not set(Component["Notices"]) <= Actual.keys():
            raise ValueError("Component paths or notice destinations differ")
        Covered.update(Paths)
    if Covered != Actual.keys():
        raise ValueError("Unreviewed payload files")
    if Role not in ("Player", "Server"):
        return
    Manifest = json.loads((Root / "game.package.json").read_bytes())
    if Path(Manifest["Player"]).stem.casefold() != ("gargantuan" + Role).casefold():
        raise ValueError("Runtime role differs")
    if Manifest["Player"].lower().endswith(".exe"):
        if any(MicrosoftRuntime.fullmatch(Path(Name).name) for Name in Actual):
            raise ValueError("Windows game contains an app-local Microsoft runtime or installer")
        Prerequisite = (Root / "notices/MSVC-Runtime.txt").read_text(encoding="utf-8")
        if (not re.search(r"requires version 14\.\d+\.\d+", Prerequisite) or
                "https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist/" not in Prerequisite):
            raise ValueError("Missing release-specific Windows prerequisite directions")
    for PathName, Markers in {
        "notices/Gargantuan.txt": ("Matching covered source", "Mozilla Public License Version 2.0", "Exhibit B"),
        "notices/SDL3_ttf.txt": ("FreeType Project LICENSE", "HarfBuzz", "Roboto Regular", "END OF TERMS AND CONDITIONS")
    }.items():
        Text = (Root / PathName).read_text(encoding="utf-8")
        if not all(Marker in Text for Marker in Markers) or "Internal build only." in Text:
            raise ValueError("Incomplete release notice: " + PathName)
    Source = Review["Source"]
    Notice = (Root / "notices/Gargantuan.txt").read_text(encoding="utf-8")
    if (not re.fullmatch("(?:[a-fA-F0-9]{40}|[a-fA-F0-9]{64})", Source.get("Revision", "")) or not Source.get("RetrievalEvidence") or not Source.get("RecipientDirections")
        or Source["RecipientDirections"] not in Notice or Source["Revision"] not in Notice):
        raise ValueError("Missing matched-source recipient retrieval evidence")
    if RetrievedSource is None:
        raise ValueError("Verify the archive or checkout retrieved using the recipient directions")
    if RetrievedSource.is_file():
        if Digest(RetrievedSource) != Source.get("ArchiveSha256"):
            raise ValueError("Retrieved source archive differs from the reviewed archive")
        if Marker in Notice and ReadArchive(Notice, Source["Revision"]) != RetrievedSource.read_bytes():
            raise ValueError("The delivered source differs from the retrieved archive")
    else:
        def Git(*Arguments):
            return subprocess.check_output(["git", "-C", str(RetrievedSource), *Arguments], text=True).strip()
        if (Git("rev-parse", "HEAD") != Source["Revision"] or Git("status", "--porcelain") or
                any(Line.startswith(("-", "+", "U")) for Line in Git("submodule", "status", "--recursive").splitlines())):
            raise ValueError("Retrieved source revision or submodule state differs")


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("action", choices=("capture", "verify"))
    Parser.add_argument("root", type=Path)
    Parser.add_argument("role", choices=("Player", "Server", "Studio", "SDK"))
    Parser.add_argument("--review", type=Path)
    Parser.add_argument("--retrieved-source", type=Path, help="Recipient-retrieved archive or recursive checkout for runtime roles")
    Args = Parser.parse_args()
    if Args.action == "capture":
        print(json.dumps({"Role": Args.role, "Files": Inventory(Args.root), "Components": []}, indent=2))
    else:
        if not Args.review:
            Parser.error("verify requires --review")
        Verify(Args.root, Args.role, json.loads(Args.review.read_bytes()), Args.retrieved_source)
        print("Verified the reviewed release inventory and file identities.")


if __name__ == "__main__":
    try:
        Main()
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as Error:
        sys.exit(str(Error))
