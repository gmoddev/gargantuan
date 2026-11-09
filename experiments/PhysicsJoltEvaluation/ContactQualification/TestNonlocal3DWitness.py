"""Analytic edge-through-face positive and separated negative controls."""

import unittest

import numpy as np

from FindNonlocal3DWitness import Search


class NonlocalWitnessTests(unittest.TestCase):
    def test_strict_disjoint_triangle_piercing(self):
        Positions = np.asarray([
            [0.2, 0.2, -0.1], [0.2, 0.2, 0.1], [0.8, 0.2, 0.1],
            [0.0, 0.0, 0.0], [1.0, 0.0, 0.0], [0.0, 1.0, 0.0],
        ], dtype=np.float64)
        Faces = np.asarray([[0, 1, 2], [3, 4, 5]], dtype=np.int32)
        Result = Search(Positions, Faces, 0.12)
        self.assertEqual(Result['Status'], 'STRICT_NONLOCAL_INTERSECTION')
        self.assertEqual(Result['FaceIndices'], [1, 0])
        Hit = Result['Witness']
        self.assertAlmostEqual(Hit['SegmentFraction'], 0.5)
        self.assertGreater(Hit['MinimumFaceBarycentric'], 0.19)
        self.assertAlmostEqual(Hit['PointM'][2], 0.0)

    def test_separated_triangles_do_not_report_intersection(self):
        Positions = np.asarray([
            [0.2, 0.2, 0.5], [0.2, 0.2, 0.7], [0.8, 0.2, 0.7],
            [0.0, 0.0, 0.0], [1.0, 0.0, 0.0], [0.0, 1.0, 0.0],
        ], dtype=np.float64)
        Faces = np.asarray([[0, 1, 2], [3, 4, 5]], dtype=np.int32)
        Result = Search(Positions, Faces, 0.12)
        self.assertEqual(Result['Status'], 'NO_STRICT_INTERIOR_EDGE_FACE_WITNESS_WITHIN_BOUNDS')


if __name__ == '__main__':
    unittest.main()
