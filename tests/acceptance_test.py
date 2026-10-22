"""Missing/forged evidence and weakened performance inputs cannot create acceptance."""
import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/qualification'))
import acceptance
from compare_performance import compare

class AcceptanceTest(unittest.TestCase):
    def test_unexecuted_template_is_valid_but_both_gates_fail(self):
        record=acceptance.load(acceptance.REFERENCE)
        tests=acceptance.validate(record,acceptance.REFERENCE.parent)
        for gate in ('software','self-service'):
            with self.assertRaisesRegex(ValueError,'incomplete'):acceptance.gate(record,tests,gate)
        record['tests'].pop()
        with self.assertRaisesRegex(ValueError,'69'):acceptance.validate(record,acceptance.REFERENCE.parent)
    def test_false_pass_and_modified_requirements_are_rejected(self):
        record=acceptance.load(acceptance.REFERENCE)
        record['tests'][0]['status']='pass'
        with self.assertRaisesRegex(ValueError,'attempt'):acceptance.validate(record,ROOT)
        record['tests'][0]['status']='not_run';record['tests'][0]['required_for_final_acceptance']=False
        with self.assertRaisesRegex(ValueError,'scope'):acceptance.validate(record,ROOT)
    def test_omitted_deliverables_and_changed_local_evidence_are_rejected(self):
        record=acceptance.load(acceptance.REFERENCE);record['workstreams']=[]
        with self.assertRaisesRegex(ValueError,'24 deliverables'):acceptance.validate(record,ROOT)
        with tempfile.TemporaryDirectory() as temporary:
            base=Path(temporary);path=base/'receipt.json';path.write_text('{}')
            artifact={'path':'receipt.json','sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
            acceptance.evidence(artifact,base)
            path.write_text('{"changed":true}')
            with self.assertRaisesRegex(ValueError,'changed evidence'):acceptance.evidence(artifact,base)
    def test_performance_needs_complete_matched_cohorts_and_conservative_ratios(self):
        thresholds={'depth_rate_min_ratio':.95,'latency_p95_max_ratio':1.1,'steady_rss_max_ratio':1.1}
        run={'warmup_seconds':300,'seconds':600,'depth_hz':10,'latency_p95_ms':100,'rss_steady_bytes':1000000,'new_core_errors':0,'throttled':0,'swap_bytes':0}
        before={'source_commit':'a'*40,'artifact_sha256':'b'*64,'conditions_sha256':'c'*64,'runs':[dict(run) for _ in range(3)]}
        after=copy.deepcopy(before)
        self.assertTrue(compare(before,after,thresholds)['pass'])
        after['runs'][0]['depth_hz']=9
        self.assertFalse(compare(before,after,thresholds)['pass'])
        after=copy.deepcopy(before);after['runs'][0]['new_core_errors']=1
        self.assertFalse(compare(before,after,thresholds)['pass'])
        after['runs'][0]['seconds']=1
        with self.assertRaisesRegex(ValueError,'ten-minute'):compare(before,after,thresholds)
        after=copy.deepcopy(before);after['conditions_sha256']='d'*64
        with self.assertRaisesRegex(ValueError,'conditions'):compare(before,after,thresholds)

if __name__=='__main__':unittest.main()
