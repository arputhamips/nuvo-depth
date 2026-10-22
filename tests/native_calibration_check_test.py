"""Exercise real native geometry validation without opening cameras."""
from pathlib import Path
import subprocess
import sys
import tempfile
from calibration_test_support import prepare_root,bundle

with tempfile.TemporaryDirectory() as directory:
    root=Path(directory);prepare_root(root);target,value=bundle(root)
    command=[sys.argv[1],'--calibration-check',str(root/'etc/nuvo-depth/daemon.yaml'),str(target),str(root/'var/lib/nuvo-depth/identity/device.uuid'),'user']
    result=subprocess.run(command,capture_output=True,text=True)
    assert result.returncode==0,result.stderr
    (root/'var/lib/nuvo-depth/identity/device.uuid').write_text('other-unit')
    assert subprocess.run(command,capture_output=True).returncode!=0
    print('Native calibration geometry and device-binding preflight passed without camera capture.')
