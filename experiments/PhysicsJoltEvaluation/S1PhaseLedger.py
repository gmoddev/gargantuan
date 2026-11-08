"""Stream native S1 phase traces into kinetic and contact-projection ledgers.

Usage: python S1PhaseLedger.py --runs PATH [--output PATH]
The default output is PATH/S1PhaseLedger.json. This reads existing traces only;
it does not execute a physics binary or decide that a spring analogy is energy.
"""

import argparse
import json
import math
from pathlib import Path


def Squared(Vector):
    return sum(float(Component) * float(Component) for Component in Vector)


def Distance(Left, Right):
    return math.sqrt(sum((float(A) - float(B)) ** 2 for A, B in zip(Left, Right)))


def RigidEnergy(State, Mass, Inertia):
    return 0.5 * Mass * Squared(State["V"]) + 0.5 * Inertia * Squared(State["W"])


def TotalRigidEnergy(States, Mass, Inertia):
    return sum(RigidEnergy(State, Mass, Inertia) for State in States)


def FrameRows(PathName):
    Config = None
    Frames = {}
    with PathName.open(encoding="utf-8") as Source:
        for Line in Source:
            Row = json.loads(Line)
            if Row.get("Type") == "Config":
                Config = Row
            elif Row.get("Type") == "Frame":
                Frames[int(Row["Frame"])] = Row
    if Config is None or not Frames:
        raise ValueError(f"Missing Config or Frame rows: {PathName}")
    return Config, Frames


def FramePath(Runs, Trace):
    Stem = Trace.name.removesuffix(".phases.jsonl")
    if Stem.startswith("Material-"):
        Prefix, Count, Steps = Stem.split("-")
        Candidate = Runs / f"Material-{Count}-{Steps}-Observed.jsonl"
    elif Stem.startswith("Refresh-"):
        _, Count, Steps = Stem.split("-")
        Candidate = Runs / f"Material-{Count}-{Steps}-Refresh.jsonl"
    else:
        Candidate = Runs / f"{Stem}.jsonl"
    if not Candidate.is_file():
        raise FileNotFoundError(f"No published-frame partner for {Trace}: {Candidate}")
    return Candidate


def MatchRigid(Event, States):
    BeforeV = Event["RigidVelocityBefore"]
    BeforeW = Event["RigidAngularBefore"]
    Errors = [Distance(State["V"], BeforeV) + Distance(State["W"], BeforeW) for State in States]
    Choice = min(range(len(States)), key=lambda Index: Errors[Index])
    if Errors[Choice] > 1.0e-3:
        raise ValueError(f"Unmatched rigid precontact velocity: errors={Errors}")
    if len(Errors) > 1 and sorted(Errors)[1] - Errors[Choice] < 1.0e-4:
        raise ValueError(f"Ambiguous rigid identity: errors={Errors}")
    return Choice, Errors[Choice]


def Analyse(Trace, Runs):
    Config, Frames = FrameRows(FramePath(Runs, Trace))
    Mass = float(Config["RigidMass"])
    Inertia = float(Config["SphereInertia"])
    States = None
    PreviousObservedK = None
    FrameNumber = None
    Substep = None
    Substeps = []
    Contacts = []
    ProjectionBefore = None
    FrameSummaries = []
    FrameIntegration = FrameReconstruction = FrameContact = FrameProjectionU = 0.0
    FrameProjectionCount = 0
    FrameBeginK = 0.0
    MaxMatchError = 0.0

    def CompleteFrame():
        if FrameNumber is None:
            return
        if Substep is None or Substep.get("AfterCollisionK") is None:
            raise ValueError(f"Incomplete final substep in frame {FrameNumber}")
        Observed = Frames[FrameNumber]
        EndK = float(Observed["KineticEnergy"])
        PredictedDelta = FrameIntegration + FrameReconstruction + FrameContact
        FrameSummaries.append({
            "Frame": FrameNumber,
            "PreviousObservedK": PreviousObservedK,
            "FirstStageK": FrameBeginK,
            "ObservedEndK": EndK,
            "IntegrationDeltaJ": FrameIntegration,
            "ReconstructionDeltaJ": FrameReconstruction,
            "DynamicContactPairDeltaJ": FrameContact,
            "ContactProjectionUhatDeltaJ": FrameProjectionU,
            "ContactProjectionCount": FrameProjectionCount,
            "PredictedDeltaJ": PredictedDelta,
            "ObservedDeltaJ": EndK - PreviousObservedK,
            "FrameClosureResidualJ": (EndK - PreviousObservedK) - PredictedDelta,
            "FirstStageResidualJ": FrameBeginK - PreviousObservedK,
            "TraceEndResidualJ": Substep["AfterCollisionK"] - EndK,
            "PublishedPositiveEdgeEstimateJ": float(Observed["CompliantEdgeElasticEstimate"]),
        })
        # Do not silently force the event-tracked rigids to the published frame.
        # Their difference is retained as the closure residual above.

    with Trace.open(encoding="utf-8") as Source:
        for Line in Source:
            Row = json.loads(Line)
            Kind = Row.get("Type")
            if Kind not in {"Stage", "Reconstruction", "DynamicContact", "ContactProjection"}:
                continue
            ThisFrame = int(Row["Frame"])
            if FrameNumber is None or ThisFrame != FrameNumber:
                if FrameNumber is not None:
                    CompleteFrame()
                    if ThisFrame != FrameNumber + 1:
                        raise ValueError(f"Non-contiguous trace frames: {FrameNumber} to {ThisFrame}")
                    PreviousObservedK = float(Frames[FrameNumber]["KineticEnergy"])
                else:
                    Previous = Frames.get(ThisFrame - 1)
                    if Previous is not None:
                        States = [{"V": list(Body["V"]), "W": list(Body["W"])} for Body in Previous["Rigids"]]
                        PreviousObservedK = float(Previous["KineticEnergy"])
                    elif ThisFrame == 0:
                        Speed = float(Config.get("Speed", 0.0))
                        States = [{"V": [0.0, 0.0, Speed if Index == 0 else -Speed], "W": [0.0, 0.0, 0.0]}
                                  for Index in range(int(Config["RigidCount"]))]
                        Seed = [float(Config.get("SeedX", 0.0)), float(Config.get("SeedY", 0.0)), float(Config.get("Seed", 0.0))]
                        PreviousObservedK = TotalRigidEnergy(States, Mass, Inertia) + 0.05 * Squared(Seed)
                    else:
                        raise ValueError(f"No preceding published frame for trace starting at {ThisFrame}")
                FrameNumber = ThisFrame
                FrameIntegration = FrameReconstruction = FrameContact = FrameProjectionU = 0.0
                FrameProjectionCount = 0
                FrameBeginK = 0.0
                Substep = None
            if Kind == "Stage":
                Phase = Row["Phase"]
                if Phase == "BeforeIntegration":
                    if Substep is not None and Substep.get("AfterCollisionK") is None:
                        raise ValueError(f"Incomplete substep before frame {FrameNumber} next integration")
                    Ordinal = 1 if Substep is None or Substep["Frame"] != FrameNumber else Substep["Ordinal"] + 1
                    StepsPerOuter = int(Config["Iterations"])
                    RigidK = TotalRigidEnergy(States, Mass, Inertia)
                    Substep = {
                        "Frame": FrameNumber, "Ordinal": Ordinal,
                        "OuterStep": (Ordinal - 1) // StepsPerOuter,
                        "LocalIteration": (Ordinal - 1) % StepsPerOuter + 1,
                        "IntegrationDt": float(Row["IntegrationDt"]),
                        "BeforeIntegrationSoftK": float(Row["SoftKE"]),
                        "BeforeIntegrationRigidK": RigidK,
                        "BeforeIntegrationK": float(Row["SoftKE"]) + RigidK,
                        "BeforeIntegrationUhat": float(Row["CompliantEdgeElasticEstimate"]),
                        "IntegrationDeltaJ": 0.0, "ReconstructionDeltaJ": 0.0,
                        "DynamicContactPairDeltaJ": 0.0,
                        "ContactProjectionUhatDeltaJ": 0.0,
                        "ContactProjectionCount": 0, "DynamicContactCount": 0,
                    }
                    if Ordinal == 1:
                        FrameBeginK = Substep["BeforeIntegrationK"]
                    Substeps.append(Substep)
                elif Substep is None:
                    raise ValueError(f"Stage without BeforeIntegration in frame {FrameNumber}")
                elif Phase == "AfterIntegration":
                    Substep["AfterIntegrationSoftK"] = float(Row["SoftKE"])
                    Substep["AfterIntegrationUhat"] = float(Row["CompliantEdgeElasticEstimate"])
                    Delta = Substep["AfterIntegrationSoftK"] - Substep["BeforeIntegrationSoftK"]
                    Substep["IntegrationDeltaJ"] = Delta
                    FrameIntegration += Delta
                    Substep["AfterIntegrationCandidateK"] = Row.get("ReconstructedCandidateKE")
                elif Phase == "BeforeCollision":
                    Substep["BeforeCollisionSoftK"] = float(Row["SoftKE"])
                    Substep["BeforeCollisionUhat"] = float(Row["CompliantEdgeElasticEstimate"])
                    Substep["BeforeCollisionCandidateK"] = Row.get("ReconstructedCandidateKE")
                    BeforeCandidate = Substep.get("AfterIntegrationCandidateK")
                    AfterCandidate = Substep["BeforeCollisionCandidateK"]
                    if BeforeCandidate is not None and AfterCandidate is not None:
                        Substep["EdgeCandidateDeltaJ"] = float(AfterCandidate) - float(BeforeCandidate)
                    Substep["EdgeUhatDeltaJ"] = Substep["BeforeCollisionUhat"] - Substep["AfterIntegrationUhat"]
                elif Phase == "AfterCollision":
                    Substep["AfterCollisionSoftK"] = float(Row["SoftKE"])
                    Substep["AfterCollisionUhat"] = float(Row["CompliantEdgeElasticEstimate"])
                    Substep["AfterCollisionK"] = Substep["AfterCollisionSoftK"] + TotalRigidEnergy(States, Mass, Inertia)
                    Substep["AfterCollisionHhat"] = Substep["AfterCollisionK"] + Substep["AfterCollisionUhat"]
                    Substep["ReconstructionVsEdgeCandidateResidualJ"] = (
                        Substep["ReconstructionDeltaJ"] - Substep.get("EdgeCandidateDeltaJ", 0.0))
                    Substep["SubstepClosureResidualJ"] = (
                        Substep["AfterCollisionK"] - Substep["BeforeIntegrationK"]
                        - Substep["IntegrationDeltaJ"] - Substep["ReconstructionDeltaJ"]
                        - Substep["DynamicContactPairDeltaJ"])
            elif Kind == "Reconstruction":
                if Substep is None:
                    raise ValueError("Reconstruction outside substep")
                Delta = float(Row["KineticChange"])
                Substep["ReconstructionDeltaJ"] += Delta
                FrameReconstruction += Delta
            elif Kind == "DynamicContact":
                if Substep is None:
                    raise ValueError("DynamicContact outside substep")
                Index, Error = MatchRigid(Row, States)
                MaxMatchError = max(MaxMatchError, Error)
                ParticleMass = 1.0 / float(Row["ParticleInverseMass"])
                BeforeParticle = Row["ParticleVelocityBeforeImpulse"]
                AfterParticle = Row["ParticleVelocityAfterImpulse"]
                BeforeRigid = {"V": Row["RigidVelocityBefore"], "W": Row["RigidAngularBefore"]}
                AfterRigid = {"V": Row["RigidVelocityAfter"], "W": Row["RigidAngularAfter"]}
                EventDelta = (0.5 * ParticleMass * (Squared(AfterParticle) - Squared(BeforeParticle))
                              + RigidEnergy(AfterRigid, Mass, Inertia) - RigidEnergy(BeforeRigid, Mass, Inertia))
                Substep["DynamicContactPairDeltaJ"] += EventDelta
                Substep["DynamicContactCount"] += 1
                FrameContact += EventDelta
                States[Index] = AfterRigid
            elif Kind == "ContactProjection":
                if Substep is None:
                    raise ValueError("ContactProjection outside substep")
                if Row["Phase"] == "Before":
                    if ProjectionBefore is not None:
                        raise ValueError("Unpaired ContactProjection Before")
                    ProjectionBefore = Row
                elif Row["Phase"] == "After":
                    Before = ProjectionBefore
                    if Before is None or int(Before["Vertex"]) != int(Row["Vertex"]) or int(Before["Frame"]) != ThisFrame:
                        raise ValueError("ContactProjection Before/After mismatch")
                    DeltaU = float(Row["CompliantEdgeElasticEstimate"]) - float(Before["CompliantEdgeElasticEstimate"])
                    Depth = float(Before["PlaneDepth"])
                    TrueBefore = float(Before["TrueFrozenSphereGap"])
                    TrueAfter = float(Row["TrueFrozenSphereGap"])
                    Contacts.append({
                        "Frame": FrameNumber, "Ordinal": Substep["Ordinal"],
                        "OuterStep": Substep["OuterStep"], "LocalIteration": Substep["LocalIteration"],
                        "Vertex": int(Row["Vertex"]), "CollisionShapeIndex": int(Row["CollisionShapeIndex"]),
                        "PlaneDepth": Depth, "TrueFrozenSphereGapBefore": TrueBefore,
                        "TrueFrozenSphereGapAfter": TrueAfter,
                        "TrueFrozenSpherePenetrationBefore": max(0.0, -TrueBefore),
                        "PlaneDepthMinusTruePenetration": Depth - max(0.0, -TrueBefore),
                        "ProjectionPositionDelta": [float(A) - float(B) for A, B in zip(Row["P"], Before["P"])],
                        "UhatBeforeJ": float(Before["CompliantEdgeElasticEstimate"]),
                        "UhatAfterJ": float(Row["CompliantEdgeElasticEstimate"]),
                        "ProjectionUhatDeltaJ": DeltaU,
                    })
                    Substep["ContactProjectionUhatDeltaJ"] += DeltaU
                    Substep["ContactProjectionCount"] += 1
                    FrameProjectionU += DeltaU
                    FrameProjectionCount += 1
                    ProjectionBefore = None
    if ProjectionBefore is not None:
        raise ValueError("Unpaired final ContactProjection Before")
    CompleteFrame()
    for Index, Item in enumerate(Substeps[:-1]):
        Next = Substeps[Index + 1]
        Item["NextSubstepReconstructionDeltaJ"] = Next["ReconstructionDeltaJ"]
        Item["NextSubstepFrame"] = Next["Frame"]
        Item["NextSubstepOrdinal"] = Next["Ordinal"]
    return {
        "Trace": Trace.name, "FrameFile": FramePath(Runs, Trace).name,
        "Config": {Key: Config.get(Key) for Key in ("RigidCount", "Steps", "Iterations", "Compliance", "Friction", "ProjectionPasses", "RefreshSpherePlane")},
        "Scope": "Kinetic ledger closes measured soft/rigid velocities; Uhat is positive-compliance edge spring analogy, not conserved potential or full work balance",
        "MaximumRigidIdentityMatchError": MaxMatchError,
        "MaximumFrameClosureResidualJ": max((abs(Item["FrameClosureResidualJ"]) for Item in FrameSummaries), default=0.0),
        "MaximumSubstepClosureResidualJ": max((abs(Item["SubstepClosureResidualJ"]) for Item in Substeps), default=0.0),
        "Frames": FrameSummaries, "Substeps": Substeps, "ContactProjections": Contacts,
    }


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--runs", type=Path, required=True)
    Parser.add_argument("--output", type=Path)
    Args = Parser.parse_args()
    Runs = Args.runs.resolve()
    Traces = sorted(Runs.glob("*.phases.jsonl"))
    if not Traces:
        raise SystemExit(f"No .phases.jsonl files in {Runs}")
    Result = {"Runs": str(Runs), "Ledgers": [Analyse(Trace, Runs) for Trace in Traces]}
    Output = Args.output.resolve() if Args.output else Runs / "S1PhaseLedger.json"
    Output.write_text(json.dumps(Result, indent=2, allow_nan=False), encoding="utf-8")
    for Ledger in Result["Ledgers"]:
        print(f"{Ledger['Trace']}: {len(Ledger['Frames'])} frames, "
              f"{len(Ledger['ContactProjections'])} projections, "
              f"max frame closure {Ledger['MaximumFrameClosureResidualJ']:.6g} J, "
              f"max substep closure {Ledger['MaximumSubstepClosureResidualJ']:.6g} J")
    print(Output)


if __name__ == "__main__":
    Main()
