"""Group-restricted local bundle activation; no caller paths or shell commands.

NVC1 + uint64_be archive length + uncompressed tar. NCA1 + uint32_be JSON
length + JSON reply. Only the four canonical regular bundle assets are accepted.
The privileged native controller owns validation, transaction and rollback.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import socket
import signal
import stat
import struct
import subprocess
import tarfile
import tempfile
import time

from nuvo_calibration_control import import_bundle
from nuvo_calibration_store import REQUIRED_ASSETS

FILES = ('manifest.json', *REQUIRED_ASSETS)
MAX_FILE_BYTES = 4 * 1024 * 1024
MAX_UPLOAD_BYTES = len(FILES) * MAX_FILE_BYTES + 65536
MAX_REPLY_BYTES = 65536
UPLOAD_SECONDS = 10


class InvalidRequest(ValueError):
    pass


def receive(connection, size, deadline):
    chunks = bytearray()
    while len(chunks) < size:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise InvalidRequest('Bundle upload timed out; retry the complete bundle')
        connection.settimeout(remaining)
        try:
            chunk = connection.recv(min(size-len(chunks), 65536))
        except TimeoutError as error:
            raise InvalidRequest('Bundle upload timed out; retry the complete bundle') from error
        if not chunk:
            raise InvalidRequest('Truncated bundle upload; retry the complete bundle')
        chunks.extend(chunk)
    return chunks


def unpack(archive, destination):
    """Never honor archive paths, links, owners, permissions or sparse maps."""
    seen = set()
    try:
        # Parse individual standard headers, rather than tarfile.open's
        # recursive PAX/GNU extension handlers. The tiny fixed contract needs
        # no extended names/metadata, and rejects those before allocating them.
        with archive.open('rb') as source:
            while True:
                header = source.read(512)
                if len(header) != 512:
                    raise InvalidRequest('Truncated calibration tar header or end marker')
                if header == bytes(512):
                    if source.read(512) != bytes(512):
                        raise InvalidRequest('Calibration tar needs two zero end blocks')
                    while True:
                        trailer = source.read(65536)
                        if not trailer:
                            break
                        if any(trailer):
                            raise InvalidRequest('Extra data after calibration tar end marker')
                    break
                member = tarfile.TarInfo.frombuf(header, encoding='utf-8', errors='strict')
                if (member.name not in FILES or member.name in seen or
                        member.type not in (tarfile.REGTYPE, tarfile.AREGTYPE) or
                        member.sparse is not None or not 0 <= member.size <= MAX_FILE_BYTES):
                    raise InvalidRequest('Tar must contain each canonical bundle file once at its root; links and extra entries are forbidden')
                seen.add(member.name)
                data = source.read(member.size)
                if len(data) != member.size:
                    raise InvalidRequest('Truncated or oversized calibration asset')
                padding = (-member.size) % 512
                if len(source.read(padding)) != padding:
                    raise InvalidRequest('Truncated calibration tar padding')
                (destination / member.name).write_bytes(data)
    except (tarfile.TarError, EOFError, UnicodeError) as error:
        raise InvalidRequest('Invalid uncompressed calibration tar; use the complete accepted bundle, not a single-camera ROS YAML') from error
    if seen != set(FILES):
        raise InvalidRequest('Calibration tar needs manifest.json, front_left.yaml, front_right.yaml and validation.json at its root')


def handle(connection, root=Path('/'), run=subprocess.run):
    """Complete a received authorized transaction even if its client disconnects."""
    try:
        deadline = time.monotonic() + UPLOAD_SECONDS
        header = receive(connection, 12, deadline)
        if header[:4] != b'NVC1':
            raise InvalidRequest('Unsupported calibration control protocol; update the native service and SDK together')
        size = struct.unpack_from('>Q', header, 4)[0]
        if not 1024 <= size <= MAX_UPLOAD_BYTES:
            raise InvalidRequest('Calibration upload exceeds bounds or is not an uncompressed bundle tar')
        with tempfile.TemporaryDirectory(prefix='nuvo-calibration-control-') as temporary:
            staging = Path(temporary)
            archive = staging / 'bundle.tar'
            # Stream to private disk with a complete-upload deadline, rather
            # than keeping an untrusted archive and all assets in memory.
            with archive.open('wb') as target:
                remaining = size
                while remaining:
                    block = receive(connection, min(remaining, 65536), deadline)
                    target.write(block)
                    remaining -= len(block)
            bundle = staging / 'bundle'
            bundle.mkdir()
            unpack(archive, bundle)
            result = import_bundle(root, bundle, run=run, idempotent=True, lock_timeout=2)
        runtime = result['verified_runtime']
        expected_authority = 2 if result['authority'] == 'factory_validated' else 1
        if (runtime.get('status') != 'running' or not runtime.get('run_uuid') or
                runtime.get('calibration_sha256') != result['bundle_id'] or
                runtime.get('calibration_authority') != expected_authority):
            raise RuntimeError('Native activation verification is unavailable; inspect nuvoctl calibration status before retrying')
        response = {'schema_version':1, 'ok':True,
                    'code':'unchanged' if result['unchanged'] else 'activated',
                    'calibration_sha256':result['bundle_id'],
                    'calibration_authority':expected_authority,
                    'run_uuid':runtime['run_uuid'], 'unchanged':result['unchanged']}
    except InvalidRequest as error:
        response = {'schema_version':1, 'ok':False, 'code':'invalid_request', 'message':str(error)}
    except TimeoutError as error:
        response = {'schema_version':1, 'ok':False, 'code':'busy', 'message':str(error)}
    except ValueError as error:
        response = {'schema_version':1, 'ok':False, 'code':'invalid_bundle', 'message':str(error)}
    except (RuntimeError, subprocess.SubprocessError) as error:
        response = {'schema_version':1, 'ok':False, 'code':'activation_failed', 'message':str(error)}
    except OSError:
        response = {'schema_version':1, 'ok':False, 'code':'internal_error',
                    'message':'Native calibration storage or service failed; inspect the host journal and nuvoctl calibration status before retrying'}
    if 'message' in response:
        response['message'] = response['message'][:2048]
    data = json.dumps(response, separators=(',', ':')).encode()
    assert len(data) <= MAX_REPLY_BYTES
    try:
        connection.settimeout(2)
        connection.sendall(b'NCA1' + struct.pack('>I', len(data)) + data)
    except OSError:
        pass  # The durable host transaction is independent of client lifetime.
    return response


def activated_listener():
    if os.geteuid() != 0:
        raise RuntimeError('The calibration broker must run as the native privileged systemd service')
    if os.environ.get('LISTEN_PID') != str(os.getpid()) or os.environ.get('LISTEN_FDS') != '1':
        raise RuntimeError('Start nuvo-depth-calibration-control.socket; no caller-supplied socket or root path is accepted')
    listener = socket.socket(fileno=3)
    if listener.family != socket.AF_UNIX or listener.type != socket.SOCK_STREAM:
        raise RuntimeError('Calibration control requires one local UNIX stream listener')
    # systemd owns this root:nuvo-video endpoint. Fail closed if unit permissions
    # drift; a read-only runtime bind mount still permits authorized connects.
    path = Path(listener.getsockname())
    info = path.lstat()
    import grp
    if (not stat.S_ISSOCK(info.st_mode) or info.st_uid != 0 or
            info.st_gid != grp.getgrnam('nuvo-video').gr_gid or
            stat.S_IMODE(info.st_mode) != 0o660):
        raise RuntimeError('Calibration control socket must be root:nuvo-video with mode 0660')
    return listener


def main():
    argparse.ArgumentParser(description=__doc__).parse_args()
    stopping = False
    def request_stop(_signum, _frame):
        nonlocal stopping
        stopping = True
    signal.signal(signal.SIGTERM, request_stop)
    signal.signal(signal.SIGINT, request_stop)
    with activated_listener() as listener:
        # One bounded upload/transaction at a time, plus four kernel backlog
        # slots (set by the socket unit). No subprocess per untrusted peer.
        listener.settimeout(1)
        while not stopping:
            try:
                connection, _ = listener.accept()
            except TimeoutError:
                continue
            with connection:
                handle(connection)


if __name__ == '__main__':
    main()
