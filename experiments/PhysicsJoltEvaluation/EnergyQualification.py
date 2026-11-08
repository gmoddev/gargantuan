"""Bounded kinetic and specified spring-model energy qualification for native Jolt.

Reads existing JSONL only. Run once per fixture (for example C1 or C2):
  python EnergyQualification.py --input Native.jsonl --trace Native.phases.jsonl --output Energy.json

The positive-compliance edge energy is a defined Hookean reference model,
U = sum(C^2 / (2 compliance)). This script does not call it Jolt's hidden solver
energy or prove conservation of arbitrary contact/constraint algorithms.
"""

import argparse
import json
import math
from pathlib import Path
import struct


def Squared(Vector):
    return sum(float(Value) * float(Value) for Value in Vector)


def Distance(Left, Right):
    return math.sqrt(sum((float(A) - float(B)) ** 2 for A, B in zip(Left, Right)))


def ScalarSpringCalibration():
    """Check the source edge formula against one anchored implicit-Euler spring."""
    Checks = []
    for Mass, Compliance, Dt, Q, Velocity in (
        (0.1, 1.0e-5, 1.0 / 1440.0, 0.001, 0.1),
        (0.4, 2.0e-4, 1.0 / 5760.0, -0.003, -0.2),
        (1.2, 5.0e-6, 1.0 / 60.0, 0.002, 0.3),
    ):
        InvMass = 1.0 / Mass
        Predicted = Q + Velocity * Dt
        # Jolt ApplyEdgeConstraints with the other endpoint pinned, aligned
        # along the edge, and zero gravity/damping/contact.
        Correction = Predicted * InvMass / (InvMass + Compliance / (Dt * Dt))
        NextQ = Predicted - Correction
        NextVelocity = (NextQ - Q) / Dt
        ImplicitQ = Predicted / (1.0 + Dt * Dt / (Mass * Compliance))
        ImplicitVelocity = (ImplicitQ - Q) / Dt
        BeforeEnergy = 0.5 * Mass * Velocity * Velocity + Q * Q / (2.0 * Compliance)
        AfterEnergy = 0.5 * Mass * NextVelocity * NextVelocity + NextQ * NextQ / (2.0 * Compliance)
        DissipationIdentity = (0.5 * Mass * (NextVelocity - Velocity) ** 2
                               + 0.5 * (NextQ - Q) ** 2 / Compliance)
        Residual = (BeforeEnergy - AfterEnergy) - DissipationIdentity
        Tolerance = 1.0e-10 * max(1.0, BeforeEnergy)
        if (abs(NextQ - ImplicitQ) > Tolerance or
                abs(NextVelocity - ImplicitVelocity) > Tolerance or
                abs(Residual) > Tolerance or AfterEnergy > BeforeEnergy + Tolerance):
            raise ValueError("One-edge spring calibration failed")
        Checks.append({"Mass": Mass, "Compliance": Compliance, "Dt": Dt,
                       "InitialEnergy": BeforeEnergy, "AfterEnergy": AfterEnergy,
                       "PositionResidual": NextQ - ImplicitQ,
                       "VelocityResidual": NextVelocity - ImplicitVelocity,
                       "DissipationIdentityResidual": Residual})
    return {"Status": "ANALYTIC_SOURCE_FORMULA_ONLY", "Checks": Checks,
            "NativeOneEdgeFixtureVerified": False,
            "Meaning": "One anchored positive-compliance edge matches implicit Euler with k=1/c; full network/contact behavior is not thereby calibrated."}


def ReadRun(PathName):
    Config = None
    Frames = []
    Microsteps = []
    with PathName.open(encoding="utf-8") as Source:
        for Line in Source:
            Row = json.loads(Line)
            Kind = Row.get("Type")
            if Kind == "Config":
                if Config is not None:
                    raise ValueError("Multiple Config records")
                Config = Row
            elif Kind == "Frame":
                Frames.append(Row)
            elif Kind == "Microstep":
                Microsteps.append(Row)
    if Config is None or not Frames:
        raise ValueError(f"Missing Config or Frame records: {PathName}")
    if [int(Row["Frame"]) for Row in Frames] != list(range(len(Frames))):
        raise ValueError("Frame indices must be consecutive from zero")
    if len(Frames) != int(Config["Frames"]):
        raise ValueError("Frame count differs from Config")
    return Config, Frames, Microsteps


def InitialValues(Config, Arguments):
    InitialK = Config.get("InitialKineticEnergy", Arguments.initial_kinetic)
    InitialU = Config.get("InitialCompliantEdgeElasticEstimate", Arguments.initial_elastic)
    if InitialK is None or InitialU is None:
        raise ValueError("Authored t0 initial K and edge-model U are required; do not substitute post-step frame 0")
    InitialK, InitialU = float(InitialK), float(InitialU)
    if not all(math.isfinite(Value) and Value >= 0.0 for Value in (InitialK, InitialU)):
        raise ValueError("Invalid initial energy")
    return InitialK, InitialU


def RowQuality(Row, KineticCeiling, ModelCeiling, ModelAvailable, MaxPenetration, MaxStrain, MaxPinDrift, Fixture):
    K = float(Row["KineticEnergy"])
    U = float(Row["CompliantEdgeElasticEstimate"])
    Finite = bool(Row["Finite"]) and all(math.isfinite(Value) for Value in (K, U))
    if K < 0.0 or U < 0.0:
        Finite = False
    Penetration = float(Row["ParticlePenetration"])
    PinDrift = float(Row["MaxPinDrift"])
    Strain = float(Row["AllEdgeStrain"])
    PinSpeed = float(Row.get("MaxPinSpeed", 0.0))
    Finite = Finite and all(math.isfinite(Value) for Value in (Penetration, PinDrift, Strain, PinSpeed))
    return {"Frame": int(Row["Frame"]), "WorldUpdate": Row.get("WorldUpdate", Row.get("Microstep")),
            "KineticEnergyJ": K, "EdgeModelEnergyJ": U, "KPlusEdgeModelJ": K + U,
            "KineticGate": Finite and K <= KineticCeiling,
            "EdgeModelGate": Finite and ModelAvailable and K + U <= ModelCeiling,
            "Finite": Finite, "PenetrationM": Penetration, "Strain": Strain,
            "PinDriftM": PinDrift, "PinSpeedMps": PinSpeed,
            "GeometryGate": (Finite and PinDrift <= MaxPinDrift and
                             (Fixture == "Spring" or (Penetration <= MaxPenetration and Strain <= MaxStrain)))}


def Float32(Value):
    return struct.unpack("<f", struct.pack("<f", Value))[0]


def ValidateSpring(Config, Accepted, RawAccepted, InitialK, InitialU):
    """Validate each native one-edge step against the independent implicit-Euler recurrence."""
    if Config.get("Fixture") != "Spring":
        return None
    if Config.get("Backend") != "IndependentNativeJolt" or Config.get("Jolt") != "e77f175595e64cb44218cc9d9d56fc365ad0e36a":
        return {"Status": "NOT_PINNED_NATIVE_JOLT", "Pass": False}
    if not RawAccepted or any("Particles" not in Row or len(Row["Particles"]) != 2 for Row in RawAccepted):
        return {"Status": "MISSING_PARTICLE_STATES", "Pass": False}
    if int(Config["Steps"]) != 1 or int(Config["Iterations"]) != 1 or not Config.get("ObserveUpdates"):
        return {"Status": "NOT_SINGLE_EDGE_MICROSTEP_SCHEDULE", "Pass": False}
    if Config.get("Contact") or float(Config.get("Speed", 0.0)) != 0.0 or float(Config.get("Damping", 0.0)) != 0.0:
        return {"Status": "EXTERNAL_INPUT_OR_DAMPING", "Pass": False}
    Compliance = Float32(float(Config["Compliance"]))
    Dt = Float32(float(Config["InternalIntegrationDt"]))
    InvMass = 10.0
    Mass = 0.1
    PreviousX = Float32(1.1)
    PreviousVelocity = Float32(float(Config.get("SeedX", 0.0)))
    PreviousEnergy = InitialK + InitialU
    MaxPositionError = MaxVelocityError = MaxEnergyIncrease = MaxIdentityResidual = 0.0
    FirstFailure = None
    for Index, (Quality, Row) in enumerate(zip(Accepted, RawAccepted)):
        Pin, Moving = Row["Particles"]
        if any(abs(float(Component)) > 1.0e-6 for Component in Pin["P"] + Pin["V"]):
            FirstFailure = Index
            break
        ObservedX = float(Moving["P"][0])
        ObservedVelocity = float(Moving["V"][0])
        PredictedQ = (PreviousX - 1.0) + Dt * PreviousVelocity
        NextQ = PredictedQ / (1.0 + InvMass * Dt * Dt / Compliance)
        ExpectedX = 1.0 + NextQ
        ExpectedVelocity = (ExpectedX - PreviousX) / Dt
        PositionError = abs(ObservedX - ExpectedX)
        VelocityError = abs(ObservedVelocity - ExpectedVelocity)
        MaxPositionError = max(MaxPositionError, PositionError)
        MaxVelocityError = max(MaxVelocityError, VelocityError)
        Energy = Quality["KPlusEdgeModelJ"]
        EnergyIncrease = Energy - PreviousEnergy
        MaxEnergyIncrease = max(MaxEnergyIncrease, EnergyIncrease)
        Identity = (0.5 * Mass * (ObservedVelocity - PreviousVelocity) ** 2
                    + 0.5 * ((ObservedX - 1.0) - (PreviousX - 1.0)) ** 2 / Compliance)
        IdentityResidual = abs((PreviousEnergy - Energy) - Identity)
        MaxIdentityResidual = max(MaxIdentityResidual, IdentityResidual)
        # This compares native float output with a double evaluation of Jolt's
        # captured-float coefficients. It does not require bitwise parity.
        if (PositionError > 1.0e-4 or VelocityError > 1.0e-4 or
                EnergyIncrease > max(1.0e-4, 1.0e-6 * (InitialK + InitialU)) or
                IdentityResidual > max(1.0e-3, 1.0e-5 * (InitialK + InitialU))):
            FirstFailure = Index
            break
        PreviousX = ObservedX
        PreviousVelocity = ObservedVelocity
        PreviousEnergy = Energy
    return {"Status": "PASS_NATIVE_ONE_EDGE_IMPLICIT_EULER" if FirstFailure is None else "FAIL_NATIVE_ONE_EDGE_IMPLICIT_EULER",
            "Pass": FirstFailure is None, "CheckedMicrosteps": len(Accepted) if FirstFailure is None else FirstFailure + 1,
            "FirstFailureIndex": FirstFailure, "MaxPositionErrorM": MaxPositionError,
            "MaxVelocityErrorMps": MaxVelocityError,
            "MaxEnergyIncreaseJ": MaxEnergyIncrease,
            "MaxDissipationIdentityResidualJ": MaxIdentityResidual,
            "Tolerance": {"PositionM": 1.0e-4, "VelocityMps": 1.0e-4,
                          "EnergyIncreaseJ": max(1.0e-4, 1.0e-6 * (InitialK + InitialU)),
                          "IdentityResidualJ": max(1.0e-3, 1.0e-5 * (InitialK + InitialU))}}


def RigidEnergy(State, Mass, Inertia):
    return 0.5 * Mass * Squared(State["V"]) + 0.5 * Inertia * Squared(State["W"])


def TraceInternal(PathName, Config, Frames, KineticCeiling, ModelCeiling, ModelAvailable):
    """Replay cached rigid velocity changes to evaluate internal AfterCollision stages."""
    Mass = float(Config["RigidMass"])
    Inertia = float(Config["SphereInertia"])
    States = None
    CurrentFrame = None
    Ordinal = 0
    Counts = {}
    Internal = []
    Projections = []
    PendingProjection = None
    MatchError = 0.0
    ContactPairChanges = []
    with PathName.open(encoding="utf-8") as Source:
        for Line in Source:
            Row = json.loads(Line)
            Kind = Row.get("Type")
            if Kind not in {"Stage", "DynamicContact", "ContactProjection"}:
                continue
            Frame = int(Row["Frame"])
            if Frame != CurrentFrame:
                if CurrentFrame is not None and Frame != CurrentFrame + 1:
                    raise ValueError("Trace frames are not consecutive")
                if States is None:
                    if Frame == 0:
                        if "Speed" not in Config:
                            raise ValueError("Frame-zero trace requires Config.Speed to seed rigid states")
                        Speed = float(Config["Speed"])
                        States = [{"V": [0.0, 0.0, Speed if Index == 0 else -Speed], "W": [0.0, 0.0, 0.0]}
                                  for Index in range(int(Config["RigidCount"]))]
                    else:
                        Prior = Frames[Frame - 1]
                        States = [{"V": Body["V"], "W": Body["W"]} for Body in Prior["Rigids"]]
                CurrentFrame = Frame
                Ordinal = 0
                Counts[Frame] = 0
            if Kind == "Stage":
                if Row["Phase"] == "BeforeIntegration":
                    Ordinal += 1
                elif Row["Phase"] == "AfterCollision":
                    Counts[Frame] += 1
                    K = float(Row["SoftKE"]) + sum(RigidEnergy(State, Mass, Inertia) for State in States)
                    U = float(Row["CompliantEdgeElasticEstimate"])
                    Local = int(Row.get("InternalIteration", (Ordinal - 1) % int(Config["Iterations"]) + 1))
                    Internal.append({"Frame": Frame, "Ordinal": Ordinal, "LocalIteration": Local,
                                     "KineticEnergyJ": K, "EdgeModelEnergyJ": U, "KPlusEdgeModelJ": K + U,
                                     "KineticGate": math.isfinite(K) and K <= KineticCeiling,
                                     "EdgeModelGate": ModelAvailable and math.isfinite(K + U) and K + U <= ModelCeiling})
            elif Kind == "DynamicContact":
                BeforeV = Row["RigidVelocityBefore"]
                BeforeW = Row["RigidAngularBefore"]
                Errors = [Distance(State["V"], BeforeV) + Distance(State["W"], BeforeW) for State in States]
                Index = min(range(len(States)), key=lambda Candidate: Errors[Candidate])
                if Errors[Index] > 1.0e-3 or (len(Errors) > 1 and sorted(Errors)[1] - Errors[Index] < 1.0e-4):
                    raise ValueError(f"Unmatched or ambiguous rigid contact state: {Errors}")
                MatchError = max(MatchError, Errors[Index])
                ParticleMass = 1.0 / float(Row["ParticleInverseMass"])
                PairChange = (0.5 * ParticleMass * (Squared(Row["ParticleVelocityAfterImpulse"]) - Squared(Row["ParticleVelocityBeforeImpulse"]))
                              + RigidEnergy({"V": Row["RigidVelocityAfter"], "W": Row["RigidAngularAfter"]}, Mass, Inertia)
                              - RigidEnergy({"V": BeforeV, "W": BeforeW}, Mass, Inertia))
                ContactPairChanges.append(PairChange)
                States[Index] = {"V": Row["RigidVelocityAfter"], "W": Row["RigidAngularAfter"]}
            elif Kind == "ContactProjection":
                if Row["Phase"] == "Before":
                    if PendingProjection is not None:
                        raise ValueError("Unpaired projection before")
                    PendingProjection = Row
                elif Row["Phase"] == "After":
                    Before = PendingProjection
                    if Before is None or int(Before["Vertex"]) != int(Row["Vertex"]):
                        raise ValueError("Unpaired projection after")
                    Projections.append({"Frame": Frame, "Ordinal": Ordinal,
                                        "Vertex": int(Row["Vertex"]),
                                        "PositionDeltaM": Distance(Row["P"], Before["P"]),
                                        "PlaneDepthM": float(Before["PlaneDepth"]),
                                        "TrueSphereGapBeforeM": float(Before["TrueFrozenSphereGap"]),
                                        "TrueSphereGapAfterM": float(Row["TrueFrozenSphereGap"]),
                                        "EdgeModelChangeJ": (float(Row["CompliantEdgeElasticEstimate"])
                                                             - float(Before["CompliantEdgeElasticEstimate"]))})
                    PendingProjection = None
    if PendingProjection is not None:
        raise ValueError("Unpaired final projection")
    Expected = int(Config["Steps"]) * int(Config["Iterations"]) * int(Config.get("WorldUpdatesPerFrame", Config.get("WorldUpdates", 1)))
    FullCoverage = len(Counts) == len(Frames) and all(Counts.get(Frame) == Expected for Frame in range(len(Frames)))
    return {"States": Internal, "ProjectionEvents": Projections, "TraceFrames": sorted(Counts),
            "ExpectedInternalStatesPerFrame": Expected, "ObservedInternalStatesPerFrame": Counts,
            "FullInternalCoverage": FullCoverage, "MaxRigidMatchError": MatchError,
            "DynamicContactPairCount": len(ContactPairChanges),
            "PositiveDynamicContactPairCountAbove1eMinus6J": sum(Change > 1.0e-6 for Change in ContactPairChanges),
            "MaximumDynamicContactPairChangeJ": max(ContactPairChanges, default=None)}


def FirstFailure(Rows, Key):
    return next((Index for Index, Row in enumerate(Rows) if not Row[Key]), None)


def Qualify(Arguments):
    Config, Frames, Microsteps = ReadRun(Arguments.input)
    InitialK, InitialU = InitialValues(Config, Arguments)
    Fixture = Config.get("Fixture", "Cloth")
    ModelCeiling = 1.05 * (InitialK + InitialU) + 1.0e-5
    # Initial spring strain is genuine available elastic energy; a zero-initial-U
    # authored cloth retains the original necessary kinetic ceiling exactly.
    KineticCeiling = ModelCeiling if Fixture == "Spring" else 1.05 * InitialK + 1.0e-5
    ModelAvailable = Config.get("AllEdgesPositiveCompliance") is True
    WorldUpdates = int(Config.get("WorldUpdatesPerFrame", Config.get("WorldUpdates", 1)))
    if Microsteps:
        if len(Microsteps) != len(Frames) * WorldUpdates:
            raise ValueError("Microstep count does not match frames × world updates")
        for Frame in range(len(Frames)):
            Block = Microsteps[Frame * WorldUpdates:(Frame + 1) * WorldUpdates]
            if any(int(Row["Frame"]) != Frame for Row in Block):
                raise ValueError("Microstep frame grouping mismatch")
            Last = Block[-1]
            if abs(float(Last["KineticEnergy"]) - float(Frames[Frame]["KineticEnergy"])) > 1.0e-5:
                raise ValueError("Last Microstep does not close to published frame K")
    elif WorldUpdates > 1:
        raise ValueError("Multiple world updates requested but no accepted Microstep rows found")
    AcceptedRaw = Microsteps if Microsteps else Frames
    Accepted = [RowQuality(Row, KineticCeiling, ModelCeiling, ModelAvailable,
                           Arguments.max_penetration, Arguments.max_strain, Arguments.max_pin_drift, Fixture)
                for Row in AcceptedRaw]
    SpringCalibration = ValidateSpring(Config, Accepted, AcceptedRaw, InitialK, InitialU)
    SpringReference = None
    SpringReferencePass = Fixture == "Spring"
    if Arguments.spring_calibration:
        SpringReference = json.loads(Arguments.spring_calibration.read_text(encoding="utf-8"))
        ReferenceConfig = SpringReference.get("ConfigSummary", {})
        SpringReferencePass = (SpringReference.get("Fixture") == "Spring"
                               and SpringReference.get("CompleteBoundedPass") is True
                               and SpringReference.get("NativeSpringCalibration", {}).get("Pass") is True
                               and ReferenceConfig.get("Jolt") == Config.get("Jolt")
                               and abs(float(ReferenceConfig.get("Compliance", -1.0))
                                       - float(Config["Compliance"])) <= 1.0e-7 * max(1.0, float(Config["Compliance"])))
    Trace = TraceInternal(Arguments.trace, Config, Frames, KineticCeiling, ModelCeiling, ModelAvailable) if Arguments.trace else None
    Internal = Trace["States"] if Trace else []
    FullInternalCoverage = bool(Trace and Trace["FullInternalCoverage"])
    # Steps=Iterations=1 makes every full World.Update endpoint an internal
    # integration endpoint with actual rigid writeback. Otherwise only a full
    # phase trace can establish every internal solver-state bound.
    if int(Config["Steps"]) == 1 and int(Config["Iterations"]) == 1 and Microsteps:
        FullInternalCoverage = True
    AcceptedPass = all(Row["Finite"] and Row["GeometryGate"] and Row["KineticGate"] for Row in Accepted)
    ModelPass = ModelAvailable and all(Row["EdgeModelGate"] for Row in Accepted)
    InternalKPass = all(Row["KineticGate"] for Row in Internal)
    InternalModelPass = ModelAvailable and all(Row["EdgeModelGate"] for Row in Internal)
    if int(Config["Steps"]) == 1 and int(Config["Iterations"]) == 1 and Microsteps:
        InternalKPass = all(Row["KineticGate"] for Row in Accepted)
        InternalModelPass = ModelAvailable and all(Row["EdgeModelGate"] for Row in Accepted)
    Preconditions = bool(Config.get("ZeroGravity", False)) and bool(Config.get("Dynamic", False)) and bool(Config.get("Pinned", False))
    DefaultContactPolicy = (bool(Config.get("Contact", False)) and
                            float(Config.get("ContactProjectionScale", 1.0)) == 1.0 and
                            int(Config.get("ProjectionPasses", 1)) == 1 and
                            not bool(Config.get("RefreshSpherePlane", False)) and
                            int(Config.get("ContactThrough", len(Frames))) >= len(Frames) - 1)
    if Fixture == "Spring":
        Preconditions = Preconditions and bool(SpringCalibration and SpringCalibration["Pass"])
    else:
        Preconditions = Preconditions and DefaultContactPolicy
    CompleteBoundedPass = (Preconditions and AcceptedPass and ModelPass and FullInternalCoverage
                           and InternalKPass and InternalModelPass and SpringReferencePass)
    if not Preconditions or not ModelAvailable:
        Status = "UNQUALIFIED_PRECONDITION"
    elif not AcceptedPass or not ModelPass or not InternalKPass or not InternalModelPass:
        Status = "FAIL_OBSERVED_BOUND"
    elif not SpringReferencePass:
        Status = "UNQUALIFIED_NATIVE_SPRING_CALIBRATION_MISSING_OR_MISMATCHED"
    elif not FullInternalCoverage:
        Status = "PASS_SAMPLED_ONLY_INTERNAL_COVERAGE_INCOMPLETE"
    elif Fixture == "Spring":
        Status = "PASS_NATIVE_ONE_EDGE_CALIBRATION_AND_BOUNDED_MODEL"
    else:
        Status = "PASS_BOUNDED_REFERENCE_MODEL_AND_RECORDED_GEOMETRY"
    Result = {
        "Input": str(Arguments.input.resolve()), "Trace": str(Arguments.trace.resolve()) if Arguments.trace else None,
        "Status": Status, "CompleteBoundedPass": CompleteBoundedPass,
        "Scope": "Finite no-gravity fixed-pin dynamic sphere fixture; necessary K and explicitly specified Hookean positive-compliance edge model. Not a full Jolt/XPBD conservation or migration qualification.",
        "Calibration": ScalarSpringCalibration(),
        "NativeSpringCalibration": SpringCalibration,
        "SpringCalibrationReference": str(Arguments.spring_calibration.resolve()) if Arguments.spring_calibration else None,
        "SpringCalibrationReferencePass": SpringReferencePass,
        "Fixture": Fixture,
        "ConfigSummary": {Key: Config.get(Key) for Key in ("Backend", "Jolt", "Fixture", "Compliance", "BendCompliance", "Steps", "Iterations", "WorldUpdatesPerFrame")},
        "InitialKineticEnergyJ": InitialK, "InitialEdgeModelEnergyJ": InitialU,
        "NecessaryKineticCeilingJ": KineticCeiling, "EdgeModelCeilingJ": ModelCeiling,
        "AllFixtureEdgesPositiveCompliance": ModelAvailable,
        "PreconditionsSatisfied": Preconditions,
        "DefaultContactPolicy": DefaultContactPolicy if Fixture != "Spring" else None,
        "AcceptedStateCount": len(Accepted), "PublishedFrameCount": len(Frames),
        "WorldUpdatesPerFrame": WorldUpdates,
        "FullInternalCoverage": FullInternalCoverage,
        "AcceptedKineticGate": all(Row["KineticGate"] for Row in Accepted),
        "AcceptedEdgeModelGate": ModelPass,
        "AcceptedGeometryGate": all(Row["GeometryGate"] for Row in Accepted),
        "AcceptedFinite": all(Row["Finite"] for Row in Accepted),
        "InternalKineticGate": InternalKPass,
        "InternalEdgeModelGate": InternalModelPass,
        "MaximumAcceptedKineticEnergyJ": max(Row["KineticEnergyJ"] for Row in Accepted),
        "MaximumAcceptedKPlusEdgeModelJ": max(Row["KPlusEdgeModelJ"] for Row in Accepted),
        "MaximumAcceptedPenetrationM": max(Row["PenetrationM"] for Row in Accepted),
        "MaximumAcceptedStrain": max(Row["Strain"] for Row in Accepted),
        "MaximumAcceptedPinDriftM": max(Row["PinDriftM"] for Row in Accepted),
        "FirstAcceptedKineticFailureIndex": FirstFailure(Accepted, "KineticGate"),
        "FirstAcceptedEdgeModelFailureIndex": FirstFailure(Accepted, "EdgeModelGate") if ModelAvailable else None,
        "FirstAcceptedGeometryFailureIndex": FirstFailure(Accepted, "GeometryGate"),
        "MaximumTracedInternalKineticEnergyJ": max((Row["KineticEnergyJ"] for Row in Internal), default=None),
        "MaximumTracedInternalKPlusEdgeModelJ": max((Row["KPlusEdgeModelJ"] for Row in Internal), default=None),
        "FirstTracedInternalKineticFailureIndex": FirstFailure(Internal, "KineticGate") if Internal else None,
        "FirstTracedInternalEdgeModelFailureIndex": FirstFailure(Internal, "EdgeModelGate") if Internal and ModelAvailable else None,
        "TraceSummary": {Key: Value for Key, Value in Trace.items() if Key not in {"States", "ProjectionEvents"}} if Trace else None,
        "TraceContactProjectionEvents": Trace["ProjectionEvents"] if Trace else [],
    }
    if Arguments.include_states:
        Result["AcceptedStates"] = Accepted
        Result["TracedInternalStates"] = Internal
    return Result


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--input", type=Path, required=True)
    Parser.add_argument("--trace", type=Path)
    Parser.add_argument("--output", type=Path, required=True)
    Parser.add_argument("--initial-kinetic", type=float, help="Explicit authored t0 K for older traces lacking Config field")
    Parser.add_argument("--initial-elastic", type=float, help="Explicit authored t0 edge U for older traces lacking Config field")
    Parser.add_argument("--spring-calibration", type=Path,
                        help="Native one-edge result JSON at the same pinned Jolt and compliance; required for a complete cloth model pass")
    Parser.add_argument("--max-penetration", type=float, default=0.1)
    Parser.add_argument("--max-strain", type=float, default=0.1)
    Parser.add_argument("--max-pin-drift", type=float, default=1.0e-4)
    Parser.add_argument("--include-states", action="store_true")
    Parser.add_argument("--require-pass", action="store_true")
    Arguments = Parser.parse_args()
    if any(not math.isfinite(Value) or Value < 0.0 for Value in
           (Arguments.max_penetration, Arguments.max_strain, Arguments.max_pin_drift)):
        Parser.error("Quality limits must be finite and nonnegative")
    Result = Qualify(Arguments)
    Arguments.output.write_text(json.dumps(Result, indent=2, allow_nan=False), encoding="utf-8")
    print(f"{Result['Status']}: accepted Kmax={Result['MaximumAcceptedKineticEnergyJ']:.9g} J, "
          f"accepted K+Umax={Result['MaximumAcceptedKPlusEdgeModelJ']:.9g} J, "
          f"internal coverage={Result['FullInternalCoverage']}")
    if Arguments.require_pass and not Result["CompleteBoundedPass"]:
        raise SystemExit(3)


if __name__ == "__main__":
    Main()
