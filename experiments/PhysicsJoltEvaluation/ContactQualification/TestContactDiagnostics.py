"""Analytic controls for bounded contact intersection and accounting checks."""

import gzip
import json
from pathlib import Path
import tempfile
import unittest

from Inspect3DSelfIntersection import TrianglesIntersect
from InspectEnergyLedger import Inspect as InspectEnergy, RawMomentumClosurePass, VerifyAccounting
from TestValidateContactTrace import Fixture, Frames


def Accounting():
    return {
        'NormalImpulseOnClothNs': [0.0, 0.0, 0.0],
        'FrictionImpulseOnClothNs': [0.0, 0.0, 0.0],
        'PrescribedColliderReactionImpulseNs': [0.0, 0.0, 0.0],
        'MomentumStartKgMPerS': [0.0, 0.0, 0.0],
        'MomentumEndKgMPerS': [0.0, 0.0, 0.0],
        'GravityForceImpulseNs': [0.0, 0.0, 0.0],
        'UniformImpulseNs': [0.0, 0.0, 0.0],
        'PointImpulseNs': [0.0, 0.0, 0.0],
        'DampingImpulseNs': [0.0, 0.0, 0.0],
        'PinConstraintImpulseOnClothNs': [0.0, 0.0, 0.0],
        'MomentumResidualNs': [0.0, 0.0, 0.0],
        'ContactKineticDeltaJ': 0.0, 'PrescribedSurfaceWorkJ': 0.0,
        'ContactEnergyResidualJ': 0.0, 'KineticStartJ': 0.0,
        'KineticEndJ': 0.0, 'AppliedDynamicsKineticDeltaJ': 0.0,
        'GravityForceKineticDeltaJ': 0.0,
        'UniformImpulseKineticDeltaJ': 0.0,
        'PointImpulseKineticDeltaJ': 0.0,
        'DampingKineticDeltaJ': 0.0,
        'StructuralProjectionKineticDeltaJ': 0.0,
        'PinProjectionKineticDeltaJ': 0.0, 'LedgerResidualJ': 0.0,
        'SubstepSeconds': 0.01 / 32,
        'PrescribedSurfaceVelocitySource': 'center(end)-center(start) per substep',
        'Method': 'source-contact-projection-and-previous-position-deltas',
        'Corrections': 0, 'StaticPoseWorkComplete': True}


def EnergyCase(Change=None):
    Case = Fixture()
    Case['Authored'] = {'Material': {'StretchCompliance': 1e-6,
                                    'BendCompliance': 1e-4},
                        'XpbdSubsteps': 32}
    Records = Frames()
    for Record in Records:
        Record['Stages'] = {'ContactProjectionVelocityDerivedMomentumDeltaNs':
                            [0.0, 0.0, 0.0], 'ContactProjectionCorrections': 0}
        Record['External'] = {'XpbdContactAccounting': Accounting()}
    if Change:
        Change(Records)
    with tempfile.TemporaryDirectory() as Temporary:
        Directory = Path(Temporary)
        (Directory / 'ContactFixtureV1.json').write_text(json.dumps(Case), encoding='utf-8')
        with gzip.open(Directory / 'ContactTraceV1.jsonl.gz', 'wt', encoding='utf-8') as Stream:
            for Record in Records:
                Stream.write(json.dumps(Record) + '\n')
        return InspectEnergy(Directory)


class DiagnosticTests(unittest.TestCase):
    def test_prespecified_direct_raw_momentum_gate(self):
        self.assertTrue(RawMomentumClosurePass(9e-6))
        self.assertFalse(RawMomentumClosurePass(1.1e-5))
        self.assertFalse(RawMomentumClosurePass(None))

    def test_disjoint_triangle_crossing_and_separation(self):
        A = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
        Crossing = [(.25, .25, -1), (.25, .25, 1), (.75, .25, 0)]
        Separate = [(2, 2, 0), (3, 2, 0), (2, 3, 0)]
        CoplanarOverlap = [(.1, .1, 0), (.4, .1, 0), (.1, .4, 0)]
        self.assertTrue(TrianglesIntersect(A, Crossing))
        self.assertFalse(TrianglesIntersect(A, Separate))
        self.assertTrue(TrianglesIntersect(A, CoplanarOverlap))

    def test_zero_stage_ledger_has_zero_physical_residual(self):
        Result = EnergyCase()
        self.assertEqual(Result['AccountingIdentityStatus'], 'VERIFIED')
        self.assertEqual(Result['MaximumAbsoluteUnclosedPhysicalResidualJ'], 0.0)

    def test_broken_impulse_split_fails_closed(self):
        def Break(Records):
            Records[0]['External']['XpbdContactAccounting']['NormalImpulseOnClothNs'][0] = 1
        with self.assertRaisesRegex(ValueError, 'impulse split'):
            EnergyCase(Break)

    def test_single_contact_kinetic_and_surface_work_identity(self):
        # m=1 kg, delta-v=1 m/s, surface speed=.5 m/s:
        # J=1 Ns, delta-K=.5 J, J dot v_surface=.5 J.
        Record = Accounting()
        Record.update(NormalImpulseOnClothNs=[1.0, 0.0, 0.0],
                      PrescribedColliderReactionImpulseNs=[-1.0, 0.0, 0.0],
                      MomentumEndKgMPerS=[1.0, 0.0, 0.0],
                      ContactKineticDeltaJ=0.5, PrescribedSurfaceWorkJ=0.5,
                      KineticEndJ=0.5, Corrections=1)
        Stages = {'ContactProjectionVelocityDerivedMomentumDeltaNs':
                  [1.0, 0.0, 0.0], 'ContactProjectionCorrections': 1}
        self.assertEqual(VerifyAccounting(Record, Stages, 0,
                         {'DeltaTimeSeconds': 0.01,
                          'Authored': {'XpbdSubsteps': 32}})['ContactEnergyResidualJ'], 0.0)
        Broken = dict(Record, ContactEnergyResidualJ=0.1)
        with self.assertRaisesRegex(ValueError, 'stage ledger'):
            VerifyAccounting(Broken, Stages, 0,
                             {'DeltaTimeSeconds': 0.01,
                              'Authored': {'XpbdSubsteps': 32}})

    def test_static_pose_change_disables_physical_closure(self):
        def Mark(Records):
            Records[1]['External']['XpbdContactAccounting']['StaticPoseWorkComplete'] = False
        Result = EnergyCase(Mark)
        self.assertEqual(Result['PhysicalResidualStatus'], 'INCOMPLETE')
        self.assertEqual(Result['StaticPoseWorkIncompleteFrames'], [1])

    def test_momentum_residual_is_reported_without_invented_gate(self):
        Record = Accounting()
        Record['MomentumEndKgMPerS'] = [1.0, 0.0, 0.0]
        Record['MomentumResidualNs'] = [1.0, 0.0, 0.0]
        Stages = {'ContactProjectionVelocityDerivedMomentumDeltaNs':
                  [0.0, 0.0, 0.0], 'ContactProjectionCorrections': 0}
        Reduced = VerifyAccounting(Record, Stages, 0,
                                   {'DeltaTimeSeconds': 0.01,
                                    'Authored': {'XpbdSubsteps': 32}})
        self.assertEqual(Reduced['NormalizedMomentumResidual'], 1.0)

    def test_free_edge_roundoff_preserves_raw_leakage_metric(self):
        Record = Accounting()
        Record['MomentumEndKgMPerS'] = [1.0, 0.0, 0.0]
        Record['UnpinnedStructuralNumericalImpulseNs'] = [1.0, 0.0, 0.0]
        Stages = {'ContactProjectionVelocityDerivedMomentumDeltaNs':
                  [0.0, 0.0, 0.0], 'ContactProjectionCorrections': 0}
        Reduced = VerifyAccounting(Record, Stages, 0,
                                   {'DeltaTimeSeconds': 0.01,
                                    'Authored': {'XpbdSubsteps': 32}})
        self.assertEqual(Reduced['NormalizedMomentumResidual'], 0.0)
        self.assertEqual(Reduced['NormalizedRawMomentumResidual'], 1.0)
        self.assertEqual(Reduced['NormalizedUnpinnedStructuralNumericalImpulse'], 1.0)

    def test_ideal_velocity_uses_published_raw_momentum_not_would_be_roundoff(self):
        Record = Accounting()
        Record['IdealVelocityReconstructionEnabled'] = True
        Record['IntegrationPositionRoundingKineticDeltaJ'] = 0.25
        Record['IdealVelocityReconstructionKineticDeltaJ'] = -0.25
        Record['UnpinnedStructuralNumericalImpulseNs'] = [1.0, 0.0, 0.0]
        Stages = {'ContactProjectionVelocityDerivedMomentumDeltaNs':
                  [0.0, 0.0, 0.0], 'ContactProjectionCorrections': 0}
        Reduced = VerifyAccounting(Record, Stages, 0,
                                   {'DeltaTimeSeconds': 0.01,
                                    'Authored': {'XpbdSubsteps': 32}})
        self.assertTrue(Reduced['IdealVelocityReconstructionEnabled'])
        self.assertEqual(Reduced['NormalizedRawMomentumResidual'], 0.0)
        self.assertEqual(Reduced['NormalizedUnpinnedStructuralNumericalImpulse'], 1.0)
        Record['MomentumEndKgMPerS'] = [0.5, 0.0, 0.0]
        Record['MomentumResidualNs'] = [0.5, 0.0, 0.0]
        Reduced = VerifyAccounting(Record, Stages, 0,
                                   {'DeltaTimeSeconds': 0.01,
                                    'Authored': {'XpbdSubsteps': 32}})
        self.assertEqual(Reduced['NormalizedRawMomentumResidual'], 0.5)
        self.assertEqual(Reduced['NormalizedMomentumResidual'], 0.5)

    def test_ideal_velocity_kinetic_gaps_and_metadata_fail_closed(self):
        Record = Accounting()
        Record['IdealVelocityReconstructionEnabled'] = True
        Record['IntegrationPositionRoundingKineticDeltaJ'] = 0.25
        Record['IdealVelocityReconstructionKineticDeltaJ'] = -0.25
        Stages = {'ContactProjectionVelocityDerivedMomentumDeltaNs':
                  [0.0, 0.0, 0.0], 'ContactProjectionCorrections': 0}
        VerifyAccounting(Record, Stages, 0,
                         {'DeltaTimeSeconds': 0.01,
                          'Authored': {'XpbdSubsteps': 32}})
        Broken = dict(Record, IdealVelocityReconstructionKineticDeltaJ=-0.2)
        with self.assertRaisesRegex(ValueError, 'kinetic stage ledger'):
            VerifyAccounting(Broken, Stages, 0,
                             {'DeltaTimeSeconds': 0.01,
                              'Authored': {'XpbdSubsteps': 32}})
        Broken = dict(Record)
        del Broken['IntegrationPositionRoundingKineticDeltaJ']
        with self.assertRaisesRegex(ValueError, 'ideal velocity reconstruction metadata'):
            VerifyAccounting(Broken, Stages, 0,
                             {'DeltaTimeSeconds': 0.01,
                              'Authored': {'XpbdSubsteps': 32}})

    def test_pin_position_store_gap_is_reported_separately(self):
        Record = Accounting()
        Record['IdealVelocityReconstructionEnabled'] = True
        Record['IntegrationPositionRoundingKineticDeltaJ'] = 0.0
        Record['IdealVelocityReconstructionKineticDeltaJ'] = 0.0
        Record['PinConstraintNumericalImpulseNs'] = [0.25, 0.0, 0.0]
        Stages = {'ContactProjectionVelocityDerivedMomentumDeltaNs':
                  [0.0, 0.0, 0.0], 'ContactProjectionCorrections': 0}
        Reduced = VerifyAccounting(Record, Stages, 0,
                                   {'DeltaTimeSeconds': 0.01,
                                    'Authored': {'XpbdSubsteps': 32}})
        self.assertEqual(Reduced['NormalizedPinConstraintNumericalImpulse'], 0.25)
        self.assertEqual(Reduced['NormalizedRawMomentumResidual'], 0.0)
        Record['PinConstraintNumericalImpulseNs'] = [float('nan'), 0.0, 0.0]
        with self.assertRaisesRegex(ValueError, 'pin constraint numerical impulse'):
            VerifyAccounting(Record, Stages, 0,
                             {'DeltaTimeSeconds': 0.01,
                              'Authored': {'XpbdSubsteps': 32}})

    def test_double_shadow_raw_momentum_does_not_subtract_legacy_roundoff(self):
        Record = Accounting()
        Record['IdealVelocityReconstructionEnabled'] = False
        Record['DoubleShadowEnabled'] = True
        Record['VelocityPrecisionMode'] = '2'
        Record['IntegrationPositionRoundingKineticDeltaJ'] = 0.25
        Record['IdealVelocityReconstructionKineticDeltaJ'] = -0.25
        Record['UnpinnedStructuralNumericalImpulseNs'] = [1.0, 0.0, 0.0]
        Stages = {'ContactProjectionVelocityDerivedMomentumDeltaNs':
                  [0.0, 0.0, 0.0], 'ContactProjectionCorrections': 0}
        Reduced = VerifyAccounting(Record, Stages, 0,
                                   {'DeltaTimeSeconds': 0.01,
                                    'Authored': {'XpbdSubsteps': 32}})
        self.assertEqual(Reduced['VelocityPrecisionMode'], '2')
        self.assertEqual(Reduced['NormalizedRawMomentumResidual'], 0.0)
        self.assertEqual(Reduced['NormalizedUnpinnedStructuralNumericalImpulse'], 1.0)
        Broken = dict(Record, DoubleShadowEnabled=False)
        with self.assertRaisesRegex(ValueError, 'velocity precision mode'):
            VerifyAccounting(Broken, Stages, 0,
                             {'DeltaTimeSeconds': 0.01,
                              'Authored': {'XpbdSubsteps': 32}})
        Broken = dict(Record)
        del Broken['IntegrationPositionRoundingKineticDeltaJ']
        with self.assertRaisesRegex(ValueError, 'ideal velocity reconstruction metadata'):
            VerifyAccounting(Broken, Stages, 0,
                             {'DeltaTimeSeconds': 0.01,
                              'Authored': {'XpbdSubsteps': 32}})


if __name__ == '__main__':
    unittest.main()
