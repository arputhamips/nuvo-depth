"""Bounded explicit local control transport fixture; no native accuracy claim."""
import io
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tarfile
import tempfile
import threading
import time

FILES = ['manifest.json', 'front_left.yaml', 'front_right.yaml', 'validation.json']
VALUE = 'a'*64
RUN = '0123456789abcdef0123456789abcdef'


def read(connection, size):
    data = bytearray()
    while len(data) < size:
        block = connection.recv(size-len(data))
        if not block:
            raise RuntimeError('truncated client upload')
        data.extend(block)
    return data


def main():
    binary = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix='nuvo-calibration-sdk-') as temporary:
        root = Path(temporary)
        bundle = root/'bundle'
        bundle.mkdir()
        for name in FILES:
            (bundle/name).write_bytes(name.encode())
        def run(path=bundle, *, response=None, raw=None, stall=False, timeout=3000, ok=True):
            endpoint = root/'control.sock'
            errors = []
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
                listener.bind(str(endpoint))
                listener.listen(1)
                listener.settimeout(2)
                def serve():
                    try:
                        with listener.accept()[0] as connection:
                            connection.settimeout(2)
                            if os.geteuid() != 0:
                                # The production client authenticates root before
                                # uploading any bytes; unprivileged CI tests this
                                # denial and input checks, never relaxes it.
                                assert connection.recv(1) == b''
                                return
                            header = read(connection, 12)
                            assert header[:4] == b'NVC1'
                            size = struct.unpack_from('>Q', header, 4)[0]
                            assert 1024 <= size <= 4*4*1024*1024+65536
                            payload = read(connection, size)
                            with tarfile.open(fileobj=io.BytesIO(payload), mode='r:') as source:
                                assert source.getnames() == FILES
                                for member in source:
                                    assert member.isfile()
                                    assert source.extractfile(member).read() == member.name.encode()
                            if stall:
                                time.sleep(.3)
                                return
                            data = (json.dumps(response).encode() if response is not None else
                                    json.dumps({'schema_version':1,'ok':True,'code':'activated',
                                     'calibration_sha256':VALUE,'calibration_authority':1,
                                     'run_uuid':RUN,'unchanged':False}).encode())
                            connection.sendall(raw if raw is not None else b'NCA1'+struct.pack('>I', len(data))+data)
                    except BaseException as error:
                        errors.append(error)
                worker = threading.Thread(target=serve)
                worker.start()
                result = subprocess.run([binary, 'calibration', str(path), str(endpoint), str(timeout)],
                                        capture_output=True, text=True, timeout=5)
                worker.join(3)
                assert not worker.is_alive()
                endpoint.unlink()
                if errors:
                    raise errors[0]
                assert (result.returncode == 0) == ok, result.stdout+result.stderr
                return result
        if os.geteuid() == 0:
            assert run().stdout.split() == [VALUE, '1', RUN, '0']
            packed = root/'bundle.tar'
            with tarfile.open(packed, 'w', format=tarfile.USTAR_FORMAT) as output:
                for name in FILES:
                    output.add(bundle/name, arcname=name)
            assert run(packed).stdout.split() == [VALUE, '1', RUN, '0']
            unchanged = {'schema_version':1,'ok':True,'code':'unchanged',
                         'calibration_sha256':VALUE,'calibration_authority':2,
                         'run_uuid':RUN,'unchanged':True}
            assert run(response=unchanged).stdout.split() == [VALUE, '2', RUN, '1']
            denied = run(response={'schema_version':1,'ok':False,'code':'invalid_bundle',
                                   'message':'Calibration belongs to another device'}, ok=False)
            assert 'invalid_bundle' in denied.stderr and 'another device' in denied.stderr
            for changes in [{'calibration_sha256':'bad'}, {'calibration_authority':0},
                            {'calibration_authority':1.2}, {'calibration_authority':4294967297},
                            {'run_uuid':'-'*32},
                            {'unchanged':False}, {'schema_version':2}]:
                run(response={**unchanged, **changes}, ok=False)
            for raw in [b'BAD!'+struct.pack('>I', 1)+b'{}',
                        b'NCA1'+struct.pack('>I', 65537), b'NCA1'+struct.pack('>I', 100)+b'{}']:
                run(raw=raw, ok=False)
            result = run(stall=True, timeout=50, ok=False)
            assert 'timed out' in result.stderr and 'may finish' in result.stderr
        else:
            result = run(ok=False)
            assert 'owned by root' in result.stderr
            print('Success-reply fixtures require root peer credentials; unprivileged peer rejection verified')
        # Fail before opening a socket for unsupported, missing or linked inputs.
        for path in [root/'single.yaml', root/'missing', root/'linked']:
            if path.name == 'single.yaml':
                path.write_text('camera_matrix: {}')
            elif path.name == 'linked':
                path.symlink_to(bundle, target_is_directory=True)
            result = subprocess.run([binary, 'calibration', str(path), str(root/'absent.sock')],
                                    capture_output=True, text=True, timeout=3)
            assert result.returncode != 0
        oversized = bundle/'front_left.yaml'
        with oversized.open('wb') as output:
            output.truncate(4*1024*1024+1)
        result = subprocess.run([binary, 'calibration', str(bundle), str(root/'absent.sock')],
                                capture_output=True, text=True, timeout=3)
        assert result.returncode != 0 and 'bounded regular files' in result.stderr
    print('Calibration control transport checks passed')


if __name__ == '__main__':
    main()
