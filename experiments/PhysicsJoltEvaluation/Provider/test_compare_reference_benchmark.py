"""Keep experimental XPBD substeps outside production timing ranking."""

from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import sys
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch

import CompareBenchmark as Comparator


class CompareReferenceBenchmarkTests(unittest.TestCase):
    def test_reference_stays_unranked_even_with_passing_envelope(self):
        Common = {'scene': 'cloth4k', 'coupling': 'none', 'frames': '30',
                  'warmup_frames': '10', 'workers_requested': '1',
                  'host': 'host', 'os': 'windows', 'build_stamp': 'stamp',
                  'binary_id': 'binary', 'run_pair': 'p0000',
                  'timing_instrumented': '0', 'constraints': '100',
                  'xpbd_substeps_requested': '8',
                  'mean_y': '1', 'center_y': '1', 'max_pin_error': '0',
                  'mean_edge_strain': '0.01', 'max_edge_strain': '0.02'}
        Jolt = {**Common, 'backend': 'jolt', 'xpbd_reference': '0',
                'xpbd_substeps_actual': '0', 'mean_total_ms': '10'}
        Xpbd = {**Common, 'backend': 'xpbd', 'xpbd_reference': '1',
                'xpbd_substeps_actual': '8', 'xpbd_workers_actual': '1',
                'xpbd_jobs_dispatched': '8',
                'xpbd_mean_worker_utilization': '0.5',
                'mean_total_ms': '2'}
        Fields = Comparator.COMMON_FIELDS
        Envelope = {Field: 1 for Field in Fields}
        Envelope['absolute_ranges'] = {Field: [0, 2] for Field in Fields}
        with TemporaryDirectory() as Directory:
            EnvelopePath = Path(Directory) / 'envelope.json'
            EnvelopePath.write_text(json.dumps(Envelope))
            Output = io.StringIO()
            with patch.object(Comparator, 'ReadSummary', side_effect=(Jolt, Xpbd)), \
                 patch.object(sys, 'argv', ['CompareBenchmark.py', 'jolt', 'xpbd',
                                           '--envelope', str(EnvelopePath)]), \
                 redirect_stdout(Output):
                Comparator.Main()
            Result = json.loads(Output.getvalue())
            self.assertTrue(Result['quality_comparable'])
            self.assertTrue(Result['xpbd_reference'])
            self.assertTrue(Result['xpbd_reference_request_matched'])
            self.assertIsNone(Result['timing_winner'])


if __name__ == '__main__':
    unittest.main()
