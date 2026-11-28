"""Build a single headless feasibility probe from a populated MSVC/Ninja build.

Run in the matching x64 Visual Studio developer shell. All new products live
in the supplied empty scratch directory. The input build is opened read-only.
"""
import argparse
import ctypes
import hashlib
import json
import os
import re
import shutil
import subprocess
from pathlib import Path


def Arguments(Command):
    Count = ctypes.c_int()
    Split = ctypes.windll.shell32.CommandLineToArgvW
    Split.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
    Split.restype = ctypes.POINTER(ctypes.c_wchar_p)
    Values = Split(Command, ctypes.byref(Count))
    try:
        return Values[:Count.value]
    finally:
        ctypes.windll.kernel32.LocalFree(ctypes.cast(Values, ctypes.c_void_p))


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    for Name in ("build", "scratch"):
        Parser.add_argument("--" + Name, required=True, type=Path)
    Options = Parser.parse_args()
    Build = Options.build.resolve()
    Scratch = Options.scratch.resolve()
    Scratch.mkdir(parents=True, exist_ok=True)
    if any(Scratch.iterdir()):
        raise ValueError("Use an empty scratch directory")
    Compile = json.loads((Build / "compile_commands.json").read_text())
    Gui = next(Row for Row in Compile if Row["file"].endswith("/GuiFoundationTests.cpp"))
    Source = Path(Gui["file"]).parents[1]
    Objects = []
    Replaced = set()

    def CompileOne(Input, Recipe, Extra=()):
        Output = Scratch / (Input.stem + ".obj")
        Original = Arguments(Recipe["command"])
        Args = [Arg for Arg in Original[1:] if not Arg.startswith(("/Fo", "/Fd"))
                and Arg not in ("/FS", "-c") and not Arg.lower().endswith(".cpp")]
        subprocess.run([Original[0], *Args, *Extra, "/c", "/Fo" + str(Output), Input], cwd=Build, check=True, timeout=60)
        Objects.append(str(Output))

    # Older retained products predate SetDevelopmentClock. Mirror the two
    # current observation points without changing the archive's class layout.
    if "SetDevelopmentClock" not in (Source / "include/gargantuan/Engine.hpp").read_text():
        for Tail, Before, After in [
            ("src/Engine.cpp", "CurrentTick = std::chrono::steady_clock::now();", "CurrentTick = ResearchClockNow();"),
            ("src/scripting/ThreadEngine.cpp", "std::chrono::steady_clock::now().time_since_epoch()",
             "ResearchClockNow().time_since_epoch()")]:
            Input = Source / Tail
            Body = Input.read_text()
            if Body.count(Before) != 1:
                raise ValueError("Unrecognized clock observation in " + Tail)
            Local = Scratch / Input.name
            Local.write_text("#include <chrono>\nstd::chrono::steady_clock::time_point ResearchClockNow();\n" + Body.replace(Before, After))
            Recipe = next(Row for Row in Compile if Path(Row["file"]).resolve() == Input)
            Replaced.add(Path(Recipe["output"]).resolve())
            CompileOne(Local, Recipe, ["/I" + str(Input.parent)])
    CompileOne(Path(__file__).with_name("NativeProbe.cpp").resolve(), Gui)
    Ninja = (Build / "build.ninja").read_text()
    Target = re.search(r"^build gargantuan_gui_foundation_tests.exe: [^ ]+ ([^\n]+)\n(.*?)(?=^\S)", Ninja, re.M | re.S)
    Libs = Arguments(re.search(r"^  LINK_LIBRARIES = (.+)$", Target[2], re.M)[1].strip())
    Core = [Build / Item for Item in Target[1].split(" | ")[0].split()
            if Item.startswith("CMakeFiles\\gargantuan_core.dir\\") and (Build / Item).resolve() not in Replaced]
    if not Core:
        raise ValueError("Expected native schema registration objects")
    Digest = hashlib.sha256()
    for Item in Core:
        with Item.open("rb") as Input:
            Digest.update(hashlib.file_digest(Input, "sha256").digest())
    print(f"[Lifecycle:Build] reused_objects={len(Core)} aggregate_sha256={Digest.hexdigest()}", flush=True)
    Exe = Scratch / "NativeProbe.exe"
    Response = Scratch / "ProbeLink.rsp"
    Response.write_text(subprocess.list2cmdline([str(Item) for Item in [*Objects, *Core, *Libs]]))
    subprocess.run(["link.exe", "/nologo", "/incremental:no", "/out:" + str(Exe),
                    "@" + str(Response)], cwd=Build, check=True, timeout=90)
    shutil.copytree(Build / "runtime", Scratch / "runtime")
    Env = os.environ.copy()
    Env.update(PATH=str(Build) + os.pathsep + Env["PATH"], SDL_AUDIODRIVER="dummy")
    ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002 | 0x8000)
    subprocess.run([Exe], cwd=Scratch, env=Env, check=True, timeout=30)


if __name__ == "__main__":
    Main()
