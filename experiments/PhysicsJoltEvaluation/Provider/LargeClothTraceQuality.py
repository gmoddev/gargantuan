"""Inspect the fixed 10-warmup plus 30-measured large-cloth physical window."""

import math
import struct


def ReadStateFrameHashes(LogText, Summary, PhysicalFrames):
    Prefix = '[Physics:ProviderBenchmark] state_frame='
    Hashes = []
    for Line in LogText.splitlines():
        if not Line.startswith(Prefix):
            continue
        Fields = Line[len(Prefix):].split()
        if len(Fields) != 2 or not Fields[0].isdecimal() or \
                Fields[1].split('=', 1)[0] != 'soft_trajectory_hash' or \
                not Fields[1].split('=', 1)[-1].isdecimal():
            raise ValueError('Large-cloth state-frame hash line is malformed')
        Index = int(Fields[0])
        Hash = int(Fields[1].split('=', 1)[1])
        if Index != len(Hashes) or Hash > 0xffffffffffffffff:
            raise ValueError('Large-cloth state-frame hashes are out of order or range')
        Hashes.append(Hash)
    if len(Hashes) != PhysicalFrames or Summary.get('soft_trajectory_hash') != str(Hashes[-1]):
        raise ValueError('Large-cloth state-frame hashes or final summary are incomplete')
    return Hashes


def InspectLargeClothTrace(Path, Summary, LogText, Workers, GroupBatch):
    MeasuredFrames = 30
    WarmupFrames = 10
    PhysicalFrames = WarmupFrames + MeasuredFrames
    Resolution = 128
    Header = struct.Struct('<8sIIIIIf')
    Vertex = struct.Struct('<fff')
    FrameBytes = Resolution * Resolution * Vertex.size
    if Path.stat().st_size != Header.size + PhysicalFrames * FrameBytes:
        raise ValueError('Large-cloth trace byte count is incomplete')
    Expected = {'backend': 'jolt', 'scene': 'cloth16k', 'coupling': 'none',
        'frames': str(MeasuredFrames), 'warmup_frames': str(WarmupFrames),
        'workers_requested': str(Workers), 'quality_trace': '1',
        'jolt_world_updates': '2', 'jolt_collision_steps_per_update': '8',
        'jolt_soft_iterations_per_collision_step': '5',
        'jolt_constraint_sweeps_per_iteration': '2',
        'jolt_soft_integrations_per_frame': '80', 'jolt_full_update_sleep_clock': '1',
        'jolt_large_cloth_group_batch': GroupBatch}
    if any(Summary.get(Key) != Value for Key, Value in Expected.items()):
        raise ValueError('Large-cloth summary does not match final source profile')
    StateHashes = ReadStateFrameHashes(LogText, Summary, PhysicalFrames)
    ComputedHash = 14695981039346656037
    RestEdge = 8.0 / (Resolution - 1)
    PeakStrain = 0.0
    PeakPinError = 0.0
    FinalStrain = 0.0
    with Path.open('rb') as Stream:
        Prefix = Header.unpack(Stream.read(Header.size))
        if Prefix[:6] != (b'PJTRACE1', 1, 1, Resolution * Resolution,
                          PhysicalFrames, WarmupFrames) or \
                not math.isfinite(Prefix[6]) or abs(Prefix[6] - 1.0 / 60.0) > 1e-7:
            raise ValueError('Large-cloth trace header disagrees with fixture')
        for Frame in range(PhysicalFrames):
            Raw = Stream.read(FrameBytes)
            for (Bits,) in struct.iter_unpack('<I', Raw):
                ComputedHash = ((ComputedHash ^ Bits) * 1099511628211) & 0xffffffffffffffff
            if ComputedHash != StateHashes[Frame]:
                raise ValueError('Large-cloth trace disagrees with state-frame hash')
            Positions = list(struct.iter_unpack('<fff', Raw))
            if len(Positions) != Resolution * Resolution or \
                    any(not all(math.isfinite(Value) for Value in Position)
                        for Position in Positions):
                raise ValueError('Large-cloth frame missing or nonfinite')
            FrameStrain = 0.0
            for Y in range(Resolution):
                for X in range(Resolution):
                    Index = Y * Resolution + X
                    Position = Positions[Index]
                    if Y == 0:
                        Pin = (-4.0 + 8.0 * X / (Resolution - 1), 7.0, 0.0)
                        PeakPinError = max(PeakPinError, math.dist(Position, Pin))
                    for Neighbor in ((Index + 1,) if X + 1 < Resolution else ()) + \
                            ((Index + Resolution,) if Y + 1 < Resolution else ()):
                        Strain = abs(math.dist(Position, Positions[Neighbor]) / RestEdge - 1.0)
                        FrameStrain = max(FrameStrain, Strain)
            PeakStrain = max(PeakStrain, FrameStrain)
            FinalStrain = FrameStrain
    return {'Workers': Workers, 'Frames': MeasuredFrames,
        'WarmupFrames': WarmupFrames, 'PhysicalFrames': PhysicalFrames,
        'FinalFrameIndex': PhysicalFrames - 1, 'PeakPinErrorM': PeakPinError,
        'VerifiedStateFrameHashes': len(StateHashes),
        'HashLogPass': True,
        'FinalSoftTrajectoryHash': ComputedHash,
        'PeakEdgeStrain': PeakStrain, 'FinalEdgeStrain': FinalStrain,
        'MaximumPinErrorM': 1e-4, 'MaximumPeakEdgeStrain': 0.05,
        'MaximumFinalEdgeStrain': 0.02,
        'QualityPass': PeakPinError <= 1e-4 and PeakStrain <= 0.05 and
                       FinalStrain <= 0.02}
