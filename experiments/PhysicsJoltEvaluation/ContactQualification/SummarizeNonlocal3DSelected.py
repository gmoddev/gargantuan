"""Summarize six bounded large-cloth probes and preserved small-cloth checks."""

import argparse
import json
from pathlib import Path


Selected = (("ES", 34), ("ES", 38), ("ES", 39),
            ("JoltIG", 37), ("JoltIG", 38), ("JoltIG", 39))


def ReadJson(PathValue):
    return json.loads(PathValue.read_text(encoding="utf-8"))


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("ArtifactRoot", type=Path)
    Parser.add_argument("--output", type=Path, required=True)
    Args = Parser.parse_args()
    Results = []
    for Mode, Frame in Selected:
        Name = f"NonlocalWitness{Mode}Frame{Frame}.json"
        Result = ReadJson(Args.ArtifactRoot / Name)
        if Result["Frame"] != Frame or Result["Faces"] != 32258 or \
                Result["Vertices"] != 16384 or \
                Result["Status"] != "NO_STRICT_INTERIOR_EDGE_FACE_WITNESS_WITHIN_BOUNDS":
            raise ValueError(f"Unexpected selected-frame result: {Name}")
        Results.append({"Artifact": Name, **Result})
    Prior = ReadJson(Args.ArtifactRoot / "FirstCorpus-20261109T103524" /
                     "IndependentReview.json")["Nonlocal3DTriangleChecks"]
    if len(Prior) != 9 or any(
            Result["Status"] != "NO_DISJOINT_FACE_INTERSECTION"
            for Result in Prior.values()):
        raise ValueError("Prior small-cloth nonlocal controls differ")
    Result = {
        "Selected128SquaredFrames": len(Results),
        "SelectedFacePairCandidates": sum(Row["CandidatePairsTested"] for Row in Results),
        "SelectedCellPostings": sum(Row["CellPostings"] for Row in Results),
        "SelectedResults": Results,
        "PriorSmallCases": len(Prior),
        "PriorSmallFrames": sum(Row["Frames"] for Row in Prior.values()),
        "PriorSmallCandidatePairs": sum(Row["CandidatePairsTested"] for Row in Prior.values()),
        "Scope": "Selected-frame strict nonlocal edge-face witness search; no all-frame or local-neighbor self-intersection certificate",
    }
    Args.output.write_text(json.dumps(Result, indent=2) + "\n", encoding="utf-8")
    print(f"Selected {len(Results)} large frames, "
          f"{Result['SelectedFacePairCandidates']} tested pairs; no strict witness")


if __name__ == "__main__":
    Main()
