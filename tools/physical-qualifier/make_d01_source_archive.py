"""Reproduce the D01 native probe source archive without build artifacts."""

import hashlib
from pathlib import Path
import sys
import zipfile


NATIVE_SOURCE_PATHS = (
    "CMakeLists.txt",
    "cmake/gns/ApplyReliableServiceFeedback.cmake",
    "cmake/gns/ReliableServiceFeedback.cpp",
    "cmake/gns/ReliableServiceFeedback.hpp",
    "cmake/gns/ReliableServiceFeedbackBridge.cpp",
    "include/gargantuan/network/MessageIntent.hpp",
    "include/gargantuan/network/ReliableServiceProfile.hpp",
    "include/gargantuan/network/Scheduler.hpp",
    "src/network/GameNetworkingSocketsTransport.cpp",
    "src/network/GameSession.cpp",
    "src/network/PooledReliableServiceFeedback.hpp",
    "src/network/PooledServiceDiagnostics.hpp",
    "src/network/ReliableServiceFeedback.hpp",
    "src/network/Scheduler.cpp",
    "tests/GameNetworkingSocketsTransportTests.cpp",
    "tests/PhysicalGnsFundingProbe.cpp",
    "tests/PhysicalGnsFundingProbeTrace.hpp",
    "tests/PooledReliableServiceProductionFixture.hpp",
    "tests/PooledServiceCurveFixture.hpp",
    "tests/ReliableServiceFeedbackFixture.hpp",
    "tests/cmake/PooledServiceSourceChecks.cmake",
)


def Main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: make_d01_source_archive.py output.zip")
    Root = Path(__file__).resolve().parents[2]
    Output = Path(sys.argv[1]).resolve()
    if Output.exists():
        raise SystemExit("[Qualification:Source] output already exists")
    Output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(Output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as Archive:
        for Name in sorted(NATIVE_SOURCE_PATHS):
            Entry = zipfile.ZipInfo(Name, (1980, 1, 1, 0, 0, 0))
            Entry.compress_type = zipfile.ZIP_DEFLATED
            Archive.writestr(Entry, (Root / Name).read_bytes(),
                             compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    print("[Qualification:Source] sha256=" + hashlib.sha256(Output.read_bytes()).hexdigest().upper())


if __name__ == "__main__":
    Main()
