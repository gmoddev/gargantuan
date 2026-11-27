"""Create a recipient ZIP of reviewed Engine runtime sources from an immutable Git tree."""
import argparse
import configparser
import hashlib
import io
import subprocess
import zipfile
from pathlib import Path

from runtime_source import CheckArchive


# Engine runtime and its build inputs. Branded media, game projects, CI captures,
# repository administration and other products are outside this source package.
Roots = ("src/", "include/", "cmake/", "assets/classes/", "assets/services/", "assets/runtime/",
         "assets/shaders/", "assets/filament/", "assets/stdlib/", "vendor/manifold/",
         "tools/batteries/", "tools/libs/", "tools/distribution/")
Singles = {".gitmodules", ".justfile", ".luaurc", "CMakeLists.txt", "rokit.toml", "tools/.config.luau",
           "tools/classgen.luau", "tools/PackagerMain.cpp", "tools/setup.luau", "docs/src/content/docs/meta/license.mdx"}


def SourceFiles(Repository, Revision):
    Cmd = ["git", "-C", str(Repository)]
    FullRevision = subprocess.check_output([*Cmd, "rev-parse", "--verify", Revision + "^{commit}"], text=True).strip()
    if FullRevision != Revision:
        raise ValueError("Supply the full immutable revision")
    Tree = subprocess.check_output([*Cmd, "ls-tree", "-rz", Revision]).split(b"\0")
    Blobs, Revisions = {}, {}
    for Entry in filter(None, Tree):
        Header, Name = Entry.decode().split("\t", 1)
        Mode, Kind, Oid = Header.split()
        if Kind == "commit":
            Revisions[Name] = Oid
        elif Name.startswith(Roots) or Name in Singles:
            if Mode not in ("100644", "100755"):
                raise ValueError("Review nonregular Engine source " + Name)
            Blobs[Name] = Oid
    Files = {}
    with subprocess.Popen([*Cmd, "cat-file", "--batch"], stdin=subprocess.PIPE, stdout=subprocess.PIPE) as Process:
        for Name, Oid in Blobs.items():
            Process.stdin.write((Oid + "\n").encode())
            Process.stdin.flush()
            Id, Kind, Size = Process.stdout.readline().split()
            if Kind != b"blob" or Id.decode() != Oid:
                raise ValueError("Git blob read failed for " + Name)
            Files[Name] = Process.stdout.read(int(Size))
            if Process.stdout.read(1) != b"\n":
                raise ValueError("Truncated Git source " + Name)
        Process.stdin.close()
        if Process.wait() != 0:
            raise ValueError("Git source extraction failed")
    Config = configparser.ConfigParser(interpolation=None)
    Config.read_string(Files[".gitmodules"].decode())
    Urls = {Section["path"]: Section["url"] for Section in Config.values() if "path" in Section}
    Pins = ["Path\tGit revision\tUpstream URL", *(f"{Name}\t{Oid}\t{Urls[Name]}" for Name, Oid in sorted(Revisions.items()))]
    Files["DependencyRevisions.tsv"] = ("\n".join(Pins) + "\n").encode()
    return Files


def WriteSource(Repository, Revision, Output, BuildRoot=None):
    Files = SourceFiles(Repository, Revision)
    if BuildRoot:
        for Name, Data in Files.items():
            if Name == "DependencyRevisions.tsv":
                continue
            Actual = (BuildRoot / Name).read_bytes()
            if Actual != Data and (b"\x00" in Data or Actual.replace(b"\r\n", b"\n") != Data.replace(b"\r\n", b"\n")):
                raise ValueError("Build tree differs from archived input " + Name)
    License = Files["docs/src/content/docs/meta/license.mdx"]
    Files["LICENSE.txt"] = License[License.index(b"Mozilla Public License Version 2.0"):]
    Files["Readme.txt"] = (f"Gargantuan Engine covered source for build revision {Revision}\n\n"
        "src and include contain editable Engine code. assets contains runtime scripts, shaders and generation inputs. "
        "cmake, tools and CMakeLists.txt contain build settings and local integration patches. Read LICENSE.txt and "
        "DependencyRevisions.tsv for the MPL terms and exact separately obtained upstream inputs.\n").encode()
    Buffer = io.BytesIO()
    with zipfile.ZipFile(Buffer, "w") as Archive:
        for Name, Data in sorted(Files.items()):
            Info = zipfile.ZipInfo(Name)
            Info.compress_type = zipfile.ZIP_DEFLATED
            Archive.writestr(Info, Data)
    Data = Buffer.getvalue()
    CheckArchive(Data, Revision)
    Output.write_bytes(Data)
    print(f"{len(Files)} files, SHA-256 {hashlib.sha256(Data).hexdigest()}")


if __name__ == "__main__":
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("repository", type=Path)
    Parser.add_argument("revision")
    Parser.add_argument("output", type=Path)
    Parser.add_argument("--build-root", type=Path, help="Compare archived files to the actual source tree used by CMake")
    Args = Parser.parse_args()
    WriteSource(Args.repository, Args.revision, Args.output, Args.build_root)
