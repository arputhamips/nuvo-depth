"""Execute offline examples, invalid pixels and a separately installed public C ABI."""
from contextlib import redirect_stdout
import io
import gzip
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from first_use_test import sample
from nuvo_depth import RecordingWriter, RecordingReader
from nuvo_depth.examples import main, pixel

ROOT=Path(__file__).resolve().parents[1]
BUILD=Path(sys.argv[1]) if len(sys.argv)>1 else None

class ExampleTest(unittest.TestCase):
    def test_recorder_cli_releases_its_session_on_completion_and_interrupt(self):
        from nuvo_depth import recorder
        for failure in (None,KeyboardInterrupt()):
            with tempfile.TemporaryDirectory() as temporary:
                client=mock.Mock()
                argv=['nuvo-record','https://fixture.invalid',str(Path(temporary)/'record.nvr'),'--claim-token','synthetic-test-token','--frames','1']
                with mock.patch.object(sys,'argv',argv),mock.patch.object(recorder,'NuvoClient',return_value=client),mock.patch.object(recorder,'record_frames',side_effect=failure,return_value=1):
                    recorder.main()
                client.close.assert_called_once()

    def test_previous_physical_recording_keeps_identity_authority_and_validity(self):
        directory=ROOT/'tests/fixtures/recordings'
        manifest=json.loads((directory/'beta6-preview.json').read_text())
        data=(directory/manifest['file']).read_bytes()
        self.assertEqual(hashlib.sha256(data).hexdigest(),manifest['sha256'])
        expanded=gzip.decompress(data)
        self.assertEqual(len(expanded),manifest['uncompressed_bytes'])
        self.assertEqual(hashlib.sha256(expanded).hexdigest(),manifest['uncompressed_sha256'])
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'old.nvr';path.write_bytes(expanded)
            with RecordingReader(path) as reader:frames=list(reader)
        self.assertEqual(len(frames),1);frame=frames[0]
        self.assertEqual(dict(frame.metadata),manifest['metadata'])
        self.assertEqual(frame.calibration_authority,0)
        self.assertEqual(sum(v==1 for v in frame.products['validity'].data),manifest['expected']['valid_pixels'])
        self.assertEqual((frame.products['depth'].width,frame.products['depth'].height),(728,544))

    def test_finite_shortfall_disconnect_and_interrupt_preserve_complete_records(self):
        for failure in (None,TimeoutError('fixture timeout'),ConnectionError('fixture disconnect'),KeyboardInterrupt()):
            with self.subTest(failure=type(failure).__name__), tempfile.TemporaryDirectory() as temporary:
                class Stream:
                    def __enter__(self):return self
                    def __exit__(self,*args):pass
                    def __iter__(self):
                        yield sample()
                        if failure:raise failure
                client=mock.Mock();client.frames.return_value=Stream()
                path=Path(temporary)/'partial.nvr'
                with mock.patch('nuvo_depth.examples.NuvoClient',return_value=client), mock.patch('getpass.getpass',return_value='fixture-only'), redirect_stdout(io.StringIO()):
                    with self.assertRaises(KeyboardInterrupt if isinstance(failure,KeyboardInterrupt) else SystemExit):
                        main(['record','--url','https://fixture.invalid:7443','--ca-file','fixture.pem','--frames','3','--output',str(path)])
                client.close.assert_called_once()
                with RecordingReader(path) as reader:self.assertEqual(len(list(reader)),1)

    def test_pixel_validity_png_and_replay_keep_wire_identity(self):
        frame=sample()
        self.assertEqual(pixel(frame,0,0),(1.25,1))
        self.assertEqual(pixel(frame,1,0),(None,0))
        with self.assertRaises(ValueError):pixel(frame,2,0)
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary); recording=root/'sample.nvr'
            with RecordingWriter(recording) as writer:writer.write(frame)
            for example in ('single','pixel','display','stream','record','replay'):
                output=root/(example+('.png' if example=='display' else '.nvr' if example=='record' else '.nuv'))
                arguments=[example,'--input',str(recording)]
                if example in ('single','display','record'):arguments+=['--output',str(output)]
                text=io.StringIO()
                with redirect_stdout(text):main(arguments)
                self.assertIn('SAVED REPLAY DATA',text.getvalue())
                if example=='single':self.assertEqual(output.read_bytes(),frame.to_bytes())
                if example=='display':
                    data=output.read_bytes();self.assertEqual(data[:8],b'\x89PNG\r\n\x1a\n')
                    self.assertEqual(struct.unpack('>II',data[16:24]),(2,1))
    @unittest.skipUnless(BUILD and shutil.which('cc') and shutil.which('pkg-config'),'Installed public C example requires a host build and C compiler/pkg-config')
    def test_c_example_uses_installed_headers_libraries_and_pkgconfig(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary); environment=dict(os.environ,DESTDIR=str(root/'installed'))
            subprocess.run(['cmake','--install',str(BUILD),'--component','public-sdk'],env=environment,check=True,capture_output=True)
            pc=next((root/'installed').rglob('nuvo-depth.pc'))
            environment.update(PKG_CONFIG_PATH=str(pc.parent),PKG_CONFIG_SYSROOT_DIR=str(root/'installed'))
            flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','nuvo-depth'],env=environment,text=True))
            self.assertTrue(all(str(ROOT) not in value for value in flags))
            source=root/'decode_frame.c';shutil.copyfile(ROOT/'sdk/c/decode_frame.c',source)
            binary=root/'decode_frame'
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(source),*flags,'-o',str(binary)],check=True,capture_output=True)
            packet=root/'sample.nuv';packet.write_bytes(sample().to_bytes())
            environment['LD_LIBRARY_PATH']=str(pc.parent.parent)
            output=subprocess.check_output([str(binary),str(packet)],env=environment,text=True)
            self.assertIn('ABI 1; kind 5; capture 1',output)
            self.assertIn('calibration_authority',output)
            packet.write_bytes(packet.read_bytes()[:-1])
            failed=subprocess.run([str(binary),str(packet)],env=environment,capture_output=True,text=True)
            self.assertNotEqual(failed.returncode,0)

if __name__=='__main__':unittest.main(argv=[sys.argv[0]])
