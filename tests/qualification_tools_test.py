import importlib.util
from pathlib import Path
import tempfile
import unittest

SOURCE=Path(__file__).parents[1]/"tools/qualification/analyze_depth_accuracy.py"
spec=importlib.util.spec_from_file_location("accuracy",SOURCE); module=importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
RUNNER_SOURCE=Path(__file__).parents[1]/"tools/qualification/run_qualification.py"
runner_spec=importlib.util.spec_from_file_location("qualification_runner",RUNNER_SOURCE)
runner=importlib.util.module_from_spec(runner_spec); runner_spec.loader.exec_module(runner)

class QualificationTest(unittest.TestCase):
    def test_existing_automated_evidence_is_identity_checked(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); path=root/"capture.json"
            path.write_text('{"id":"capture","status":"pass"}\n')
            result=runner.existing_gate({"id":"capture"},root)
            self.assertEqual(result["status"],"pass")
            self.assertEqual(len(result["sha256"]),64)
            path.write_text('{"id":"different","status":"pass"}\n')
            self.assertEqual(runner.existing_gate({"id":"capture"},root)["status"],"fail")

    def test_accuracy_requires_error_and_coverage(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/"accuracy.csv"
            path.write_text("target_m,measured_m,valid\n1,1.01,1\n2,2.01,1\n3,3,0\n")
            self.assertEqual(module.analyze(path,.02,.02,.6)["status"],"pass")
            self.assertEqual(module.analyze(path,.02,.02,.9)["status"],"fail")

if __name__=="__main__": unittest.main()
