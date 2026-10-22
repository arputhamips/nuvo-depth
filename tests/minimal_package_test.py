"""Inspect a real minimal .deb and its linked dependency closure in CI."""
from pathlib import Path
import subprocess
import sys
import tempfile

package = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    subprocess.run(['dpkg-deb', '-x', str(package), str(root)], check=True)
    required = ['usr/bin/depthd', 'usr/bin/nuvoctl', 'usr/lib/nuvo-depth/nuvo-depth-read',
                'usr/lib/nuvo-depth/nuvoctl.py', 'usr/lib/nuvo-depth/nuvo-trigger-prepare',
                'usr/lib/nuvo-depth/nuvo_calibration_control.py', 'usr/lib/nuvo-depth/nuvo_calibration_store.py',
                'usr/lib/systemd/system/nuvo-depthd-native.service',
                'usr/share/nuvo-depth/calibration/front_left.yaml',
                'usr/share/nuvo-depth/calibration/front_right.yaml',
                'usr/share/nuvo-depth/calibration/profile.json',
                'etc/nuvo-depth/stereo.json', 'etc/nuvo-depth/exposure.json']
    assert all((root/name).is_file() for name in required)
    files = {str(p.relative_to(root)) for p in root.rglob('*') if p.is_file()}
    for excluded in ('gateway', 'nuvo-setup', 'system-agent', 'transport-rollback', 'usb-gadget',
                     'nuvo-calibration', 'python3/dist-packages', '/web/', 'encoding.json', 'depthd-assets'):
        assert not any(excluded in name for name in files), excluded
    for binary in ('usr/bin/depthd', 'usr/lib/nuvo-depth/nuvo-depth-read'):
        linked = subprocess.check_output(['ldd', str(root/binary)], text=True)
        for excluded in ('libavcodec', 'libavutil', 'libswscale', 'libssl', 'libboost_system', 'libEGL', 'libGLES'):
            # Upstream OpenCV/libcamera may have unrelated transitive libraries;
            # direct ELF NEEDED entries define Nuvo's feature dependency boundary.
            dynamic = subprocess.check_output(['readelf', '-d', str(root/binary)], text=True)
            assert excluded not in dynamic, (binary, excluded)
        assert 'not found' not in linked, linked
    service = (root/'usr/lib/systemd/system/nuvo-depthd-native.service').read_text()
    assert 'WantedBy=multi-user.target' in service
    assert 'nuvo-calibration' not in service and 'nuvo-depth.target' not in service
    controls = subprocess.check_output(['dpkg-deb', '-f', str(package), 'Depends'], text=True)
    for excluded in ('nuvo-depth-core', 'nuvo-depth-tools', 'nuvo-depth-calibration', 'python3-opencv', 'python3-numpy', 'network-manager'):
        assert excluded not in controls, controls
    assert 'python3-cryptography' in controls
    print('Minimal package payload, services, direct ELF dependencies and linkage verified.')
