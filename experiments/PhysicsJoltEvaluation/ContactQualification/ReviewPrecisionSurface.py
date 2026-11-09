"""Reduce staged contact precision and face-response comparisons without rerunning physics."""

import argparse
import hashlib
import json
from pathlib import Path


def ReadJson(PathValue):
    return json.loads(PathValue.read_text(encoding="utf-8"))


def VerifySha(PathValue, Expected):
    Actual = hashlib.sha256(PathValue.read_bytes()).hexdigest()
    if Actual != Expected:
        raise ValueError(f"SHA mismatch: {PathValue}: {Actual} != {Expected}")


def ReviewPrecision(Root):
    Rows = []
    for Case in sorted(Root.glob("xpbd-*")):
        Source = ReadJson(Case / "Source.json")
        if Source["Engine"] != "252340fa4566164eade9f0631cd333921c3f46f5":
            raise ValueError(f"Unexpected Engine source: {Case.name}")
        if Source["Native"] != "069b75b6222f19a8db2434a91fe2f72b3ae998de":
            raise ValueError(f"Unexpected native source: {Case.name}")
        VerifySha(Case / "ContactFixtureV1.json", Source["FixtureSha256"])
        VerifySha(Case / "ContactTraceV1.jsonl.gz", Source["TraceSha256"])
        Energy = ReadJson(Case / "IndependentEnergy.json")
        Verdict = ReadJson(Case / "Verdict.json")
        if Energy["AccountingIdentityStatus"] != "VERIFIED" or not Verdict["GeometryGatePass"]:
            raise ValueError(f"Failed accounting identity or geometry: {Case.name}")
        if "MaximumNormalizedUnpinnedStructuralNumericalImpulse" not in Energy:
            raise ValueError(f"Old source without free-edge measurement: {Case.name}")
        Rows.append({
            "Case": Case.name,
            "RawMomentumResidual": Energy["MaximumNormalizedRawMomentumResidual"],
            "MeasuredFreeEdgeRounding": Energy["MaximumNormalizedUnpinnedStructuralNumericalImpulse"],
            "AdjustedMomentumResidual": Energy["MaximumNormalizedMomentumResidual"],
            "RawToAdjustedRatio": Energy["MaximumNormalizedRawMomentumResidual"] /
                Energy["MaximumNormalizedMomentumResidual"],
            "MaximumAbsoluteUnclosedPhysicalResidualJ": Energy["MaximumAbsoluteUnclosedPhysicalResidualJ"],
            "PhysicalResidualStatus": Energy["PhysicalResidualStatus"],
        })
    if len(Rows) != 18:
        raise ValueError(f"Expected 18 precision controls, found {len(Rows)}")
    return Rows


def ReviewSurface(BroadRoot, SurfaceRoot):
    Suffixes = (
        "face-center", "edge-midpoint", "corner-vertex", "plane-0",
        "plane-plus10um", "plane-minus10um", "plane-plus1cm", "plane-minus1cm",
        "original128", "original128-plus10um", "original128-minus10um", "no-contact",
    )
    Rows = []
    for Suffix in Suffixes:
        Cases = [BroadRoot / f"jolt-A-{Suffix}", SurfaceRoot / f"jolt-H-{Suffix}",
                 SurfaceRoot / f"jolt-IG-{Suffix}"]
        Definitions = []
        Variants = []
        for Case in Cases:
            Source = ReadJson(Case / "Source.json")
            VerifySha(Case / "ContactFixtureV1.json", Source["FixtureSha256"])
            VerifySha(Case / "ContactTraceV1.jsonl.gz", Source["TraceSha256"])
            Definition = ReadJson(Case / "Case.json")
            Definition.pop("CaseId")
            Definitions.append(Definition)
            Verdict = ReadJson(Case / "Verdict.json")
            Variants.append({
                "Case": Case.name,
                "Engine": Source["Engine"],
                "Native": Source["Native"],
                "GeometryGatePass": Verdict["GeometryGatePass"],
                "MaximumAxialStrain": Verdict["MaximumAxialStrain"],
                "MaximumSpherePenetrationM": Verdict["MaximumSpherePenetrationM"],
                "ProjectedOrientationFlipInstances": Verdict["ProjectedOrientationFlipInstances"],
                "FullPhysicalQualification": Verdict["FullPhysicalQualification"],
            })
        if Definitions[0] != Definitions[1] or Definitions[0] != Definitions[2]:
            raise ValueError(f"Authored cases differ: {Suffix}")
        Rows.append({"Fixture": Suffix, "Variants": Variants})
    NoContact = [BroadRoot / "jolt-A-no-contact", SurfaceRoot / "jolt-H-no-contact",
                 SurfaceRoot / "jolt-IG-no-contact"]
    Hashes = [ReadJson(Case / "FrameHashes.json") for Case in NoContact]
    if len(Hashes[0]) != 60 or Hashes[0] != Hashes[1] or Hashes[0] != Hashes[2]:
        raise ValueError("No-contact position/velocity frame hashes differ")
    return {"Rows": Rows, "NoContactExactPositionAndVelocityFrames": 60}


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("ArtifactRoot", type=Path)
    Parser.add_argument("--output", type=Path, required=True)
    Args = Parser.parse_args()
    Result = {
        "Precision": ReviewPrecision(Args.ArtifactRoot / "Precision-20261109T111738"),
        "Surface": ReviewSurface(Args.ArtifactRoot / "BroadNative-20261109T104906",
                                 Args.ArtifactRoot / "Surface-20261109T110536"),
        "Scope": "Geometry and source accounting comparison; energy and reciprocal dynamic reaction remain ungated",
    }
    Args.output.write_text(json.dumps(Result, indent=2) + "\n", encoding="utf-8")
    print(f"Verified {len(Result['Precision'])} precision cases and "
          f"{len(Result['Surface']['Rows'])} matched native fixtures")


if __name__ == "__main__":
    Main()
