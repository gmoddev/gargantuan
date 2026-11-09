"""Independently verify Shadow2 source, payloads, fixed gates and frozen controls."""

import argparse
import gzip
import hashlib
import json
import math
from pathlib import Path

from InspectEnergyLedger import Inspect as InspectEnergy
from ReviewPrecisionSurface import VerifyFrameHashes, VerifySha
from ValidateFreeFlight import Inspect as InspectFreeFlight


EnginePin = "7f2fd23cd3b3846e6486e851270af9b1069f734e"
NativePin = "1691460cdcd9bd401c877cc219f38a89071a76ca"
Variants = ("A", "AH", "AS", "ES", "GS")
Kinds = ("xpbd-accounting-moving-control", "xpbd-accounting-static-control",
         "xpbd-ideal-velocity-no-contact-uniform-launch",
         "xpbd-ideal-velocity-pinned-no-contact-launch")


def ReadJson(PathValue):
    return json.loads(PathValue.read_text(encoding="utf-8"))


def CheckReceipt(Case, Source):
    VerifySha(Case / "Receipt.json", Source["ReceiptSha256"])
    Receipt = ReadJson(Case / "Receipt.json")
    if Receipt["ExitCode"] != 0 or Receipt["SolverEnvironment"][
            "GANTRIA_XPBD_IDEAL_VELOCITY"] != ("2" if "-AS-" in Case.name or
            "-ES-" in Case.name or "-GS-" in Case.name else
            "1" if "-AH-" in Case.name else "0"):
        raise ValueError(f"Wrong environment/exit receipt: {Case.name}")
    VerifySha(Case / "stdout.txt", Receipt["StdoutSha256"])
    VerifySha(Case / "stderr.txt", Receipt["StderrSha256"])


def MaximumPinReaction(Case):
    Maximum = 0.0
    with gzip.open(Case / "ContactTraceV1.jsonl.gz", "rt", encoding="utf-8") as Stream:
        for Line in Stream:
            Vector = json.loads(Line)["External"]["XpbdContactAccounting"][
                "PinConstraintImpulseOnClothNs"]
            Maximum = max(Maximum, math.sqrt(sum(Value * Value for Value in Vector)))
    return Maximum


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("Corpus", type=Path)
    Parser.add_argument("FrozenH1", type=Path)
    Parser.add_argument("--output", type=Path, required=True)
    Args = Parser.parse_args()
    Rows = []
    ManifestHash = hashlib.sha256((Args.Corpus / "Manifest.json").read_bytes()).hexdigest()
    for Kind in Kinds:
        for Substeps in (16, 32, 64):
            Definitions = []
            for Variant in Variants:
                Case = Args.Corpus / f"xpbd-{Variant}-{Kind}-N{Substeps}"
                Source = ReadJson(Case / "Source.json")
                if Source["Engine"] != EnginePin or Source["Native"] != NativePin or \
                        Source["ManifestSha256"] != ManifestHash:
                    raise ValueError(f"Wrong source pin/manifest: {Case.name}")
                VerifySha(Case / "ContactFixtureV1.json", Source["FixtureSha256"])
                VerifySha(Case / "ContactTraceV1.jsonl.gz", Source["TraceSha256"])
                CheckReceipt(Case, Source)
                if VerifyFrameHashes(Case) != 12:
                    raise ValueError(f"Wrong frame count/hash: {Case.name}")
                Definition = ReadJson(Case / "Case.json")
                Definition.pop("CaseId")
                Definitions.append(Definition)
                Verdict = ReadJson(Case / "Verdict.json")
                Energy = InspectEnergy(Case)
                Mode = "2" if Variant in ("AS", "ES", "GS") else \
                    "1" if Variant == "AH" else "0"
                if not Verdict["GeometryGatePass"] or \
                        Energy["AccountingIdentityStatus"] != "VERIFIED" or \
                        Energy["VelocityPrecisionMode"] != Mode:
                    raise ValueError(f"Failed geometry/ledger/mode: {Case.name}")
                if Mode == "2" and not Energy["DirectRawMomentumClosurePass"]:
                    raise ValueError(f"Mode2 raw momentum 10ppm gate failed: {Case.name}")
                if Variant in ("A", "AH") and Kind != Kinds[-1]:
                    Frozen = Args.FrozenH1 / Case.name / "FrameHashes.json"
                    if ReadJson(Frozen) != ReadJson(Case / "FrameHashes.json"):
                        raise ValueError(f"Frozen A/H1 payload differs: {Case.name}")
                FreeFlight = None
                PinReaction = None
                if Kind == Kinds[2]:
                    FreeFlight = InspectFreeFlight(Case)
                    if FreeFlight["GatePass"] != (Mode == "2"):
                        raise ValueError(f"Prespecified free-flight gate mismatch: {Case.name}")
                if Kind == Kinds[3]:
                    PinReaction = MaximumPinReaction(Case)
                    if PinReaction <= 0.0:
                        raise ValueError(f"Pin control has no reaction: {Case.name}")
                Rows.append({"Case": Case.name, "Mode": Mode,
                             "GeometryGatePass": Verdict["GeometryGatePass"],
                             "RawMomentumResidual": Energy["MaximumNormalizedRawMomentumResidual"],
                             "RawMomentum10ppmPass": Energy["DirectRawMomentumClosurePass"],
                             "PinNumericalImpulse":
                                 Energy["MaximumNormalizedPinConstraintNumericalImpulse"],
                             "UnclosedPhysicalEnergyResidualJ":
                                 Energy["MaximumAbsoluteUnclosedPhysicalResidualJ"],
                             "MaximumPinReactionImpulseNs": PinReaction,
                             "FreeFlight": FreeFlight})
            if any(Definitions[0] != Definition for Definition in Definitions[1:]):
                raise ValueError(f"Authored comparison differs: {Kind}/N{Substeps}")
    if len(Rows) != 60:
        raise ValueError(f"Expected 60 controls, found {len(Rows)}")
    for Substeps in (16, 32, 64):
        Hashes = [ReadJson(Args.Corpus / f"xpbd-{Variant}-{Kinds[2]}-N{Substeps}" /
                           "FrameHashes.json") for Variant in ("AS", "ES", "GS")]
        if Hashes[0] != Hashes[1] or Hashes[0] != Hashes[2]:
            raise ValueError(f"Mode2 no-contact variants differ: N{Substeps}")
    Result = {"Engine": EnginePin, "Native": NativePin,
              "ManifestSha256": ManifestHash,
              "VerifiedCases": 60, "VerifiedFullPayloadFrames": 720,
              "FrozenAAndH1CasesVerified": 18, "Rows": Rows,
              "Scope": "Fixed analytic free-flight and direct raw cloth momentum gates; other energy/work incomplete"}
    Args.output.write_text(json.dumps(Result, indent=2) + "\n", encoding="utf-8")
    print("Verified 60 cases, 720 full-payload frames, and 18 frozen A/H1 traces")


if __name__ == "__main__":
    Main()
