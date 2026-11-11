#!/usr/bin/env python3
"""Freeze built Runtime6 GPU probes without running them on the CI runner."""

import argparse
import hashlib
import json
import re
import shutil
import subprocess
from pathlib import Path


PROBES = (
    "gargantuan_runtime_foundation6_benchmark.exe",
    "gargantuan_renderer_depth_tests.exe",
)


def Hash(PathValue):
    Digest = hashlib.sha256()
    with PathValue.open("rb") as Source:
        for Chunk in iter(lambda: Source.read(1024 * 1024), b""):
            Digest.update(Chunk)
    return Digest.hexdigest()


def WriteJson(PathValue, Value):
    PathValue.write_text(json.dumps(Value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def CopyHashed(Source, Destination):
    if not Source.is_file() or Source.stat().st_size == 0:
        raise RuntimeError(f"missing or empty physical probe input: {Source}")
    Destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(Source, Destination)
    SourceHash = Hash(Source)
    if Hash(Destination) != SourceHash:
        raise RuntimeError(f"physical probe copy changed bytes: {Source}")
    return SourceHash


def Run(*Arguments):
    Result = subprocess.run(Arguments, check=True, capture_output=True, text=True)
    return Result.stdout


def Freeze(SourceRoot, BuildRoot, OutputRoot, ExpectedSource):
    Source = Run("git", "-C", str(SourceRoot), "rev-parse", "HEAD").strip()
    if Source != ExpectedSource:
        raise RuntimeError(f"source receipt differs from checkout: {Source} != {ExpectedSource}")
    if OutputRoot.exists():
        raise RuntimeError(f"physical probe output already exists: {OutputRoot}")
    Receipts = OutputRoot / "receipts"
    Receipts.mkdir(parents=True)

    Cache = (BuildRoot / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace")
    for Entry in (
        "CMAKE_BUILD_TYPE:STRING=Release",
        "GARGANTUAN_WITH_GNS:BOOL=ON",
        "GARGANTUAN_BUILD_RENDERER_BENCHMARKS:BOOL=ON",
    ):
        if not re.search(r"(?m)^" + re.escape(Entry) + r"\s*$", Cache):
            raise RuntimeError(f"physical probe build lacks required CMake configuration: {Entry}")
    CopyHashed(BuildRoot / "CMakeCache.txt", Receipts / "CMakeCache.txt")
    CopyHashed(SourceRoot / "cmake" / "gns" / "vcpkg.json", Receipts / "gns-vcpkg.json")

    CompilerFiles = list((BuildRoot / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake"))
    if len(CompilerFiles) != 1:
        raise RuntimeError(f"expected one configured C++ compiler receipt, got {len(CompilerFiles)}")
    CompilerText = CompilerFiles[0].read_text(encoding="utf-8", errors="replace")
    Compiler = {}
    for Key in ("CMAKE_CXX_COMPILER_ID", "CMAKE_CXX_COMPILER_VERSION", "CMAKE_CXX_COMPILER"):
        Match = re.search(r"(?m)^set\(" + Key + r' "([^"]+)"\)', CompilerText)
        if not Match:
            raise RuntimeError(f"configured compiler receipt lacks {Key}")
        Compiler[Key] = Match.group(1)
    Compiler["receiptSha256"] = CopyHashed(CompilerFiles[0], Receipts / "CMakeCXXCompiler.cmake")
    WriteJson(Receipts / "compiler-identity.json", Compiler)

    Submodules = Run("git", "-C", str(SourceRoot), "submodule", "status", "--recursive")
    if any(Line.startswith(("-", "+", "U")) for Line in Submodules.splitlines()):
        raise RuntimeError("submodule checkout differs from source gitlinks")
    (Receipts / "submodules.txt").write_text(Submodules, encoding="utf-8")

    ShaderRoot = SourceRoot / "assets" / "shaders"
    ShaderSources = sorted(
        PathValue for PathValue in ShaderRoot.rglob("*")
        if PathValue.is_file() and PathValue.suffix in (".vert", ".frag", ".glsl")
    )
    ExpectedShaders = sorted(PathValue.name + ".spv" for PathValue in ShaderSources
                             if PathValue.suffix in (".vert", ".frag"))
    CompiledShaders = sorted(BuildRoot.joinpath("shaders").glob("*.spv"))
    if len(ExpectedShaders) != 18 or len(set(ExpectedShaders)) != 18 or [P.name for P in CompiledShaders] != ExpectedShaders:
        raise RuntimeError("physical probe needs exactly the 18 matching compiled SPIR-V shaders")
    WriteJson(Receipts / "shader-inputs.sha256.json", {
        "source": Source,
        "files": {P.relative_to(SourceRoot).as_posix(): Hash(P) for P in ShaderSources},
    })
    Shaders = {P.name: CopyHashed(P, OutputRoot / "shaders" / P.name) for P in CompiledShaders}

    Dlls = sorted(BuildRoot.glob("*.dll"))
    if not Dlls or not any(P.name.lower() == "sdl3.dll" for P in Dlls):
        raise RuntimeError("physical probe runtime lacks its root SDL3.dll")
    Binaries = {Name: CopyHashed(BuildRoot / Name, OutputRoot / Name) for Name in PROBES}
    for Dll in Dlls:
        Binaries[Dll.name] = CopyHashed(Dll, OutputRoot / Dll.name)

    Dumpbin = shutil.which("dumpbin")
    Dependencies = {"tool": Dumpbin, "probes": {}}
    if Dumpbin:
        for Name in PROBES:
            Dependencies["probes"][Name] = Run(Dumpbin, "/dependents", str(BuildRoot / Name))
    WriteJson(Receipts / "dependencies.json", Dependencies)
    WriteJson(Receipts / "manifest.json", {
        "schema": 1, "source": Source, "binaries": Binaries, "shaders": Shaders,
        "cmakeCacheSha256": Hash(Receipts / "CMakeCache.txt"),
        "gnsManifestSha256": Hash(Receipts / "gns-vcpkg.json"),
        "submodulesSha256": Hash(Receipts / "submodules.txt"),
        "shaderInputsSha256": Hash(Receipts / "shader-inputs.sha256.json"),
        "compilerIdentitySha256": Hash(Receipts / "compiler-identity.json"),
        "dependenciesSha256": Hash(Receipts / "dependencies.json"),
    })
    print(f"[CI:Runtime6Physical] source={Source} probes={len(PROBES)} dlls={len(Dlls)} shaders={len(Shaders)}")


def Main():
    Parser = argparse.ArgumentParser()
    Parser.add_argument("--source-root", type=Path, required=True)
    Parser.add_argument("--build-root", type=Path, required=True)
    Parser.add_argument("--output-root", type=Path, required=True)
    Parser.add_argument("--source", required=True)
    Args = Parser.parse_args()
    Freeze(Args.source_root.resolve(), Args.build_root.resolve(), Args.output_root.resolve(), Args.source)


if __name__ == "__main__":
    Main()
