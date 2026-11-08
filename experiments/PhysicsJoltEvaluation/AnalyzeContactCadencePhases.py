"""Reduce native Jolt phase traces to an operation-level K plus edge-U ledger.

This analyzes preserved diagnostic rows only. Only AfterCollision states count as
accepted soft internal steps. Intermediate operation crossings are diagnostics.
The positive-compliance edge U is a specified Hookean reference model, not a
claim about Jolt's hidden solver energy or complete contact work.
"""

import argparse
import json
import math
from pathlib import Path


def Squared(Vector):
    return sum(float(Value) * float(Value) for Value in Vector)


def RigidKinetic(Velocity, Angular, Mass, Inertia):
    return 0.5 * Mass * Squared(Velocity) + 0.5 * Inertia * Squared(Angular)


def ReadPublished(PathName):
    Config = None
    Frames = {}
    with PathName.open(encoding="utf-8") as Source:
        for Line in Source:
            Row = json.loads(Line)
            if Row.get("Type") == "Config":
                assert Config is None, "Duplicate Config"
                Config = Row
            elif Row.get("Type") == "Frame":
                Frames[int(Row["Frame"])] = Row
    if Config is None or not Frames:
        raise ValueError("Published native JSONL lacks Config or Frame rows")
    return Config, Frames


def Position(Row, Ordinal, Iterations):
    return {"Frame": int(Row["Frame"]), "Ordinal": Ordinal,
            "OuterCollisionStep": (Ordinal - 1) // Iterations,
            "LocalIteration": (Ordinal - 1) % Iterations + 1,
            "Vertex": Row.get("Vertex")}


def Analyse(PublishedPath, TracePath):
    Config, Frames = ReadPublished(PublishedPath)
    Mass = float(Config["RigidMass"])
    Inertia = float(Config["SphereInertia"])
    Count = int(Config["RigidCount"])
    Iterations = int(Config["Iterations"])
    Steps = int(Config["Steps"])
    # The older S1 diagnostic publishes the authored state through frame zero,
    # but does not repeat initial energy in Config. Its frame-zero fixture has
    # no contact and zero soft motion, so it supplies the same initial budget.
    InitialFrame = Frames.get(0)
    if "InitialKineticEnergy" in Config:
        InitialK = float(Config["InitialKineticEnergy"])
        InitialU = float(Config["InitialCompliantEdgeElasticEstimate"])
        InitialBudgetSource = "Config authored initial state"
    elif (InitialFrame is not None and not InitialFrame["Contacts"] and not InitialFrame["SoftKE"]
          and all(float(Config.get(Key, 0.0)) == 0.0 for Key in ("Seed", "SeedX", "SeedY"))):
        InitialK = float(InitialFrame["KineticEnergy"])
        InitialU = float(InitialFrame["CompliantEdgeElasticEstimate"])
        InitialBudgetSource = "Native frame zero; no contact and zero soft motion"
    else:
        raise ValueError("No defensible authored initial energy budget")
    KineticCeiling = 1.05 * InitialK + 1.0e-5
    ModelCeiling = 1.05 * (InitialK + InitialU) + 1.0e-5
    PositiveCompliance = (Config.get("AllEdgesPositiveCompliance") is True or
                          float(Config.get("Compliance", 0.0)) > 0.0 and
                          float(Config.get("BendCompliance", 0.0)) > 0.0)
    if not Config.get("ZeroGravity") or not PositiveCompliance:
        raise ValueError("Closed positive-compliance zero-gravity fixture required")
    if not Config.get("Dynamic") or not Config.get("Pinned"):
        raise ValueError("Dynamic sphere and fixed pins required")

    # With one sphere there is no rigid-rigid pair. In C2, fixed horizontal
    # center separation exceeds two radii for every possible z separation.
    MinimumHorizontalPairDistance = None
    if Count > 1:
        for Frame in Frames.values():
            Rigids = Frame["Rigids"]
            for Left in range(len(Rigids)):
                for Right in range(Left + 1, len(Rigids)):
                    A, B = Rigids[Left]["P"], Rigids[Right]["P"]
                    Horizontal = math.hypot(float(A[0]) - float(B[0]), float(A[1]) - float(B[1]))
                    MinimumHorizontalPairDistance = (Horizontal if MinimumHorizontalPairDistance is None else
                                                     min(MinimumHorizontalPairDistance, Horizontal))
        if MinimumHorizontalPairDistance <= 0.9:
            raise ValueError("Rigid-rigid contact cannot be excluded by horizontal separation")

    Substeps = []
    ProjectionEvents = []
    DynamicPairEvents = []
    FrameSummaries = []
    CurrentFrame = None
    CurrentStep = None
    PendingProjection = None
    CurrentSoftK = CurrentU = RigidK = None
    FrameStartRigidResidual = 0.0
    FirstAcceptedModelFailure = FirstAcceptedKineticFailure = None
    FirstTransientModelCrossing = FirstTransientKineticCrossing = None
    FirstAcceptedFailureEvents = None
    FirstAcceptedKineticFailureEvents = None
    MaximumKineticClosure = MaximumModelClosure = 0.0
    MaximumFrameKClosure = MaximumFrameUClosure = 0.0
    MaximumInterStepSoftKJump = MaximumInterStepEdgeUJump = 0.0
    MaximumProjectionBeforeUResidual = 0.0
    MinimumDynamicPairDelta = math.inf
    MaximumDynamicPairDelta = -math.inf
    FrameStartK = FrameStartU = 0.0
    FrameOrdinal = 0
    LastAcceptedSoftK = LastAcceptedU = None

    def RecordEvent(Kind, DeltaK, DeltaU, Row, Detail=None):
        nonlocal FirstTransientModelCrossing, FirstTransientKineticCrossing
        BeforeK = CurrentSoftK + RigidK - DeltaK
        BeforeU = CurrentU - DeltaU
        AfterK = CurrentSoftK + RigidK
        AfterU = CurrentU
        Event = {"Operation": Kind, "DeltaKJ": DeltaK, "DeltaEdgeUJ": DeltaU,
                 "KBeforeJ": BeforeK, "KAfterJ": AfterK,
                 "HBeforeJ": BeforeK + BeforeU, "HAfterJ": AfterK + AfterU}
        if Detail:
            Event.update(Detail)
        CurrentStep["Events"].append(Event)
        if FirstTransientModelCrossing is None and Event["HBeforeJ"] <= ModelCeiling < Event["HAfterJ"]:
            FirstTransientModelCrossing = {**Position(Row, CurrentStep["Ordinal"], Iterations),
                                           "Operation": Kind, "HBeforeJ": Event["HBeforeJ"],
                                           "HAfterJ": Event["HAfterJ"], "Detail": Detail}
        if FirstTransientKineticCrossing is None and BeforeK <= KineticCeiling < AfterK:
            FirstTransientKineticCrossing = {**Position(Row, CurrentStep["Ordinal"], Iterations),
                                             "Operation": Kind, "KBeforeJ": BeforeK,
                                             "KAfterJ": AfterK, "Detail": Detail}

    def CompleteStep(Row):
        nonlocal CurrentStep, MaximumKineticClosure, MaximumModelClosure
        nonlocal FirstAcceptedModelFailure, FirstAcceptedKineticFailure
        nonlocal FirstAcceptedFailureEvents, FirstAcceptedKineticFailureEvents
        nonlocal LastAcceptedSoftK, LastAcceptedU
        if CurrentStep is None:
            raise ValueError("AfterCollision without active substep")
        AcceptedSoftK = float(Row["SoftKE"])
        AcceptedU = float(Row["CompliantEdgeElasticEstimate"])
        AcceptedK = AcceptedSoftK + RigidK
        AcceptedH = AcceptedK + AcceptedU
        PredictedK = (CurrentStep["BeforeKJ"] + CurrentStep["IntegrationDeltaKJ"] +
                      CurrentStep["EdgeStageDeltaKJ"] + CurrentStep["ReconstructionDeltaKJ"] +
                      CurrentStep["DynamicPairDeltaKJ"])
        PredictedU = (CurrentStep["BeforeEdgeUJ"] + CurrentStep["IntegrationDeltaUJ"] +
                      CurrentStep["EdgeStageDeltaUJ"] + CurrentStep["ProjectionDeltaUJ"])
        KResidual = AcceptedK - PredictedK
        UResidual = AcceptedU - PredictedU
        MaximumKineticClosure = max(MaximumKineticClosure, abs(KResidual))
        MaximumModelClosure = max(MaximumModelClosure, abs(UResidual))
        CurrentStep.update({"AfterCollisionKJ": AcceptedK, "AfterCollisionEdgeUJ": AcceptedU,
                            "AfterCollisionHJ": AcceptedH,
                            "KineticClosureResidualJ": KResidual,
                            "EdgeUClosureResidualJ": UResidual,
                            "AcceptedKineticExceeded": AcceptedK > KineticCeiling,
                            "AcceptedModelExceeded": AcceptedH > ModelCeiling,
                            "ContactProjectionCount": CurrentStep["ContactProjectionCount"],
                            "DynamicContactCount": CurrentStep["DynamicContactCount"]})
        if FirstAcceptedModelFailure is None and AcceptedH > ModelCeiling:
            FirstAcceptedModelFailure = {"Frame": CurrentFrame, "Ordinal": CurrentStep["Ordinal"],
                                         "OuterCollisionStep": CurrentStep["OuterCollisionStep"],
                                         "LocalIteration": CurrentStep["LocalIteration"],
                                         "AcceptedHJ": AcceptedH, "CeilingJ": ModelCeiling}
            FirstAcceptedFailureEvents = CurrentStep["Events"]
        if FirstAcceptedKineticFailure is None and AcceptedK > KineticCeiling:
            FirstAcceptedKineticFailure = {"Frame": CurrentFrame, "Ordinal": CurrentStep["Ordinal"],
                                           "OuterCollisionStep": CurrentStep["OuterCollisionStep"],
                                           "LocalIteration": CurrentStep["LocalIteration"],
                                           "AcceptedKJ": AcceptedK, "CeilingJ": KineticCeiling}
            FirstAcceptedKineticFailureEvents = CurrentStep["Events"]
            if FirstAcceptedFailureEvents is None:
                FirstAcceptedFailureEvents = CurrentStep["Events"]
        LastAcceptedSoftK, LastAcceptedU = AcceptedSoftK, AcceptedU
        CurrentStep.pop("Events")
        Substeps.append(CurrentStep)
        CurrentStep = None

    def CompleteFrame():
        nonlocal MaximumFrameKClosure, MaximumFrameUClosure
        if CurrentFrame is None:
            return
        if CurrentStep is not None or PendingProjection is not None:
            raise ValueError("Incomplete final phase at frame boundary")
        Frame = Frames[CurrentFrame]
        End = Substeps[-1]
        PublishedK = float(Frame["KineticEnergy"])
        PublishedU = float(Frame["CompliantEdgeElasticEstimate"])
        KResidual = PublishedK - End["AfterCollisionKJ"]
        UResidual = PublishedU - End["AfterCollisionEdgeUJ"]
        MaximumFrameKClosure = max(MaximumFrameKClosure, abs(KResidual))
        MaximumFrameUClosure = max(MaximumFrameUClosure, abs(UResidual))
        FrameSummaries.append({"Frame": CurrentFrame, "Substeps": FrameOrdinal,
                               "FirstStageKJ": FrameStartK, "FirstStageEdgeUJ": FrameStartU,
                               "EndAcceptedKJ": End["AfterCollisionKJ"],
                               "EndAcceptedHJ": End["AfterCollisionHJ"],
                               "PublishedKJ": PublishedK, "PublishedEdgeUJ": PublishedU,
                               "PublishedKClosureJ": KResidual, "PublishedEdgeUClosureJ": UResidual,
                               "RigidStartResidualJ": FrameStartRigidResidual})

    with TracePath.open(encoding="utf-8") as Source:
        for Line in Source:
            Row = json.loads(Line)
            Kind = Row.get("Type")
            if Kind not in ("Stage", "Reconstruction", "ContactProjection", "DynamicContact"):
                continue
            Frame = int(Row["Frame"])
            if CurrentFrame != Frame:
                if CurrentFrame is not None:
                    CompleteFrame()
                    if Frame != CurrentFrame + 1:
                        raise ValueError("Trace frames must be consecutive")
                Previous = Frames.get(Frame - 1)
                if Previous is not None:
                    RigidK = sum(RigidKinetic(R["V"], R["W"], Mass, Inertia) for R in Previous["Rigids"])
                    FrameStartRigidResidual = (RigidK -
                        (float(Previous["KineticEnergy"]) - float(Previous["SoftKE"])))
                elif Frame == 0:
                    RigidK = InitialK - 0.05 * (float(Config.get("Seed", 0.0)) ** 2 +
                                                 float(Config.get("SeedX", 0.0)) ** 2 +
                                                 float(Config.get("SeedY", 0.0)) ** 2)
                    FrameStartRigidResidual = 0.0
                else:
                    raise ValueError("Missing published frame before trace start")
                CurrentFrame = Frame
                FrameStartK = FrameStartU = 0.0
                FrameOrdinal = 0
            if Kind == "Stage" and Row["Phase"] == "BeforeIntegration":
                if CurrentStep is not None:
                    raise ValueError("New integration before prior AfterCollision")
                FrameOrdinal += 1
                Ordinal = FrameOrdinal
                if Ordinal > Steps * Iterations:
                    raise ValueError("Too many soft integrations in frame")
                LocalIteration = (Ordinal - 1) % Iterations + 1
                if int(Row["InternalIteration"]) != LocalIteration:
                    raise ValueError("Native local iteration differs from flattened order")
                CurrentSoftK = float(Row["SoftKE"])
                CurrentU = float(Row["CompliantEdgeElasticEstimate"])
                SoftKJump = (CurrentSoftK - LastAcceptedSoftK if LastAcceptedSoftK is not None else None)
                EdgeUJump = (CurrentU - LastAcceptedU if LastAcceptedU is not None else None)
                if SoftKJump is not None:
                    MaximumInterStepSoftKJump = max(MaximumInterStepSoftKJump, abs(SoftKJump))
                    MaximumInterStepEdgeUJump = max(MaximumInterStepEdgeUJump, abs(EdgeUJump))
                K = CurrentSoftK + RigidK
                CurrentStep = {"Frame": Frame, "Ordinal": Ordinal,
                               "OuterCollisionStep": (Ordinal - 1) // Iterations,
                               "LocalIteration": LocalIteration,
                               "BeforeKJ": K, "BeforeEdgeUJ": CurrentU,
                               "BeforeHJ": K + CurrentU,
                               "InterStepSoftKJumpJ": SoftKJump,
                               "InterStepEdgeUJumpJ": EdgeUJump,
                               "IntegrationDt": float(Row["IntegrationDt"]),
                               "IntegrationDeltaKJ": 0.0, "IntegrationDeltaUJ": 0.0,
                               "EdgeStageDeltaKJ": 0.0, "EdgeStageDeltaUJ": 0.0,
                               "EdgeCandidateDeltaKJ": None,
                               "ReconstructionDeltaKJ": 0.0,
                               "DynamicPairDeltaKJ": 0.0, "ProjectionDeltaUJ": 0.0,
                               "ContactProjectionCount": 0, "DynamicContactCount": 0,
                               "Events": []}
                if Ordinal == 1:
                    FrameStartK = K
                    FrameStartU = CurrentU
                continue
            if CurrentStep is None:
                raise ValueError(f"{Kind} outside active substep")
            if Kind == "Stage":
                Phase = Row["Phase"]
                if Phase == "AfterIntegration":
                    NewSoftK = float(Row["SoftKE"])
                    NewU = float(Row["CompliantEdgeElasticEstimate"])
                    DeltaK, DeltaU = NewSoftK - CurrentSoftK, NewU - CurrentU
                    CurrentSoftK, CurrentU = NewSoftK, NewU
                    CurrentStep["IntegrationDeltaKJ"] = DeltaK
                    CurrentStep["IntegrationDeltaUJ"] = DeltaU
                    CurrentStep["AfterIntegrationHJ"] = CurrentSoftK + RigidK + CurrentU
                    CurrentStep["AfterIntegrationCandidateKJ"] = Row.get("ReconstructedCandidateKE")
                    RecordEvent("IntegrationAndDamping", DeltaK, DeltaU, Row)
                elif Phase == "BeforeCollision":
                    NewSoftK = float(Row["SoftKE"])
                    NewU = float(Row["CompliantEdgeElasticEstimate"])
                    DeltaK, DeltaU = NewSoftK - CurrentSoftK, NewU - CurrentU
                    CurrentSoftK, CurrentU = NewSoftK, NewU
                    CurrentStep["EdgeStageDeltaKJ"] = DeltaK
                    CurrentStep["EdgeStageDeltaUJ"] = DeltaU
                    BeforeCandidate = CurrentStep.get("AfterIntegrationCandidateKJ")
                    AfterCandidate = Row.get("ReconstructedCandidateKE")
                    if BeforeCandidate is not None and AfterCandidate is not None:
                        CurrentStep["EdgeCandidateDeltaKJ"] = float(AfterCandidate) - float(BeforeCandidate)
                    CurrentStep["BeforeCollisionHJ"] = CurrentSoftK + RigidK + CurrentU
                    RecordEvent("EdgeSweep", DeltaK, DeltaU, Row)
                elif Phase == "AfterCollision":
                    CompleteStep(Row)
                else:
                    raise ValueError(f"Unknown Stage phase {Phase}")
            elif Kind == "Reconstruction":
                DeltaK = float(Row["KineticChange"])
                CurrentSoftK += DeltaK
                CurrentStep["ReconstructionDeltaKJ"] += DeltaK
                RecordEvent("VelocityReconstruction", DeltaK, 0.0, Row,
                            {"Vertex": int(Row["Vertex"])})
            elif Kind == "ContactProjection":
                if Row["Phase"] == "Before":
                    if PendingProjection is not None:
                        raise ValueError("Unpaired projection Before")
                    PendingProjection = Row
                elif Row["Phase"] == "After":
                    Before = PendingProjection
                    if Before is None or Before["Vertex"] != Row["Vertex"] or Before["Frame"] != Row["Frame"]:
                        raise ValueError("Unpaired projection After")
                    BeforeU = float(Before["CompliantEdgeElasticEstimate"])
                    AfterU = float(Row["CompliantEdgeElasticEstimate"])
                    MaximumProjectionBeforeUResidual = max(MaximumProjectionBeforeUResidual,
                                                            abs(CurrentU - BeforeU))
                    DeltaU = AfterU - BeforeU
                    CurrentU = AfterU
                    CurrentStep["ProjectionDeltaUJ"] += DeltaU
                    CurrentStep["ContactProjectionCount"] += 1
                    Detail = {"Vertex": int(Row["Vertex"]),
                              "CollisionShapeIndex": int(Row["CollisionShapeIndex"]),
                              "PlaneDepthM": float(Before["PlaneDepth"]),
                              "TrueFrozenSphereGapBeforeM": float(Before["TrueFrozenSphereGap"]),
                              "TrueFrozenSphereGapAfterM": float(Row["TrueFrozenSphereGap"])}
                    RecordEvent("ContactPositionProjection", 0.0, DeltaU, Row, Detail)
                    ProjectionEvents.append({**Position(Row, CurrentStep["Ordinal"], Iterations),
                                             **Detail, "DeltaEdgeUJ": DeltaU,
                                             "HAfterJ": CurrentSoftK + RigidK + CurrentU})
                    PendingProjection = None
                else:
                    raise ValueError("Unknown ContactProjection phase")
            elif Kind == "DynamicContact":
                ParticleMass = 1.0 / float(Row["ParticleInverseMass"])
                SoftDelta = 0.5 * ParticleMass * (Squared(Row["ParticleVelocityAfterImpulse"]) -
                                                  Squared(Row["ParticleVelocityBeforeImpulse"]))
                RigidDelta = (RigidKinetic(Row["RigidVelocityAfter"], Row["RigidAngularAfter"], Mass, Inertia) -
                              RigidKinetic(Row["RigidVelocityBefore"], Row["RigidAngularBefore"], Mass, Inertia))
                CurrentSoftK += SoftDelta
                RigidK += RigidDelta
                PairDelta = SoftDelta + RigidDelta
                MinimumDynamicPairDelta = min(MinimumDynamicPairDelta, PairDelta)
                MaximumDynamicPairDelta = max(MaximumDynamicPairDelta, PairDelta)
                CurrentStep["DynamicPairDeltaKJ"] += PairDelta
                CurrentStep["DynamicContactCount"] += 1
                Detail = {"SoftDeltaKJ": SoftDelta, "RigidDeltaKJ": RigidDelta,
                          "ProjectionDepthM": float(Row["ProjectionDepth"])}
                RecordEvent("DynamicContactImpulse", PairDelta, 0.0, Row, Detail)
                DynamicPairEvents.append({**Position(Row, CurrentStep["Ordinal"], Iterations),
                                          **Detail, "PairDeltaKJ": PairDelta})
    CompleteFrame()
    if PendingProjection is not None:
        raise ValueError("Unpaired final projection")
    if any(Frame["Substeps"] != Steps * Iterations for Frame in FrameSummaries):
        raise ValueError("Incomplete phase trace in a frame")
    return {"Published": str(PublishedPath), "Trace": str(TracePath),
            "Scope": "Closed measured K ledger; positive-compliance Hookean edge U reference model. Intermediate crossings are diagnostics; only AfterCollision is accepted.",
            "Config": {Key: Config.get(Key) for Key in ("RigidCount", "Steps", "Iterations", "Compliance",
                                                       "BendCompliance", "Damping", "Friction", "Restitution")},
            "NecessaryKineticCeilingJ": KineticCeiling,
            "ReferenceModelCeilingJ": ModelCeiling,
            "InitialBudgetSource": InitialBudgetSource,
            "MinimumRigidHorizontalPairDistanceM": MinimumHorizontalPairDistance,
            "RigidPairDiameterM": 0.9,
            "MaximumSubstepKineticClosureResidualJ": MaximumKineticClosure,
            "MaximumSubstepEdgeUClosureResidualJ": MaximumModelClosure,
            "MaximumPublishedKineticClosureResidualJ": MaximumFrameKClosure,
            "MaximumPublishedEdgeUClosureResidualJ": MaximumFrameUClosure,
            "MaximumInterStepSoftKJumpJ": MaximumInterStepSoftKJump,
            "MaximumInterStepEdgeUJumpJ": MaximumInterStepEdgeUJump,
            "MaximumProjectionBeforeUResidualJ": MaximumProjectionBeforeUResidual,
            "MinimumDynamicContactPairDeltaKJ": MinimumDynamicPairDelta if DynamicPairEvents else None,
            "MaximumDynamicContactPairDeltaKJ": MaximumDynamicPairDelta if DynamicPairEvents else None,
            "FirstTransientModelCrossing": FirstTransientModelCrossing,
            "FirstTransientKineticCrossing": FirstTransientKineticCrossing,
            "FirstAcceptedModelFailure": FirstAcceptedModelFailure,
            "FirstAcceptedKineticFailure": FirstAcceptedKineticFailure,
            "FirstAcceptedFailureOperationEvents": FirstAcceptedFailureEvents,
            "FirstAcceptedKineticFailureOperationEvents": FirstAcceptedKineticFailureEvents,
            "Frames": FrameSummaries, "Substeps": Substeps,
            "ContactProjectionEvents": ProjectionEvents,
            "DynamicContactPairEvents": DynamicPairEvents}


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--published", type=Path, required=True)
    Parser.add_argument("--trace", type=Path, required=True)
    Parser.add_argument("--output", type=Path, required=True)
    Arguments = Parser.parse_args()
    Result = Analyse(Arguments.published, Arguments.trace)
    Arguments.output.parent.mkdir(parents=True, exist_ok=True)
    Arguments.output.write_text(json.dumps(Result, indent=2, allow_nan=False), encoding="utf-8")
    print(f"[Physics:CadencePhase] {len(Result['Substeps'])} accepted internal steps; "
          f"first model failure={Result['FirstAcceptedModelFailure']}; "
          f"first kinetic failure={Result['FirstAcceptedKineticFailure']}; "
          f"max K closure={Result['MaximumSubstepKineticClosureResidualJ']:.6g} J")


if __name__ == "__main__":
    Main()
