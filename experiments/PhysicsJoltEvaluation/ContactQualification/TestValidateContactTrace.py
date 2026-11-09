"""Small independent controls for the cloth contact trace inspector."""

import copy
import gzip
import json
from pathlib import Path
import tempfile
import unittest

from ValidateContactTrace import BoundaryCrossings, Inspect, TriangleDistanceSquared


def Fixture():
    Rest = [[0.0, 0.0, 0.0], [1.0, 0.0, 0.0],
            [0.0, 1.0, 0.0], [1.0, 1.0, 0.0]]
    return {
        'Schema': 'Gargantuan.ContactFixture.V1', 'CaseId': 'unit_square',
        'Frames': 2, 'DeltaTimeSeconds': 0.01,
        'RestPositions': Rest,
        'InitialVelocities': [[0.0, 0.0, 0.0] for _ in Rest],
        'VertexMassKg': [0.0, 1.0, 1.0, 1.0],
        'Triangles': [[0, 1, 2], [1, 3, 2]],
        'StructuralEdges': [
            {'A': A, 'B': B, 'RestLengthM': 1.0, 'Kind': 'axial'}
            for A, B in ((0, 1), (0, 2), (1, 3), (2, 3))],
        'Pins': [{'Vertex': 0, 'Target': Rest[0]}],
        'ThicknessM': 0.0, 'GravityMPerS2': [0.0, 0.0, 0.0],
        'Colliders': [{'Id': 'sphere', 'Kind': 'sphere', 'RadiusM': 0.25,
                       'MassKg': None, 'Motion': 'static',
                       'Trajectory': {'Kind': 'hold'}}],
        'Gates': {'MaximumAxialStrain': 0.1, 'MaximumPinErrorM': 1e-4,
                  'MaximumSpherePenetrationM': 1e-6}}


def Frames(Positions=None, Center=None):
    Positions = Positions or Fixture()['RestPositions']
    Center = Center or [0.5, 0.5, 2.0]
    Flat = [Coordinate for Vertex in Positions for Coordinate in Vertex]
    return [{
        'Frame': Frame, 'TimeSeconds': (Frame + 1) * 0.01,
        'Positions': Flat, 'Velocities': [0.0] * len(Flat),
        'ColliderStates': [{'Id': 'sphere', 'Center': Center,
                            'OrientationWxyz': [1.0, 0.0, 0.0, 0.0],
                            'LinearVelocity': [0.0, 0.0, 0.0],
                            'AngularVelocity': [0.0, 0.0, 0.0]}]
    } for Frame in range(2)]


def InspectCase(FixtureValue, Records):
    with tempfile.TemporaryDirectory() as Temporary:
        Directory = Path(Temporary)
        (Directory / 'ContactFixtureV1.json').write_text(json.dumps(FixtureValue),
                                                          encoding='utf-8')
        with gzip.open(Directory / 'ContactTraceV1.jsonl.gz', 'wt',
                       encoding='utf-8') as Stream:
            for Record in Records:
                Stream.write(json.dumps(Record) + '\n')
        return Inspect(Directory)


class ContactTraceTests(unittest.TestCase):
    def test_clear_surface_keeps_missing_physics_incomplete(self):
        Result = InspectCase(Fixture(), Frames())
        self.assertTrue(Result['GeometryGatePass'])
        self.assertEqual(Result['VelocityStatus'], 'COMPLETE')
        self.assertEqual(Result['ContactImpulseStatus'], 'INCOMPLETE')
        self.assertEqual(Result['FullPhysicalQualification'], 'INCOMPLETE')
        self.assertEqual(Result['SelfIntersectionStatus'], 'CERTIFIED_ABSENT_PLANAR')

    def test_face_penetration_fails_prespecified_gate(self):
        Result = InspectCase(Fixture(), Frames(Center=[0.25, 0.25, 0.15]))
        self.assertFalse(Result['GeometryGatePass'])
        self.assertAlmostEqual(Result['MaximumSpherePenetrationM'], 0.1)

    def test_closest_point_regions(self):
        A, B, C = (0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)
        self.assertAlmostEqual(TriangleDistanceSquared((0.2, 0.2, 0.3), A, B, C), 0.09)
        self.assertAlmostEqual(TriangleDistanceSquared((0.5, -0.2, 0.3), A, B, C), 0.13)
        self.assertAlmostEqual(TriangleDistanceSquared((-0.2, -0.2, 0.3), A, B, C), 0.17)

    def test_planar_inversion_fails_without_strain_confusion(self):
        # Swap two free vertices across a triangle's oriented rest plane.
        Rest = Fixture()['RestPositions']
        Inverted = copy.deepcopy(Rest)
        Inverted[1], Inverted[2] = Inverted[2], Inverted[1]
        Result = InspectCase(Fixture(), Frames(Positions=Inverted))
        self.assertFalse(Result['GeometryGatePass'])
        self.assertFalse(Result['PlanarInversionPass'])

    def test_missing_velocity_marks_kinetics_incomplete(self):
        Records = Frames()
        Records[1].pop('Velocities')
        Result = InspectCase(Fixture(), Records)
        self.assertTrue(Result['GeometryGatePass'])
        self.assertEqual(Result['VelocityStatus'], 'INCOMPLETE')
        self.assertIsNone(Result['FinalClothKineticEnergyJ'])

    def test_rotated_planar_mesh_is_certified_in_its_rest_plane(self):
        Case = Fixture()
        Case['RestPositions'] = [[X, 0.0, Y] for X, Y, _ in Case['RestPositions']]
        Case['Pins'][0]['Target'] = Case['RestPositions'][0]
        Result = InspectCase(Case, Frames(Positions=Case['RestPositions']))
        self.assertEqual(Result['SelfIntersectionStatus'], 'CERTIFIED_ABSENT_PLANAR')
        self.assertEqual(Result['MaximumOutOfRestPlaneM'], 0.0)

    def test_crossed_boundary_is_detected(self):
        Bow = [(0.0, 0.0, 0.0), (1.0, 1.0, 0.0),
               (0.0, 1.0, 0.0), (1.0, 0.0, 0.0)]
        Crossings, _ = BoundaryCrossings(Bow, [0, 1, 2, 3],
                                          Bow[0], (1.0, 0.0, 0.0),
                                          (0.0, 1.0, 0.0), 100)
        self.assertEqual(Crossings, 1)

    def test_inconsistent_winding_cannot_claim_planar_injectivity(self):
        Case = Fixture()
        Case['Triangles'][1] = [1, 2, 3]
        with self.assertRaisesRegex(ValueError, 'manifold disk'):
            InspectCase(Case, Frames())

    def test_duplicate_frame_fails_closed(self):
        Records = Frames()
        Records[1]['Frame'] = 0
        with self.assertRaisesRegex(ValueError, 'frame index'):
            InspectCase(Fixture(), Records)


if __name__ == '__main__':
    unittest.main()
