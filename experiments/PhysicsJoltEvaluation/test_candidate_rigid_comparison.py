import copy
import unittest

from RunCandidateRigidComparison import PhysicalRow


class PhysicalSummaryTests(unittest.TestCase):
    def test_query_and_nested_shape_timings_do_not_change_summary(self):
        Timing = {'P50Ms': 1, 'P95Ms': 2, 'P99Ms': 3, 'MaximumMs': 4, 'RawMs': [1, 4]}
        First = {'Case': 'advanced-queries', 'Raycast': Timing,
                 'Shapes': [{'Kind': 'Sphere', 'Hits': 7, 'Cast': Timing, 'Overlap': Timing}],
                 'MeshCreateMs': 15, 'Resources': {'WorkingSetAfterBytes': 100},
                 'FinalBallY': 0.5, 'Quality': {'Status': 'pass'}}
        Second = copy.deepcopy(First)
        Second['Raycast']['P50Ms'] = 10
        Second['Shapes'][0]['Overlap']['RawMs'] = [30, 40]
        Second['MeshCreateMs'] = 120
        Second['Resources']['WorkingSetAfterBytes'] = 200
        self.assertEqual(PhysicalRow(First), PhysicalRow(Second))
        self.assertEqual(PhysicalRow(First)['Shapes'], [{'Kind': 'Sphere', 'Hits': 7}])

    def test_physical_state_counts_and_quality_remain_significant(self):
        First = {'FinalBallY': 0.5, 'Shapes': [{'Kind': 'Sphere', 'Hits': 7}],
                 'Quality': {'Status': 'pass'}}
        for Key, Value in [('FinalBallY', 0.4), ('Shapes', [{'Kind': 'Sphere', 'Hits': 6}]),
                           ('Quality', {'Status': 'fail'})]:
            Second = copy.deepcopy(First)
            Second[Key] = Value
            self.assertNotEqual(PhysicalRow(First), PhysicalRow(Second))


if __name__ == '__main__':
    unittest.main()
