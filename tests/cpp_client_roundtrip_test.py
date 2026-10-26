"""Actual appliance serializer -> installed SDK views -> Python NVR -> C++ replay."""
from pathlib import Path
import subprocess, sys, tempfile
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'sdk/python'))
from nuvo_depth import RecordingReader, RecordingWriter, decode_frame
with tempfile.TemporaryDirectory() as directory:
    root=Path(directory)
    subprocess.run([sys.argv[1],str(root)],check=True)
    packet=root/'geometry.nuv'
    subprocess.run([sys.argv[2],'decode',str(packet),'3'],check=True)
    frame=decode_frame(packet.read_bytes())
    assert frame.metadata['calibration_geometry']['matching']['baseline_m']==.1
    recording=root/'native.nvr'
    with RecordingWriter(recording) as writer:writer.write(frame)
    with RecordingReader(recording) as reader:assert next(iter(reader)).identity==frame.identity
    subprocess.run([sys.argv[2],'replay',str(recording),'3'],check=True)
