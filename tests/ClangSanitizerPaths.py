# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0

"""Exercise Clang's actual sanitizer metadata, symbolizer and build dependency edges."""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def Run(Arguments, **Options):
    return subprocess.run(Arguments, check=True, text=True, **Options)


def CheckCase(Base, Module, Compiler, Generator, Layout):
    Sentinel = "privateCheckoutMarker67D1"
    Source = Base / (Sentinel + " source")
    Build = {"disjoint": Base / (Sentinel + " build"), "nested": Source / (Sentinel + " build"),
             "in-source": Source}[Layout]
    Dependency = Base / (Sentinel + " external")
    Relocated = Base / "Relocated"
    for Directory in (Source, Build, Dependency, Relocated):
        Directory.mkdir(parents=True, exist_ok=True)
    (Source / "CMakeLists.txt").write_text(
        'cmake_minimum_required(VERSION 3.25)\nproject(PathProbe CXX)\n'
        'include("${Module}")\nGargantuanAddSanitizerSource(External "${Dependency}")\n'
        'add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/Generated.hpp" '
        'COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/Value.txt" '
        '"${CMAKE_CURRENT_BINARY_DIR}/Generated.hpp" DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Value.txt")\n'
        'add_compile_options(-x c++)\n'
        'add_executable(Probe Probe.cpp "${CMAKE_CURRENT_BINARY_DIR}/Generated.hpp")\n'
        'target_include_directories(Probe PRIVATE "${Dependency}" "${CMAKE_CURRENT_BINARY_DIR}")\n'
    )
    ExternalHeader = Dependency / "Arithmetic.hpp"
    ExternalHeader.write_text('__attribute__((noinline)) int Add(int A) { return A + 1; }\n')
    Value = Source / "Value.txt"
    Value.write_text("#define GENERATED 35\n")
    (Source / "Probe.cpp").write_text(
        '#include "Arithmetic.hpp"\n#include "Generated.hpp"\n#include <climits>\n'
        '__attribute__((noinline)) int Read(int *P) { return *P; }\n'
        'int main(int Count, char **Args) {\n'
        '    if (Count == 2 && Args[1][0] == \'u\') return Add(INT_MAX);\n'
        '    if (Count == 2 && Args[1][0] == \'a\') {\n'
        '        int *P = new int(10); delete P; return Read(P);\n    }\n'
        '    return Add(GENERATED);\n}\n'
    )
    Chain = Base / "Chain.py"
    Chain.write_text('import os, sys\nfrom pathlib import Path\n'
                     'Path(sys.argv[1]).touch()\nos.execvp(sys.argv[2], sys.argv[2:])\n')
    ChainMarker = Base / "ChainUsed"
    Flags = "-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g -O1"
    Environment = dict(os.environ, CMAKE_NINJA_FORCE_RESPONSE_FILE="1")
    Run(["cmake", "-S", Source, "-B", Build, "-G", Generator, "-DModule=" + str(Module),
         "-DDependency=" + str(Dependency), "-DCMAKE_CXX_COMPILER=" + Compiler,
         "-DCMAKE_CXX_FLAGS=" + Flags,
         "-DCMAKE_CXX_COMPILER_LAUNCHER=" + ";".join([sys.executable, str(Chain), str(ChainMarker)])],
        env=Environment, stdout=subprocess.PIPE)
    def BuildAndRun(Expected):
        Run(["cmake", "--build", Build, "--parallel", "1"], env=Environment, stdout=subprocess.PIPE)
        Result = subprocess.run([Build / "Probe"])
        if Result.returncode != Expected:
            raise RuntimeError(f"Stale compiled input: expected {Expected}, got {Result.returncode}")

    BuildAndRun(36)
    Object = next((Build / "CMakeFiles" / "Probe.dir").rglob("*.o"))
    FirstWrite = Object.stat().st_mtime_ns
    BuildAndRun(36)
    if Object.stat().st_mtime_ns != FirstWrite:
        raise RuntimeError("Unchanged dependency graph recompiled the probe")
    Value.write_text("#define GENERATED 21\n")
    BuildAndRun(22)
    ExternalHeader.write_text('__attribute__((noinline)) int Add(int A) { return A + 2; }\n')
    BuildAndRun(23)
    if not ChainMarker.exists():
        raise RuntimeError("Configured compiler launcher did not run")
    Program = Relocated / "Probe"
    shutil.copy2(Build / "Probe", Program)
    Objects = list((Build / "CMakeFiles" / "Probe.dir").rglob("*.o"))
    if len(Objects) != 1:
        raise RuntimeError("Expected one compiled translation unit")
    for File in (Program, *Objects):
        if Sentinel.encode() in File.read_bytes():
            raise RuntimeError(f"Private directory name in {File.name}")
    Environment.update(ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:symbolize=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    for Mode, Diagnostic, Location in [("u", "signed integer overflow", "Arithmetic.hpp:1"),
                                        ("a", "heap-use-after-free", "Probe.cpp:4")]:
        Result = subprocess.run(["./Probe", Mode], cwd=Relocated, env=Environment,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if (Result.returncode == 0 or Diagnostic not in Result.stdout or Location not in Result.stdout
                or Sentinel in Result.stdout):
            raise RuntimeError(Result.stdout)
        Frame = next(Line for Line in Result.stdout.splitlines() if Location in Line)
        print(f"[Build:SanitizerPaths] {Generator} layout={Layout} {Frame.strip()}")


if __name__ == "__main__":
    Args = argparse.ArgumentParser()
    Args.add_argument("Module", type=Path)
    Args.add_argument("Compiler")
    Args.add_argument("Generator")
    Values = Args.parse_args()
    for Layout in ("disjoint", "nested", "in-source"):
        with tempfile.TemporaryDirectory(prefix="GargantuanSanitizerPaths-") as Temp:
            CheckCase(Path(Temp), Values.Module.resolve(), Values.Compiler, Values.Generator, Layout)
