"""Package the qualifier and an isolated Windows Python stdlib runtime; no probe."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys
import zipfile
from dependency import GetRoot


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("destination")
    Parser.add_argument("--skill", required=True)
    Args = Parser.parse_args()
    Upstream = GetRoot()
    Output = Path(Args.destination)
    Output.mkdir(parents=True, exist_ok=False)
    Source = Path(__file__).resolve().parent
    Skill = Path(Args.skill)
    shutil.copytree(Source, Output / "tool", ignore=shutil.ignore_patterns(".agent-coordinator", "__pycache__", "*.pyc", "bin", "obj", "publish"))
    shutil.copytree(Skill, Output / "skill", ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    shutil.copytree(Upstream, Output / "tool/.agent-coordinator", ignore=shutil.ignore_patterns(".git", "__pycache__", "*.pyc", "bin", "obj", "publish", "build", "dist", "*.egg-info"))
    Scripts = Output / "skill/scripts"
    Scripts.mkdir(exist_ok=True)
    for Name in ("qualifier.py", "dependency.py", "f1_candidate_source.py", "upstream.lock.json"):
        shutil.copyfile(Source / Name, Scripts / Name)
    # The installed personal skill can predate this project's qualified NDIS
    # hook. Keep both packaged entrypoints on the same pinned capture policy.
    shutil.copyfile(Source / "worker/PktMonCapture.ps1", Scripts / "PktMonCapture.ps1")
    shutil.copytree(Output / "tool/.agent-coordinator", Scripts / ".agent-coordinator")
    Runtime = Output / "runtime"
    Runtime.mkdir()
    Python = Path(sys.base_prefix)
    for Name in ("python.exe", "python3.dll", "python312.dll", "vcruntime140.dll", "vcruntime140_1.dll", "LICENSE.txt"):
        shutil.copyfile(Python / Name, Runtime / Name)
    shutil.copytree(Python / "DLLs", Runtime / "DLLs", ignore=shutil.ignore_patterns("*.pdb", "*.pyc", "__pycache__"))
    with zipfile.ZipFile(Runtime / "python312.zip", "w", zipfile.ZIP_DEFLATED) as Archive:
        for File in sorted((Python / "Lib").rglob("*")):
            Relative = File.relative_to(Python / "Lib")
            if File.is_file() and not set(Relative.parts).intersection({"site-packages", "__pycache__", "test", "tests", "idlelib", "tkinter"}) and File.suffix != ".pyc":
                Archive.write(File, str(Relative))
    (Runtime / "python312._pth").write_text("python312.zip\nDLLs\n.\n", encoding="ascii")
    Entries = []
    for File in sorted(Output.rglob("*")):
        if File.is_file():
            Entries.append({"Path": File.relative_to(Output).as_posix(), "SHA256": hashlib.sha256(File.read_bytes()).hexdigest().upper(), "Bytes": File.stat().st_size})
    (Output / "bundle-manifest.json").write_text(json.dumps({"PythonVersion": sys.version, "Files": Entries}, indent=2) + "\n")
    shutil.make_archive(str(Output), "zip", Output)
    print(json.dumps({"Package": str(Output), "Archive": str(Output) + ".zip", "PayloadFileCount": len(Entries)}))


if __name__ == "__main__":
    Main()
