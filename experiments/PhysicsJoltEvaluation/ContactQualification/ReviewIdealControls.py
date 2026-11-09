"""Independently reduce paired XPBD ideal-velocity accounting and free-flight controls."""

import argparse
import gzip
import json
import math
from pathlib import Path

from ReviewPrecisionSurface import VerifyFrameHashes, VerifySha


EnginePin = "27cdfdf4ec518275b5e934318070aa3cb1f1a532"
NativePin = "069b75b6222f19a8db2434a91fe2f72b3ae998de"
Kinds = ("xpbd-accounting-moving-control", "xpbd-accounting-static-control",
         "xpbd-ideal-velocity-no-contact-uniform-launch")


def ReadJson(PathValue):
    return json.loads(PathValue.read_text(encoding="utf-8"))


def VelocityError(Case, Target):
    Maximum = 0.0
    SumSquares = 0.0
    Count = 0
    with gzip.open(Case / "ContactTraceV1.jsonl.gz", "rt", encoding="utf-8") as Stream:
        for Line in Stream:
            Velocities = json.loads(Line)["Velocities"]
            for Index in range(0, len(Velocities), 3):
                Error = math.sqrt(sum((Velocities[Index + Axis] - Target[Axis]) ** 2
                                      for Axis in range(3)))
                Maximum = max(Maximum, Error)
                SumSquares += Error * Error
                Count += 1
    return {"MaximumVertexVelocityErrorMPerS": Maximum,
            "RmsVertexVelocityErrorMPerS": math.sqrt(SumSquares / Count),
            "MeasuredVertexFrames": Count}


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("Corpus", type=Path)
    Parser.add_argument("--output", type=Path, required=True)
    Args = Parser.parse_args()
    Rows = []
    for Base in ("A", "E", "G"):
        for Kind in Kinds:
            for Substeps in (16, 32, 64):
                Cases = [Args.Corpus / f"xpbd-{Variant}-{Kind}-N{Substeps}"
                         for Variant in (Base, Base + "H")]
                Definitions = []
                Pair = []
                for Case in Cases:
                    Source = ReadJson(Case / "Source.json")
                    if Source["Engine"] != EnginePin or Source["Native"] != NativePin:
                        raise ValueError(f"Unexpected source pin: {Case.name}")
                    VerifySha(Case / "ContactFixtureV1.json", Source["FixtureSha256"])
                    VerifySha(Case / "ContactTraceV1.jsonl.gz", Source["TraceSha256"])
                    if VerifyFrameHashes(Case) != 12:
                        raise ValueError(f"Unexpected frame count: {Case.name}")
                    Definition = ReadJson(Case / "Case.json")
                    Definition.pop("CaseId")
                    Definitions.append(Definition)
                    Verdict = ReadJson(Case / "Verdict.json")
                    Energy = ReadJson(Case / "IndependentEnergy.json")
                    Ideal = Case.name.startswith(f"xpbd-{Base}H-")
                    if not Verdict["GeometryGatePass"] or \
                            Energy["AccountingIdentityStatus"] != "VERIFIED" or \
                            Energy["IdealVelocityReconstructionEnabled"] != Ideal:
                        raise ValueError(f"Failed geometry/accounting/flag: {Case.name}")
                    Data = {
                        "Case": Case.name,
                        "IdealVelocityReconstructionEnabled": Ideal,
                        "RawMomentumResidual": Energy["MaximumNormalizedRawMomentumResidual"],
                        "SourceMomentumResidual": Energy["MaximumNormalizedMomentumResidual"],
                        "WouldBeFreeEdgeRounding":
                            Energy["MaximumNormalizedUnpinnedStructuralNumericalImpulse"],
                        "PinPositionStoreRounding":
                            Energy["MaximumNormalizedPinConstraintNumericalImpulse"],
                        "MaximumUnclosedPhysicalEnergyResidualJ":
                            Energy["MaximumAbsoluteUnclosedPhysicalResidualJ"],
                        "InitialKineticJ": Energy["Initial"]["KineticJ"],
                        "FinalKineticJ": Energy["Final"]["KineticJ"],
                        "PhysicalResidualStatus": Energy["PhysicalResidualStatus"],
                    }
                    if Kind.endswith("no-contact-uniform-launch"):
                        if Definition["Pins"] != "free" or Definition["Spheres"] or \
                                Definition["GravityMPerS2"] != [0, 0, 0]:
                            raise ValueError(f"Not a free uniform flight: {Case.name}")
                        Data.update(VelocityError(Case, Definition["InitialVelocityMPerS"]))
                    Pair.append(Data)
                if Definitions[0] != Definitions[1]:
                    raise ValueError(f"Paired fixtures differ: {Base}/{Kind}/N{Substeps}")
                Rows.append({"Base": Base, "Control": Kind, "Substeps": Substeps,
                             "A": Pair[0], "H": Pair[1],
                             "RawMomentumImprovementRatio":
                                 Pair[0]["RawMomentumResidual"] / Pair[1]["RawMomentumResidual"]})
    if len(Rows) != 27:
        raise ValueError(f"Expected 27 paired controls, found {len(Rows)}")
    for Substeps in (16, 32, 64):
        for Suffix in ("", "H"):
            Hashes = [ReadJson(Args.Corpus / f"xpbd-{Base}{Suffix}-"
                               f"xpbd-ideal-velocity-no-contact-uniform-launch-N{Substeps}" /
                               "FrameHashes.json") for Base in ("A", "E", "G")]
            if Hashes[0] != Hashes[1] or Hashes[0] != Hashes[2]:
                raise ValueError(f"No-contact variants differ at N{Substeps} H={bool(Suffix)}")
    Result = {"Engine": EnginePin, "Native": NativePin, "PairedControls": Rows,
              "VerifiedCases": 54, "VerifiedFrames": 648,
              "Scope": "Raw momentum and published local velocity checks; physical energy ungated"}
    Args.output.write_text(json.dumps(Result, indent=2) + "\n", encoding="utf-8")
    print("Verified 54 cases, 648 full-payload frames, and 27 authored pairs")


if __name__ == "__main__":
    Main()
