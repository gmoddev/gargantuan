#!/usr/bin/env python3
"""Read-only, whole-tree provenance check for the Foundation 3–6 squash."""

import argparse
import json
import subprocess


Main = "2a4c1d2b03e40178b97334bb6f1f2eeedd51fe9f"
Fork = "5b6782e35c4e24fd80bc8d5526104c615c72d84d"
Sources = (
    "81a5c5aff36e2149c3181ada6858d7143da19ea0",
    "94f920485564798742bc22a061cbc3a415034fa3",
    "da902ea481e627d7c4d789033076b3be7dc38334",
    "741146b810d0eb59e1589248f8ccefa31f2bcf1d",
)
Differences = {
    "README.md": "Preserved mainline-only provenance clarification",
    "CMakeLists.txt": "Verify player/server distribution shader completeness",
    "cmake/StageRuntimeDistribution.cmake": "Stage all 18 foundation shaders",
    "tests/PackagingTests.cpp": "Package presence and missing-shader rejection controls",
    "src/render/Renderer.cpp": "Retain constructed pass phases for correct SDR metric attribution",
    "tests/RendererDepthTests.cpp": "HDR/SDR pass metric assertions in real-Part GPU fixture",
    ".github/workflows/renderer-foundation5.yml": "Execute real-Part fixture on software Vulkan too",
    "devdocs/CurrentArchitecture/StandalonePackagingFoundation1.md": "Document complete shader packaging",
    "devdocs/CurrentArchitecture/RendererFoundation5Cpu.md": "Document SDR metric attribution",
    "KNOWN_ISSUES.md": "Record pre-existing publisher failure-recovery limitation",
    "devdocs/Validation/EngineFoundations3To6Integration.md": "Integration ledger and acceptance boundaries",
    "tools/ci/VerifyEngineFoundationIntegration.py": "Reproducible source-equivalence verifier",
}


def Git(*Arguments):
    return subprocess.check_output(["git", *Arguments])


def Tree(Revision):
    Result = {}
    for Entry in Git("ls-tree", "-rz", "--full-tree", Revision).split(b"\0"):
        if Entry:
            Metadata, Path = Entry.split(b"\t", 1)
            Result[Path.decode("utf-8")] = Metadata.decode("ascii")
    return Result


def Require(Condition, Message):
    if not Condition:
        raise RuntimeError(Message)


def Verify(Candidate):
    Head = Git("rev-parse", "--verify", Candidate + "^{commit}").decode().strip()
    subprocess.run(["git", "merge-base", "--is-ancestor", Main, Head], check=True)
    MainTree, SourceTree, CandidateTree = Tree(Main), Tree(Sources[-1]), Tree(Head)
    Require(MainTree["README.md"] == CandidateTree["README.md"], "Mainline README was lost")
    Commits = Git("rev-list", "--reverse", "--first-parent", Main + ".." + Head).decode().splitlines()
    Require(4 <= len(Commits) <= 10, "Integration must have four to ten authored commits")
    Stages = []
    for Commit, Source in zip(Commits[:4], Sources):
        Original, Squashed = Tree(Source), Tree(Commit)
        Changed = sorted(Path for Path in Original.keys() | Squashed.keys()
                         if Original.get(Path) != Squashed.get(Path))
        Require(Changed == ["README.md"], "Foundation stage differs beyond mainline README: " + Commit)
        Require(Squashed["README.md"] == MainTree["README.md"], "Stage lost mainline README")
        Stages.append({"Commit": Commit, "Source": Source,
                       "SourceTree": Git("rev-parse", Source + "^{tree}").decode().strip()})
    Changed = sorted(Path for Path in SourceTree.keys() | CandidateTree.keys()
                     if SourceTree.get(Path) != CandidateTree.get(Path))
    Require(set(Changed) == set(Differences), "Unclassified or missing integration differences: " + repr(Changed))
    Require(not (SourceTree.keys() - CandidateTree.keys()), "Accepted source files were removed")
    Evidence = [Path for Path in SourceTree if Path.startswith("devdocs/Validation/")]
    Require(all(SourceTree[Path] == CandidateTree[Path] for Path in Evidence),
            "Historical validation evidence changed")
    subprocess.run(["git", "diff", "--check", Main, Head], check=True)
    return {
        "Schema": 1, "Candidate": Head, "Mainline": Main, "DevelopmentFork": Fork,
        "AcceptedSource": Sources[-1], "QualifiedImplementation": "f686a4fbce5c21d6cec89bbb08499586b1893378",
        "CandidateTree": Git("rev-parse", Head + "^{tree}").decode().strip(),
        "AuthoredCommits": len(Commits), "Stages": Stages,
        "AcceptedFiles": len(SourceTree), "IdenticalFiles": len(SourceTree.keys() & CandidateTree.keys()) -
        sum(Path in SourceTree and Path in CandidateTree for Path in Changed),
        "HistoricalValidationFilesPreserved": len(Evidence),
        "Differences": [{"Path": Path, "Reason": Differences[Path],
                         "SourceEntry": SourceTree.get(Path), "CandidateEntry": CandidateTree.get(Path)}
                        for Path in Changed],
        "Verdict": "PASS_SOURCE_EQUIVALENCE_WITH_CLASSIFIED_INTEGRATION_CHANGES",
        "Qualification": "Source comparison only; fresh build/runtime/physical receipts are separate",
    }


def Run():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--candidate", default="HEAD")
    Arguments = Parser.parse_args()
    print(json.dumps(Verify(Arguments.candidate), indent=2))


if __name__ == "__main__":
    Run()
