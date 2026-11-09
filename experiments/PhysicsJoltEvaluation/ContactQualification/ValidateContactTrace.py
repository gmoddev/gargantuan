"""Independently inspect full 3D cloth contact traces without running a solver."""

import argparse
import gzip
import hashlib
import json
import math
from pathlib import Path
import re
import struct


MaximumVertices = 16_384
MaximumFrames = 600
MaximumTriangles = 32_512
MaximumEdges = 100_000
MaximumPins = 256
MaximumColliders = 8
MaximumTriangleSphereChecks = 5_000_000
MaximumLineBytes = 8 * 1024 * 1024
MaximumBoundaryPairChecks = 2_000_000


def FiniteVector(Value, Size=3):
    return isinstance(Value, list) and len(Value) == Size and all(
        isinstance(Component, (int, float)) and math.isfinite(Component)
        for Component in Value)


def Sub(A, B):
    return (A[0] - B[0], A[1] - B[1], A[2] - B[2])


def Dot(A, B):
    return sum(Left * Right for Left, Right in zip(A, B))


def Cross(A, B):
    return (A[1] * B[2] - A[2] * B[1],
            A[2] * B[0] - A[0] * B[2],
            A[0] * B[1] - A[1] * B[0])


def Norm(A):
    return math.sqrt(Dot(A, A))


def SegmentDistanceSquared(Point, A, B):
    Edge = Sub(B, A)
    LengthSquared = Dot(Edge, Edge)
    Factor = max(0.0, min(1.0, Dot(Sub(Point, A), Edge) / LengthSquared)) \
        if LengthSquared > 0 else 0.0
    Closest = tuple(A[Axis] + Factor * Edge[Axis] for Axis in range(3))
    Delta = Sub(Point, Closest)
    return Dot(Delta, Delta)


def TriangleDistanceSquared(Point, A, B, C):
    """Closest point to a triangle, including face, edge and corner regions."""
    AB, AC, AP = Sub(B, A), Sub(C, A), Sub(Point, A)
    Normal = Cross(AB, AC)
    if Dot(Normal, Normal) < 1e-24:
        return min(SegmentDistanceSquared(Point, A, B),
                   SegmentDistanceSquared(Point, B, C),
                   SegmentDistanceSquared(Point, C, A))
    D1, D2 = Dot(AB, AP), Dot(AC, AP)
    if D1 <= 0 and D2 <= 0:
        return Dot(AP, AP)
    BP = Sub(Point, B)
    D3, D4 = Dot(AB, BP), Dot(AC, BP)
    if D3 >= 0 and D4 <= D3:
        return Dot(BP, BP)
    VC = D1 * D4 - D3 * D2
    if VC <= 0 and D1 >= 0 and D3 <= 0:
        return SegmentDistanceSquared(Point, A, B)
    CP = Sub(Point, C)
    D5, D6 = Dot(AB, CP), Dot(AC, CP)
    if D6 >= 0 and D5 <= D6:
        return Dot(CP, CP)
    VB = D5 * D2 - D1 * D6
    if VB <= 0 and D2 >= 0 and D6 <= 0:
        return SegmentDistanceSquared(Point, A, C)
    VA = D3 * D6 - D5 * D4
    if VA <= 0 and D4 - D3 >= 0 and D5 - D6 >= 0:
        return SegmentDistanceSquared(Point, B, C)
    PlaneDistance = Dot(AP, Normal)
    return PlaneDistance * PlaneDistance / Dot(Normal, Normal)


def DiskBoundary(Triangles, VertexCount):
    EdgeUses = {}
    Used = set()
    for Triangle in Triangles:
        Used.update(Triangle)
        for A, B in ((Triangle[0], Triangle[1]), (Triangle[1], Triangle[2]),
                     (Triangle[2], Triangle[0])):
            Key = tuple(sorted((A, B)))
            EdgeUses.setdefault(Key, []).append((A, B))
    if len(Used) != VertexCount or any(len(Uses) > 2 or
            (len(Uses) == 2 and Uses[0] != Uses[1][::-1])
            for Uses in EdgeUses.values()) or \
            VertexCount - len(EdgeUses) + len(Triangles) != 1:
        raise ValueError('Triangle topology is not a single manifold disk')
    VertexNeighbors = [[] for _ in range(VertexCount)]
    for A, B in EdgeUses:
        VertexNeighbors[A].append(B)
        VertexNeighbors[B].append(A)
    Connected = {0}
    Stack = [0]
    while Stack:
        for Neighbor in VertexNeighbors[Stack.pop()]:
            if Neighbor not in Connected:
                Connected.add(Neighbor)
                Stack.append(Neighbor)
    if len(Connected) != VertexCount:
        raise ValueError('Triangle topology has disconnected components')
    BoundaryEdges = [Edge for Edge, Uses in EdgeUses.items() if len(Uses) == 1]
    Neighbors = {}
    for A, B in BoundaryEdges:
        Neighbors.setdefault(A, []).append(B)
        Neighbors.setdefault(B, []).append(A)
    if not BoundaryEdges or any(len(Values) != 2 for Values in Neighbors.values()):
        raise ValueError('Triangle disk boundary is missing or nonmanifold')
    Start = min(Neighbors)
    Ordered = [Start]
    Previous = None
    Current = Start
    while True:
        Next = next(Value for Value in Neighbors[Current] if Value != Previous)
        if Next == Start:
            break
        if Next in Ordered or len(Ordered) >= len(Neighbors):
            raise ValueError('Triangle boundary has a repeated vertex')
        Ordered.append(Next)
        Previous, Current = Current, Next
    if len(Ordered) != len(Neighbors):
        raise ValueError('Triangle boundary has multiple loops')
    return Ordered


def RestPlane(Rest, Triangles):
    A, B, C = (Rest[Index] for Index in Triangles[0])
    U = Sub(B, A)
    Length = Norm(U)
    if Length <= 1e-12:
        raise ValueError('Rest triangle is degenerate')
    U = tuple(Value / Length for Value in U)
    N = Cross(U, Sub(C, A))
    Length = Norm(N)
    if Length <= 1e-12:
        raise ValueError('Rest triangle is degenerate')
    N = tuple(Value / Length for Value in N)
    V = Cross(N, U)
    if max(abs(Dot(Sub(Point, A), N)) for Point in Rest) > 1e-5:
        return None
    return A, U, V, N


def BoundaryCrossings(Positions, Boundary, Origin, U, V, RemainingChecks):
    Projected = [(Dot(Sub(Positions[Index], Origin), U),
                  Dot(Sub(Positions[Index], Origin), V)) for Index in Boundary]
    Segments = []
    for Index, A in enumerate(Projected):
        B = Projected[(Index + 1) % len(Projected)]
        if A == B:
            return 1, 0
        Segments.append((Index, A, B, min(A[0], B[0]), max(A[0], B[0]),
                         min(A[1], B[1]), max(A[1], B[1])))
    Segments.sort(key=lambda Value: Value[3])
    Active = []
    Checks = 0
    Crossings = 0
    Length = len(Boundary)
    for Current in Segments:
        Active = [Other for Other in Active if Other[4] >= Current[3]]
        for Other in Active:
            I, J = Current[0], Other[0]
            if abs(I - J) == 1 or {I, J} == {0, Length - 1} or \
                    Other[6] < Current[5] or Current[6] < Other[5]:
                continue
            Checks += 1
            if Checks > RemainingChecks:
                raise ValueError('Planar boundary intersection budget exceeded')
            A, B, C, D = Current[1], Current[2], Other[1], Other[2]
            Turn = lambda P, Q, R: (Q[0] - P[0]) * (R[1] - P[1]) - \
                (Q[1] - P[1]) * (R[0] - P[0])
            if Turn(A, B, C) * Turn(A, B, D) <= 0 and \
                    Turn(C, D, A) * Turn(C, D, B) <= 0:
                Crossings += 1
        Active.append(Current)
    return Crossings, Checks


def ReadVectors(Row, Name, Count, Directory):
    Inline = Row.get(Name)
    Reference = Row.get(Name + 'Ref')
    if (Inline is None) == (Reference is None):
        if Inline is None:
            return None
        raise ValueError(f'{Name} has both inline and referenced data')
    if Inline is not None:
        if not isinstance(Inline, list) or len(Inline) != Count * 3 or \
                any(not isinstance(Value, (int, float)) or not math.isfinite(Value)
                    for Value in Inline):
            raise ValueError(f'{Name} inline array is missing, nonfinite or mis-sized')
        Values = Inline
    else:
        if not isinstance(Reference, dict) or Reference.get('Count') != Count * 3 or \
                not isinstance(Reference.get('File'), str) or \
                not isinstance(Reference.get('OffsetBytes'), int) or \
                Reference['OffsetBytes'] < 0 or \
                not isinstance(Reference.get('Sha256'), str) or \
                not re.fullmatch(r'[0-9a-f]{64}', Reference['Sha256']):
            raise ValueError(f'{Name} reference is malformed')
        File = (Directory / Reference['File']).resolve()
        if not File.is_relative_to(Directory.resolve()) or not File.is_file():
            raise ValueError(f'{Name} reference escapes or is missing')
        ByteCount = Count * 3 * 4
        if Reference['OffsetBytes'] + ByteCount > File.stat().st_size:
            raise ValueError(f'{Name} reference exceeds sidecar')
        with File.open('rb') as Stream:
            Stream.seek(Reference['OffsetBytes'])
            Raw = Stream.read(ByteCount)
        if hashlib.sha256(Raw).hexdigest() != Reference['Sha256']:
            raise ValueError(f'{Name} frame digest mismatch')
        Values = [Value for (Value,) in struct.iter_unpack('<f', Raw)]
        if any(not math.isfinite(Value) for Value in Values):
            raise ValueError(f'{Name} sidecar contains nonfinite values')
    return [tuple(Values[Index:Index + 3]) for Index in range(0, len(Values), 3)]


def LoadFixture(PathValue):
    Fixture = json.loads(PathValue.read_text(encoding='utf-8'))
    if Fixture.get('Schema') != 'Gargantuan.ContactFixture.V1' or \
            not isinstance(Fixture.get('CaseId'), str) or not Fixture['CaseId'] or \
            not isinstance(Fixture.get('Frames'), int) or not 1 <= Fixture['Frames'] <= MaximumFrames or \
            not isinstance(Fixture.get('DeltaTimeSeconds'), (int, float)) or \
            not math.isfinite(Fixture['DeltaTimeSeconds']) or Fixture['DeltaTimeSeconds'] <= 0:
        raise ValueError('Invalid contact fixture identity or cadence')
    Rest = Fixture.get('RestPositions')
    if not isinstance(Rest, list) or not 3 <= len(Rest) <= MaximumVertices or \
            not all(FiniteVector(Value) for Value in Rest):
        raise ValueError('Invalid rest vertices')
    Count = len(Rest)
    Initial = Fixture.get('InitialVelocities')
    Masses = Fixture.get('VertexMassKg')
    if not isinstance(Initial, list) or len(Initial) != Count or \
            not all(FiniteVector(Value) for Value in Initial) or \
            not isinstance(Masses, list) or len(Masses) != Count or \
            any(not isinstance(Value, (int, float)) or not math.isfinite(Value) or Value < 0
                for Value in Masses):
        raise ValueError('Invalid initial velocities or vertex masses')
    Triangles = Fixture.get('Triangles')
    Edges = Fixture.get('StructuralEdges')
    Pins = Fixture.get('Pins')
    Colliders = Fixture.get('Colliders')
    if not isinstance(Triangles, list) or not 1 <= len(Triangles) <= MaximumTriangles or \
            not all(isinstance(Triangle, list) and len(Triangle) == 3 and
                    len(set(Triangle)) == 3 and all(type(Index) is int and 0 <= Index < Count
                                                    for Index in Triangle)
                    for Triangle in Triangles):
        raise ValueError('Invalid oriented triangle topology')
    if not isinstance(Edges, list) or len(Edges) > MaximumEdges or \
            not all(isinstance(Edge, dict) and type(Edge.get('A')) is int and
                    type(Edge.get('B')) is int and Edge['A'] != Edge['B'] and
                    0 <= Edge['A'] < Count and 0 <= Edge['B'] < Count and
                    Edge.get('Kind') in ('axial', 'shear', 'bend') and
                    isinstance(Edge.get('RestLengthM'), (int, float)) and
                    math.isfinite(Edge['RestLengthM']) and Edge['RestLengthM'] > 0
                    for Edge in Edges):
        raise ValueError('Invalid structural edges')
    if any('StiffnessNPerM' in Edge and
           (not isinstance(Edge['StiffnessNPerM'], (int, float)) or
            not math.isfinite(Edge['StiffnessNPerM']) or Edge['StiffnessNPerM'] <= 0)
           for Edge in Edges):
        raise ValueError('Invalid optional structural stiffness')
    if not any(Edge['Kind'] == 'axial' for Edge in Edges):
        raise ValueError('Fixture has no axial strain edges')
    if not isinstance(Pins, list) or len(Pins) > MaximumPins or \
            not all(isinstance(Pin, dict) and type(Pin.get('Vertex')) is int and
                    0 <= Pin['Vertex'] < Count and FiniteVector(Pin.get('Target'))
                    for Pin in Pins) or len({Pin['Vertex'] for Pin in Pins}) != len(Pins):
        raise ValueError('Invalid pins')
    if not isinstance(Colliders, list) or len(Colliders) > MaximumColliders or \
            not all(isinstance(C, dict) and isinstance(C.get('Id'), str) and C['Id'] and
                    C.get('Kind') == 'sphere' and C.get('Motion') in
                    ('static', 'kinematic', 'dynamic') and
                    isinstance(C.get('RadiusM'), (int, float)) and
                    math.isfinite(C['RadiusM']) and C['RadiusM'] > 0 and
                    (C.get('MassKg') is None or
                     (isinstance(C['MassKg'], (int, float)) and
                      math.isfinite(C['MassKg']) and C['MassKg'] > 0))
                    for C in Colliders) or len({C['Id'] for C in Colliders}) != len(Colliders):
        raise ValueError('Invalid sphere colliders')
    if any('InertiaKgM2' in C and
           (not isinstance(C['InertiaKgM2'], (int, float)) or
            not math.isfinite(C['InertiaKgM2']) or C['InertiaKgM2'] <= 0)
           for C in Colliders):
        raise ValueError('Invalid optional sphere inertia')
    if len(Triangles) * len(Colliders) * Fixture['Frames'] > MaximumTriangleSphereChecks:
        raise ValueError('Triangle-sphere check budget exceeded')
    if not isinstance(Fixture.get('ThicknessM'), (int, float)) or \
            not math.isfinite(Fixture['ThicknessM']) or Fixture['ThicknessM'] < 0 or \
            not FiniteVector(Fixture.get('GravityMPerS2')):
        raise ValueError('Invalid thickness or gravity')
    Gates = Fixture.get('Gates')
    if not isinstance(Gates, dict) or any(
            not isinstance(Gates.get(Key), (int, float)) or
            not math.isfinite(Gates[Key]) or Gates[Key] < 0
            for Key in ('MaximumAxialStrain', 'MaximumPinErrorM',
                        'MaximumSpherePenetrationM')):
        raise ValueError('Missing or invalid prespecified geometry gates')
    for Key in ('MaximumContactCount', 'MinimumContactCount'):
        if Key in Gates and (type(Gates[Key]) is not int or Gates[Key] < 0):
            raise ValueError('Invalid optional contact-count gate')
    if 'MaximumContactImpulsePairResidualNs' in Gates and \
            (not isinstance(Gates['MaximumContactImpulsePairResidualNs'], (int, float)) or
             not math.isfinite(Gates['MaximumContactImpulsePairResidualNs']) or
             Gates['MaximumContactImpulsePairResidualNs'] < 0):
        raise ValueError('Invalid optional contact-impulse gate')
    return Fixture


def Inspect(Directory):
    Directory = Directory.resolve()
    FixturePath = Directory / 'ContactFixtureV1.json'
    TracePath = Directory / 'ContactTraceV1.jsonl.gz'
    Fixture = LoadFixture(FixturePath)
    if not TracePath.is_file() or TracePath.stat().st_size > 512 * 1024 * 1024:
        raise ValueError('Compressed contact trace is missing or oversized')
    Rest = Fixture['RestPositions']
    Count = len(Rest)
    Boundary = DiskBoundary(Fixture['Triangles'], Count)
    Plane = RestPlane(Rest, Fixture['Triangles'])
    Colliders = {Collider['Id']: Collider for Collider in Fixture['Colliders']}
    RestNormals = []
    RestAreas = []
    for A, B, C in Fixture['Triangles']:
        Normal = Cross(Sub(Rest[B], Rest[A]), Sub(Rest[C], Rest[A]))
        Area2 = Norm(Normal)
        if Area2 <= 1e-12:
            raise ValueError('Rest triangle is degenerate')
        RestNormals.append(Normal)
        RestAreas.append(Area2)
    MaxAxial = MaxAllEdge = MaxPin = MaxPenetration = 0.0
    MaxAxialCompression = MaxAxialTension = 0.0
    MinAreaRatio = math.inf
    OrientationFlips = 0
    MaxImpulsePairResidual = 0.0
    AllVelocities = True
    AllContactImpulses = True
    AllExternalWork = True
    AllRigidInertia = True
    AllStiffness = all('StiffnessNPerM' in Edge for Edge in Fixture['StructuralEdges'])
    InitialKinetic = sum(0.5 * Mass * Dot(Velocity, Velocity)
                         for Mass, Velocity in zip(Fixture['VertexMassKg'],
                                                   Fixture['InitialVelocities']))
    MaxKinetic = 0.0
    LastKinetic = None
    MaxGravityPotential = -math.inf
    LastGravityPotential = None
    MaxElasticSurrogate = 0.0
    LastElasticSurrogate = None
    MaxRigidTranslationalKinetic = 0.0
    LastRigidTranslationalKinetic = None
    MaxRigidRotationalKinetic = 0.0
    LastRigidRotationalKinetic = None
    MaxSpeed = 0.0
    ContactCount = 0
    MaxOutOfPlane = 0.0
    PlanarBoundaryCrossings = 0
    BoundaryPairChecks = 0
    ExternalWorkTotals = {Key: 0.0 for Key in
        ('ActuatorWorkJ', 'StaticPoseProjectionWorkJ', 'PinWorkJ',
         'OtherWorkJ', 'DissipationJ')}
    TraceDigest = hashlib.sha256()
    with TracePath.open('rb') as RawTrace, gzip.GzipFile(fileobj=RawTrace) as Stream:
        for Frame in range(Fixture['Frames']):
            Line = Stream.readline(MaximumLineBytes + 1)
            if not Line or len(Line) > MaximumLineBytes:
                raise ValueError('Contact trace frame missing or line oversized')
            TraceDigest.update(Line)
            Row = json.loads(Line)
            if type(Row.get('Frame')) is not int or Row['Frame'] != Frame or \
                    not isinstance(Row.get('TimeSeconds'), (int, float)) or \
                    not math.isfinite(Row['TimeSeconds']) or \
                    abs(Row['TimeSeconds'] - (Frame + 1) * Fixture['DeltaTimeSeconds']) > 2e-6:
                raise ValueError('Contact trace frame index or physical time mismatch')
            Positions = ReadVectors(Row, 'Positions', Count, Directory)
            Velocities = ReadVectors(Row, 'Velocities', Count, Directory)
            if Positions is None:
                raise ValueError('Full contact positions are required')
            if Plane is not None:
                Origin, U, V, PlaneNormal = Plane
                FramePlaneDeviation = max(abs(Dot(Sub(Position, Origin), PlaneNormal))
                                          for Position in Positions)
                MaxOutOfPlane = max(MaxOutOfPlane, FramePlaneDeviation)
                if FramePlaneDeviation <= 1e-5:
                    Crossings, Checks = BoundaryCrossings(
                        Positions, Boundary, Origin, U, V,
                        MaximumBoundaryPairChecks - BoundaryPairChecks)
                    PlanarBoundaryCrossings += Crossings
                    BoundaryPairChecks += Checks
            if Velocities is None:
                AllVelocities = False
            States = Row.get('ColliderStates')
            if not isinstance(States, list) or len(States) != len(Colliders) or \
                    {State.get('Id') for State in States if isinstance(State, dict)} != set(Colliders):
                raise ValueError('Collider state count or identity mismatch')
            for State in States:
                if not FiniteVector(State.get('Center')) or \
                        not FiniteVector(State.get('OrientationWxyz'), 4) or \
                        not FiniteVector(State.get('LinearVelocity')) or \
                        not FiniteVector(State.get('AngularVelocity')) or \
                        abs(Norm(State['OrientationWxyz']) - 1.0) > 1e-3:
                    raise ValueError('Invalid actual collider state')
            for Edge in Fixture['StructuralEdges']:
                Length = Norm(Sub(Positions[Edge['A']], Positions[Edge['B']]))
                SignedStrain = Length / Edge['RestLengthM'] - 1.0
                MaxAllEdge = max(MaxAllEdge, abs(SignedStrain))
                if Edge['Kind'] == 'axial':
                    MaxAxial = max(MaxAxial, abs(SignedStrain))
                    MaxAxialCompression = max(MaxAxialCompression, -SignedStrain)
                    MaxAxialTension = max(MaxAxialTension, SignedStrain)
            if AllStiffness:
                ElasticSurrogate = sum(
                    0.5 * Edge['StiffnessNPerM'] *
                    (Norm(Sub(Positions[Edge['A']], Positions[Edge['B']])) -
                     Edge['RestLengthM']) ** 2
                    for Edge in Fixture['StructuralEdges'])
                MaxElasticSurrogate = max(MaxElasticSurrogate, ElasticSurrogate)
                LastElasticSurrogate = ElasticSurrogate
            for Pin in Fixture['Pins']:
                MaxPin = max(MaxPin, Norm(Sub(Positions[Pin['Vertex']], Pin['Target'])))
            for Index, (A, B, C) in enumerate(Fixture['Triangles']):
                Normal = Cross(Sub(Positions[B], Positions[A]),
                               Sub(Positions[C], Positions[A]))
                MinAreaRatio = min(MinAreaRatio, Norm(Normal) / RestAreas[Index])
                if Dot(Normal, RestNormals[Index]) < 0:
                    OrientationFlips += 1
                for State in States:
                    Sphere = Colliders[State['Id']]
                    Radius = Sphere['RadiusM'] + Fixture['ThicknessM']
                    Center = State['Center']
                    if any(min(Positions[V][Axis] for V in (A, B, C)) - Radius > Center[Axis] or
                           max(Positions[V][Axis] for V in (A, B, C)) + Radius < Center[Axis]
                           for Axis in range(3)):
                        continue
                    Distance = math.sqrt(TriangleDistanceSquared(
                        Center, Positions[A], Positions[B], Positions[C]))
                    MaxPenetration = max(MaxPenetration, max(0.0, Radius - Distance))
            if Velocities is not None:
                Kinetic = sum(0.5 * Mass * Dot(Velocity, Velocity)
                              for Mass, Velocity in zip(Fixture['VertexMassKg'], Velocities))
                MaxKinetic = max(MaxKinetic, Kinetic)
                LastKinetic = Kinetic
                MaxSpeed = max(MaxSpeed, max(Norm(Velocity) for Velocity in Velocities))
            GravityPotential = -sum(Mass * Dot(Fixture['GravityMPerS2'], Position)
                                    for Mass, Position in zip(Fixture['VertexMassKg'], Positions))
            MaxGravityPotential = max(MaxGravityPotential, GravityPotential)
            LastGravityPotential = GravityPotential
            RigidTranslationalKinetic = sum(
                0.5 * Colliders[State['Id']]['MassKg'] * Dot(State['LinearVelocity'],
                                                               State['LinearVelocity'])
                for State in States if Colliders[State['Id']]['Motion'] == 'dynamic' and
                Colliders[State['Id']]['MassKg'] is not None)
            MaxRigidTranslationalKinetic = max(MaxRigidTranslationalKinetic,
                                               RigidTranslationalKinetic)
            LastRigidTranslationalKinetic = RigidTranslationalKinetic
            RigidRotationalKinetic = sum(
                0.5 * Colliders[State['Id']]['InertiaKgM2'] *
                Dot(State['AngularVelocity'], State['AngularVelocity'])
                for State in States if Colliders[State['Id']]['Motion'] == 'dynamic' and
                'InertiaKgM2' in Colliders[State['Id']])
            MaxRigidRotationalKinetic = max(MaxRigidRotationalKinetic,
                                            RigidRotationalKinetic)
            LastRigidRotationalKinetic = RigidRotationalKinetic
            if any(Colliders[State['Id']]['Motion'] == 'dynamic' and
                   ('InertiaKgM2' not in Colliders[State['Id']] or
                    Colliders[State['Id']]['MassKg'] is None)
                   for State in States):
                AllRigidInertia = False
            Contacts = Row.get('Contacts')
            if Contacts is None:
                AllContactImpulses = False
            elif not isinstance(Contacts, list):
                raise ValueError('Contacts is not an array')
            else:
                ContactCount += len(Contacts)
                for Contact in Contacts:
                    if not isinstance(Contact, dict) or \
                            Contact.get('ColliderId') not in Colliders or \
                            Contact.get('Feature') not in ('vertex', 'edge', 'triangle') or \
                            not isinstance(Contact.get('Indices'), list) or \
                            len(Contact['Indices']) != {'vertex': 1, 'edge': 2,
                                                       'triangle': 3}[Contact['Feature']] or \
                            any(type(Index) is not int or not 0 <= Index < Count
                                for Index in Contact['Indices']) or \
                            not FiniteVector(Contact.get('Point')) or \
                            not FiniteVector(Contact.get('Normal')) or \
                            abs(Norm(Contact['Normal']) - 1.0) > 1e-3:
                        raise ValueError('Invalid contact geometry record')
                    PhaseTime = Contact.get('PhaseTimeSeconds')
                    if not isinstance(PhaseTime, (int, float)) or not math.isfinite(PhaseTime) or \
                            not Frame * Fixture['DeltaTimeSeconds'] - 2e-6 <= PhaseTime <= \
                            (Frame + 1) * Fixture['DeltaTimeSeconds'] + 2e-6:
                        AllContactImpulses = False
                    ClothImpulse = Contact.get('ImpulseOnClothNs')
                    RigidImpulse = Contact.get('ImpulseOnRigidNs')
                    if not FiniteVector(ClothImpulse) or not FiniteVector(RigidImpulse):
                        AllContactImpulses = False
                    else:
                        MaxImpulsePairResidual = max(MaxImpulsePairResidual,
                            Norm(tuple(ClothImpulse[Axis] + RigidImpulse[Axis]
                                       for Axis in range(3))))
            External = Row.get('External')
            if not isinstance(External, dict) or not all(
                    isinstance(External.get(Key), (int, float)) and
                    math.isfinite(External[Key]) for Key in ExternalWorkTotals):
                AllExternalWork = False
            else:
                for Key in ExternalWorkTotals:
                    ExternalWorkTotals[Key] += External[Key]
        if Stream.readline(1):
            raise ValueError('Contact trace has extra frames')
    FixtureSha = hashlib.sha256(FixturePath.read_bytes()).hexdigest()
    with TracePath.open('rb') as TraceBytes:
        CompressedTraceSha = hashlib.file_digest(TraceBytes, 'sha256').hexdigest()
    SourcePath = Directory / 'Source.json'
    ProvenanceStatus = 'INCOMPLETE'
    if SourcePath.is_file():
        Source = json.loads(SourcePath.read_text(encoding='utf-8'))
        for Key, Actual in (('FixtureSha256', FixtureSha),
                            ('TraceSha256', CompressedTraceSha)):
            if Key in Source and Source[Key] != Actual:
                raise ValueError(f'Source {Key} disagrees with recorded bytes')
        ReceiptPath = Directory / 'Receipt.json'
        if Source.get('ReceiptSha256') is not None and ReceiptPath.is_file() and \
                Source['ReceiptSha256'] != hashlib.sha256(ReceiptPath.read_bytes()).hexdigest():
            raise ValueError('Source receipt digest disagrees with recorded bytes')
        RequiredSource = ('Engine', 'SolverVariant', 'OS', 'Compiler', 'Workers',
                          'FixtureSha256', 'TraceSha256', 'ReceiptSha256')
        if ReceiptPath.is_file() and all(Source.get(Key) is not None
                                         for Key in RequiredSource):
            ProvenanceStatus = 'COMPLETE'
    Gates = Fixture['Gates']
    RestIsPlanar = Plane is not None
    TraceRemainsPlanar = RestIsPlanar and MaxOutOfPlane <= 1e-5
    GeometryPass = MaxAxial <= Gates['MaximumAxialStrain'] and \
        MaxPin <= Gates['MaximumPinErrorM'] and \
        MaxPenetration <= Gates['MaximumSpherePenetrationM'] and MinAreaRatio > 0 and \
        (not TraceRemainsPlanar or
         (OrientationFlips == 0 and PlanarBoundaryCrossings == 0)) and \
        ('MaximumContactCount' not in Gates or ContactCount <= Gates['MaximumContactCount']) and \
        ('MinimumContactCount' not in Gates or ContactCount >= Gates['MinimumContactCount']) and \
        ('MaximumContactImpulsePairResidualNs' not in Gates or
         (AllContactImpulses and MaxImpulsePairResidual <=
          Gates['MaximumContactImpulsePairResidualNs']))
    Result = {'Schema': 'Gargantuan.ContactVerdict.V1', 'CaseId': Fixture['CaseId'],
        'Frames': Fixture['Frames'], 'Vertices': Count,
        'FixtureSha256': FixtureSha,
        'CompressedTraceSha256': CompressedTraceSha,
        'UncompressedTraceSha256': TraceDigest.hexdigest(),
        'MaximumAxialStrain': MaxAxial, 'MaximumAllStructuralEdgeStrain': MaxAllEdge,
        'MaximumAxialCompression': MaxAxialCompression,
        'MaximumAxialTension': MaxAxialTension,
        'MaximumPinErrorM': MaxPin, 'MaximumSpherePenetrationM': MaxPenetration,
        'MinimumTriangleAreaRatio': MinAreaRatio,
        'ProjectedOrientationFlipInstances': OrientationFlips,
        'MaximumOutOfRestPlaneM': MaxOutOfPlane if RestIsPlanar else None,
        'PlanarInversionPass': OrientationFlips == 0 if TraceRemainsPlanar else None,
        'PlanarBoundaryCrossingPairs': PlanarBoundaryCrossings if TraceRemainsPlanar else None,
        'PlanarBoundaryPairChecks': BoundaryPairChecks if TraceRemainsPlanar else None,
        'MaximumClothKineticEnergyJ': MaxKinetic if AllVelocities else None,
        'FinalClothKineticEnergyJ': LastKinetic if AllVelocities else None,
        'InitialClothKineticEnergyJ': InitialKinetic,
        'MaximumClothSpeedMPerS': MaxSpeed if AllVelocities else None,
        'MaximumGravityPotentialJ': MaxGravityPotential,
        'FinalGravityPotentialJ': LastGravityPotential,
        'MaximumElasticSpringSurrogateJ': MaxElasticSurrogate if AllStiffness else None,
        'FinalElasticSpringSurrogateJ': LastElasticSurrogate if AllStiffness else None,
        'MaximumRigidTranslationalKineticEnergyJ': MaxRigidTranslationalKinetic,
        'FinalRigidTranslationalKineticEnergyJ': LastRigidTranslationalKinetic,
        'MaximumRigidRotationalKineticEnergyJ':
            MaxRigidRotationalKinetic if AllRigidInertia else None,
        'FinalRigidRotationalKineticEnergyJ':
            LastRigidRotationalKinetic if AllRigidInertia else None,
        'TotalContactRecords': ContactCount,
        'MaximumContactImpulsePairResidualNs':
            MaxImpulsePairResidual if AllContactImpulses else None,
        'GeometryGatePass': GeometryPass,
        'VelocityStatus': 'COMPLETE' if AllVelocities else 'INCOMPLETE',
        'RigidKineticStatus': 'COMPLETE' if AllRigidInertia else 'INCOMPLETE',
        'ContactImpulseStatus': 'COMPLETE' if AllContactImpulses else 'INCOMPLETE',
        'ExternalWorkStatus': 'RECORDED_UNCLOSED' if AllExternalWork else 'INCOMPLETE',
        'ExternalWorkTotalsJ': ExternalWorkTotals if AllExternalWork else None,
        'ElasticEnergyStatus': 'SURROGATE_ONLY' if AllStiffness else 'INCOMPLETE',
        'SelfIntersectionStatus':
            ('PLANAR_OVERLAP_OR_DEGENERACY' if
             TraceRemainsPlanar and (OrientationFlips or PlanarBoundaryCrossings or
                                     MinAreaRatio <= 0) else
             'CERTIFIED_ABSENT_PLANAR' if TraceRemainsPlanar else 'INCOMPLETE'),
        'ProvenanceStatus': ProvenanceStatus,
        'FullPhysicalQualification': 'INCOMPLETE'}
    return Result


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument('CaseDirectory', type=Path)
    Parser.add_argument('--output', type=Path)
    Args = Parser.parse_args()
    try:
        Result = Inspect(Args.CaseDirectory)
        Text = json.dumps(Result, indent=2, sort_keys=True) + '\n'
        if Args.output:
            Args.output.write_text(Text, encoding='utf-8')
        print(Text, end='')
        return 0 if Result['GeometryGatePass'] else 2
    except (OSError, ValueError, KeyError, TypeError, OverflowError) as Error:
        print(f'[Physics:ContactValidator] {Error}')
        return 2


if __name__ == '__main__':
    raise SystemExit(Main())
