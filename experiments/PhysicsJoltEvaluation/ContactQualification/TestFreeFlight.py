"""Analytic positive and negative controls for the prespecified free-flight gates."""

import unittest

from ValidateFreeFlight import FrameErrors, FreeFlightGatePass


class FreeFlightTests(unittest.TestCase):
    def test_rigid_uniform_translation_passes(self):
        Rest = [[0.0, 0.0, 0.0], [1.0, 0.0, 0.0]]
        Masses = [1.0, 1.0]
        Target = [2.0, 0.0, 0.0]
        Errors = FrameErrors(Rest, Target, Masses,
                             [0.2, 0.0, 0.0, 1.2, 0.0, 0.0],
                             [2.0, 0.0, 0.0, 2.0, 0.0, 0.0], 0.1)
        self.assertTrue(FreeFlightGatePass(Errors))

    def test_each_prespecified_bound_discriminates(self):
        Zero = dict(MaximumVertexVelocityErrorMPerS=0.0,
                    MaximumVertexPositionErrorM=0.0,
                    RelativeTotalMomentumDrift=0.0,
                    RelativeTotalKineticDrift=0.0)
        Limits = dict(MaximumVertexVelocityErrorMPerS=1e-5,
                      MaximumVertexPositionErrorM=2e-6,
                      RelativeTotalMomentumDrift=1e-5,
                      RelativeTotalKineticDrift=1e-5)
        for Name, Limit in Limits.items():
            Within = dict(Zero, **{Name: Limit})
            Beyond = dict(Zero, **{Name: Limit * 1.1})
            with self.subTest(Name=Name):
                self.assertTrue(FreeFlightGatePass(Within))
                self.assertFalse(FreeFlightGatePass(Beyond))


if __name__ == '__main__':
    unittest.main()
