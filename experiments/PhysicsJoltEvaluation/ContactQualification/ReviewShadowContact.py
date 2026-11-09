"""Reduce the 24-case Shadow2 contact corpus across preserved and recovered roots."""

import argparse
import gzip
import hashlib
from itertools import zip_longest
import json
from pathlib import Path

from ReviewPackagedCorpus import CheckFixtureFaces, Mirror
from ReviewPrecisionSurface import VerifyFrameHashes, VerifySha
from ValidateContactTrace import Inspect as InspectGeometry


EnginePin = "7f2fd23cd3b3846e6486e851270af9b1069f734e"
NativePin = "1691460cdcd9bd401c877cc219f38a89071a76ca"


def ReadJson(PathValue):
    return json.loads(PathValue.read_text(encoding="utf-8"))


def Hash(PathValue):
    return hashlib.sha256(PathValue.read_bytes()).hexdigest()


def VerifyRecovery(Original, Recovered):
    Name = "xpbd-GS-original128-plus10um"
    OriginalCase = Original / Name
    RecoveredCase = Recovered / Name
    Receipt = ReadJson(RecoveredCase / "RecoveryReceipt.json")
    if Receipt["Case"] != Name or Receipt["OriginalDataset"] != Original.name or \
            Receipt["OriginalProcessExit"] != 0 or \
            Receipt["RawTraceSha256"] != Hash(OriginalCase / "ContactTraceV1.jsonl") or \
            Receipt["PartialGzipSha256"] != Hash(OriginalCase / "ContactTraceV1.jsonl.gz") or \
            Receipt["PartialGzipSha256"] != Hash(RecoveredCase / "FailedCompression.partial.gz") or \
            Receipt["RecoveredGzipSha256"] != Hash(RecoveredCase / "ContactTraceV1.jsonl.gz"):
        raise ValueError("Recovered plus trace is not linked to preserved raw output")
    for FileName in ("Case.json", "ContactFixtureV1.json", "Receipt.json"):
        if Hash(OriginalCase / FileName) != Hash(RecoveredCase / FileName):
            raise ValueError(f"Recovered plus {FileName} differs from original")
    Source = ReadJson(RecoveredCase / "Source.json")
    if Source["UncompressedTraceSha256"] != Receipt["RawTraceSha256"] or \
            Source["TraceSha256"] != Receipt["RecoveredGzipSha256"]:
        raise ValueError("Recovered source hash contradicts original raw output")
    return Receipt


def VerifyMirrorMetadata(First, Second):
    Frames = 0
    with gzip.open(First / "ContactTraceV1.jsonl.gz", "rt", encoding="utf-8") as A, \
            gzip.open(Second / "ContactTraceV1.jsonl.gz", "rt", encoding="utf-8") as B:
        for Frames, (FirstLine, SecondLine) in enumerate(zip_longest(A, B), 1):
            if FirstLine is None or SecondLine is None:
                raise ValueError("Mirror frame count differs")
            FirstRow, SecondRow = json.loads(FirstLine), json.loads(SecondLine)
            if FirstRow["Frame"] != SecondRow["Frame"] or \
                    FirstRow["TimeSeconds"] != SecondRow["TimeSeconds"] or \
                    len(FirstRow["ColliderStates"]) != len(SecondRow["ColliderStates"]):
                raise ValueError("Mirror frame timing or collider count differs")
            for FirstCollider, SecondCollider in zip(
                    FirstRow["ColliderStates"], SecondRow["ColliderStates"]):
                if FirstCollider["Id"] != SecondCollider["Id"] or \
                        FirstCollider["OrientationWxyz"] != SecondCollider["OrientationWxyz"] or \
                        FirstCollider["VelocityMethod"] != SecondCollider["VelocityMethod"]:
                    raise ValueError("Mirror sphere metadata differs")
    return Frames


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("ArtifactRoot", type=Path)
    Parser.add_argument("--output", type=Path, required=True)
    Args = Parser.parse_args()
    Original = Args.ArtifactRoot / "ShadowContact-20261109T120323"
    Remainder = Args.ArtifactRoot / "ShadowRemainder-20261109T122111"
    Recovered = Args.ArtifactRoot / "ShadowRecovered"
    Manifests = {Root.name: Hash(Root / "Manifest.json")
                 for Root in (Original, Remainder)}
    Recovery = VerifyRecovery(Original, Recovered)
    Definitions = ReadJson(Original / "Manifest.json")["Definitions"]
    if len(Definitions) != 12:
        raise ValueError("Expected 12 authored contact definitions")
    Cases = {}
    Rows = []
    for Mode in ("ES", "GS"):
        for Definition in Definitions:
            Name = f"xpbd-{Mode}-{Definition['CaseId']}"
            Root = Remainder if Name == "xpbd-GS-original128-minus10um" else \
                Recovered if Name == "xpbd-GS-original128-plus10um" else Original
            Case = Root / Name
            Source = ReadJson(Case / "Source.json")
            ExpectedManifest = Manifests[Original.name] if Root == Recovered else \
                Manifests[Root.name]
            if Source["Engine"] != EnginePin or Source["Native"] != NativePin or \
                    Source["SolverVariant"] != f"xpbd:{Mode}" or \
                    Source["ManifestSha256"] != ExpectedManifest:
                raise ValueError(f"Wrong source/manifest pin: {Name}")
            VerifySha(Case / "ContactFixtureV1.json", Source["FixtureSha256"])
            VerifySha(Case / "ContactTraceV1.jsonl.gz", Source["TraceSha256"])
            VerifySha(Case / "Receipt.json", Source["ReceiptSha256"])
            Receipt = ReadJson(Case / "Receipt.json")
            if Receipt["ExitCode"] != 0 or \
                    Receipt["SolverEnvironment"]["GANTRIA_XPBD_IDEAL_VELOCITY"] != "2":
                raise ValueError(f"Wrong solver environment or exit: {Name}")
            VerifySha(Case / "stdout.txt", Receipt["StdoutSha256"])
            VerifySha(Case / "stderr.txt", Receipt["StderrSha256"])
            Authored = ReadJson(Case / "Case.json")
            ExpectedDefinition = dict(Definition)
            ExpectedDefinition.pop("CaseId")
            if Authored.pop("CaseId") != Name or Authored.pop("Backend") != "xpbd" or \
                    Authored != ExpectedDefinition:
                raise ValueError(f"Authored case differs from original manifest: {Name}")
            CheckFixtureFaces(Case)
            Verdict = InspectGeometry(Case)
            if Verdict != ReadJson(Case / "Verdict.json"):
                raise ValueError(f"Independent geometry reduction differs: {Name}")
            Frames = VerifyFrameHashes(Case)
            if Frames != Verdict["Frames"]:
                raise ValueError(f"Frame count differs: {Name}")
            Cases[Name] = Case
            Rows.append({"Case": Name, "EvidenceRoot": Root.name,
                         "Frames": Frames, "GeometryGatePass": Verdict["GeometryGatePass"],
                         "MaximumAxialStrain": Verdict["MaximumAxialStrain"],
                         "MaximumSpherePenetrationM": Verdict["MaximumSpherePenetrationM"],
                         "ProjectedOrientationFlipInstances":
                             Verdict["ProjectedOrientationFlipInstances"],
                         "FullPhysicalQualification": Verdict["FullPhysicalQualification"],
                         "TraceSha256": Source["TraceSha256"]})
    Mirrors = {}
    for Mode in ("ES", "GS"):
        for Positive, Negative in (("plane-plus10um", "plane-minus10um"),
                                   ("plane-plus1cm", "plane-minus1cm"),
                                   ("original128-plus10um", "original128-minus10um")):
            First, Second = f"xpbd-{Mode}-{Positive}", f"xpbd-{Mode}-{Negative}"
            Result = Mirror(Cases[First], Cases[Second])
            MetadataFrames = VerifyMirrorMetadata(Cases[First], Cases[Second])
            if Result["Frames"] != MetadataFrames or \
                    Result["Frames"] != ReadJson(Cases[First] / "Verdict.json")["Frames"]:
                raise ValueError(f"Reflection frame count differs: {First}")
            Result["TimeAndSphereOrientationMetadataEqual"] = True
            Mirrors[f"{Mode}:{Positive}/{Negative}"] = Result
    RecoveredMirror = Mirrors["GS:original128-plus10um/original128-minus10um"]
    if any(RecoveredMirror[Key] != 0 for Key in (
            "XYBitMismatches", "NonzeroZReflectionBitMismatches",
            "ColliderXYBitMismatches", "ColliderZReflectionBitMismatches",
            "MaximumReflectionErrorM")):
        raise ValueError("Recovered GS 128² signed reflection failed")
    if len(Rows) != 24:
        raise ValueError(f"Expected 24 cases, found {len(Rows)}")
    Result = {"Engine": EnginePin, "Native": NativePin,
              "SourceManifestSha256": Manifests, "RecoveredPlusReceipt": Recovery,
              "VerifiedCases": len(Rows),
              "VerifiedFullPayloadFrames": sum(Row["Frames"] for Row in Rows),
              "Rows": Rows, "Mirrors": Mirrors,
              "Scope": "Geometry and signed reflection, not full energy/work/rigid impulse qualification"}
    Args.output.write_text(json.dumps(Result, indent=2) + "\n", encoding="utf-8")
    print(f"Verified {len(Rows)} cases, {Result['VerifiedFullPayloadFrames']} "
          "full-payload frames, six signed-reflection pairs")


if __name__ == "__main__":
    Main()
