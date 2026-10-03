"""Reproduce the F1 native probe source archive from the qualified revision."""

import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import zipfile


NATIVE_SOURCE_PATHS = (
    "CMakeLists.txt",
    "cmake/GameNetworkingSockets.cmake",
    "cmake/gns/ApplyServiceFairness.cmake",
    "cmake/gns/ApplyReliableServiceFeedback.cmake",
    "cmake/gns/ApplyPreciseSenderWake.cmake",
    "cmake/gns/ReliableServiceFeedback.cpp",
    "cmake/gns/ReliableServiceFeedback.hpp",
    "cmake/gns/ReliableServiceFeedbackBridge.cpp",
    "include/gargantuan/network/MessageIntent.hpp",
    "include/gargantuan/network/ReliableServiceProfile.hpp",
    "include/gargantuan/network/Scheduler.hpp",
    "src/network/FiniteGrantServiceCurve.hpp",
    "src/network/GameNetworkingSocketsTransport.cpp",
    "src/network/GameSession.cpp",
    "src/network/GnsServiceDiagnostics.hpp",
    "src/network/PooledReliableServiceFeedback.hpp",
    "src/network/PooledServiceDiagnostics.hpp",
    "src/network/ReliableServiceFeedback.hpp",
    "src/network/Scheduler.cpp",
    "tests/FiniteGrantServiceCurveFixture.hpp",
    "tests/GameNetworkingSocketsTransportTests.cpp",
    "tests/GnsFourGrantFixture.hpp",
    "tests/GnsMixedTrafficFixture.hpp",
    "tests/GnsPacketTailFixture.hpp",
    "tests/PhysicalGnsFundingProbe.cpp",
    "tests/PhysicalGnsFundingProbeTrace.hpp",
    "tests/PooledReliableServiceProductionFixture.hpp",
    "tests/PooledServiceCurveFixture.hpp",
    "tests/ReliableServiceFeedbackFixture.hpp",
    "tests/cmake/GnsObserveSourceChecks.cmake",
    "tests/cmake/PooledServiceSourceChecks.cmake",
)


def PinnedRevision() -> str:
    Manifest = json.loads((Path(__file__).resolve().parent /
                           "phase1-f1-source-manifest.json").read_text(encoding="utf-8"))
    Revision = Manifest.get("BaseHead")
    if not isinstance(Revision, str) or not re.fullmatch(r"[0-9a-f]{40}", Revision):
        raise SystemExit("[Qualification:Source] invalid pinned F1 source revision")
    return Revision


def Main() -> None:
    Root = Path(__file__).resolve().parents[2]
    if len(sys.argv) >= 2 and sys.argv[1] in ("--candidate", "--verify-candidate", "--verify-candidate-tree"):
        from f1_candidate_source import FORMAT, Create, VerifyPinned, VerifyTree
        Mode = sys.argv[1]
        if len(sys.argv) != (5 if Mode == "--verify-candidate-tree" else 4):
            raise SystemExit("usage: --candidate commit output.zip | --verify-candidate commit archive.zip | "
                             "--verify-candidate-tree commit archive.zip source-root")
        Expected, ArchivePath = sys.argv[2], Path(sys.argv[3]).resolve()
        if Mode == "--candidate":
            Hash = Create(Root, Expected, ArchivePath)
            print(json.dumps({"SourceArchiveFormat": FORMAT, "BaseHead": Expected,
                              "SourceArchive": ArchivePath.name, "OverlayArchiveSha256": Hash}))
        else:
            Manifest = (VerifyTree(Path(sys.argv[4]).resolve(), ArchivePath, Expected)
                        if Mode == "--verify-candidate-tree" else VerifyPinned(Root, ArchivePath, Expected))
            print("[Qualification:Source] candidate_verified=" + str(len(Manifest["Files"])))
        return
    SourceRevision = PinnedRevision()
    if len(sys.argv) == 3 and sys.argv[1] == "--verify-pinned":
        with zipfile.ZipFile(Path(sys.argv[2]).resolve()) as Archive:
            if Archive.namelist() != sorted(NATIVE_SOURCE_PATHS):
                raise SystemExit("[Qualification:Source] F1 source path list mismatch")
            for Name in Archive.namelist():
                Expected = subprocess.run(["git", "show", SourceRevision + ":" + Name],
                                          cwd=Root, check=True, capture_output=True).stdout
                if Archive.read(Name) != Expected:
                    raise SystemExit("[Qualification:Source] pinned source mismatch: " + Name)
        print("[Qualification:Source] pinned_verified=" + str(len(NATIVE_SOURCE_PATHS)))
        return
    if len(sys.argv) == 4 and sys.argv[1] == "--verify-tree":
        ArchivePath = Path(sys.argv[2]).resolve()
        Root = Path(sys.argv[3]).resolve()
        with zipfile.ZipFile(ArchivePath) as Archive:
            if Archive.namelist() != sorted(NATIVE_SOURCE_PATHS):
                raise SystemExit("[Qualification:Source] F1 source path list mismatch")
            Mismatches = []
            NormalizedLineEndings = 0
            for Name in Archive.namelist():
                Expected = Archive.read(Name)
                Actual = (Root / Name).read_bytes()
                if Expected == Actual:
                    continue
                if Expected.replace(b"\r\n", b"\n") == Actual.replace(b"\r\n", b"\n"):
                    NormalizedLineEndings += 1
                else:
                    Mismatches.append(Name)
            if Mismatches:
                raise SystemExit("[Qualification:Source] source mismatches: " + ", ".join(Mismatches))
        print("[Qualification:Source] verified=" + str(len(NATIVE_SOURCE_PATHS)) +
              " line_ending_normalized=" + str(NormalizedLineEndings))
        return
    if len(sys.argv) != 2:
        raise SystemExit("usage: make_f1_source_archive.py output.zip | --verify-pinned archive.zip | --verify-tree archive.zip source-root")
    Output = Path(sys.argv[1]).resolve()
    if Output.exists():
        raise SystemExit("[Qualification:Source] output already exists")
    Output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(Output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as Archive:
        for Name in sorted(NATIVE_SOURCE_PATHS):
            Source = subprocess.run(["git", "show", SourceRevision + ":" + Name], cwd=Root,
                                    check=True, capture_output=True).stdout
            Entry = zipfile.ZipInfo(Name, (1980, 1, 1, 0, 0, 0))
            Entry.compress_type = zipfile.ZIP_DEFLATED
            Archive.writestr(Entry, Source, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    print("[Qualification:Source] sha256=" + hashlib.sha256(Output.read_bytes()).hexdigest().upper())


if __name__ == "__main__":
    Main()
