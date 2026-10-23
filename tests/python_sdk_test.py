import base64
import errno
import hashlib
import http.cookiejar
import json
from pathlib import Path
import re
import struct
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).parents[1] / "sdk/python"))
from nuvo_depth import NuvoClient, RecordingReader, RecordingWriter, decode_encoded_frame, decode_frame
from nuvo_depth import client as client_module
from nuvo_depth.recorder import record_frames


def sample(*, run_uuid="11" * 16, sequence=3, data_format="gray8"):
    payload = b"\x01\x02" if data_format == "gray8" else b"\x01\x02\x03\x04\x05\x06"
    metadata = json.dumps({
        "schema_version": 1, "capture_epoch": 2, "capture_sequence": sequence,
        "device_uuid": "22" * 16, "run_uuid": run_uuid,
        "capture_time_ns": 4, "configuration_epoch": 5,
        "calibration_sha256": "00" * 32, "calibration_authority": 0,
        "products": [{"kind":"left_rectified", "format":data_format,
                      "offset":0, "length":len(payload), "width":2, "height":1}],
    }, separators=(",", ":")).encode()
    return struct.pack(">4sHHHHIQQ", b"NUV1", 1, 0, 5, 0,
                       len(metadata), len(payload), 1) + metadata + payload


def websocket_frame(payload=b"", opcode=2, final=True, declared_size=None):
    length = len(payload) if declared_size is None else declared_size
    if length < 126:
        header = bytes((int(final) * 0x80 | opcode, length))
    elif length < 65536:
        header = bytes((int(final) * 0x80 | opcode, 126)) + struct.pack(">H", length)
    else:
        header = bytes((int(final) * 0x80 | opcode, 127)) + struct.pack(">Q", length)
    return header + payload


def sent_control(payload):
    """Decode the small, masked client control frames captured by the mock."""
    first, size = payload[:2]
    assert size & 0x80
    mask = payload[2:6]
    return first & 15, bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload[6:]))


class MockSocket:
    def __init__(self, frames, *, chunk_size=4096, status="101 Switching Protocols",
                 accept_valid=True, failure=None, qualification=None):
        self.frames = frames
        self.chunk_size = chunk_size
        self.status = status
        self.accept_valid = accept_valid
        self.failure = failure
        self.qualification = qualification
        self.incoming = bytearray()
        self.sent = []
        self.closed = False
        self.read_sizes = []
        self.timeouts = []

    def settimeout(self, timeout):
        self.timeouts.append(timeout)

    def sendall(self, data):
        self.sent.append(data)
        if data.startswith(b"GET "):
            key = re.search(rb"Sec-WebSocket-Key: ([^\r]+)", data).group(1)
            accept = base64.b64encode(hashlib.sha1(key + client_module._WEBSOCKET_GUID).digest())
            if not self.accept_valid:
                accept = b"wrong-server"
            self.incoming.extend(
                f"HTTP/1.1 {self.status}\r\n".encode()
                + b"Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "
                + accept + b"\r\n"
                + (f"X-Nuvo-Stream-Qualification: {self.qualification}\r\n".encode()
                   if self.qualification is not None else b"")
                + b"\r\n" + self.frames
            )

    def recv(self, size):
        self.read_sizes.append(size)
        if not self.incoming and self.failure is not None:
            raise self.failure
        size = min(size, self.chunk_size, len(self.incoming))
        data = bytes(self.incoming[:size])
        del self.incoming[:size]
        return data

    def recv_into(self, view):
        data = self.recv(len(view))
        view[:len(data)] = data
        return len(data)

    def close(self):
        self.closed = True


class SdkTest(unittest.TestCase):
    def test_client_close_revokes_once_and_clears_authorization(self):
        client=NuvoClient('https://nuvo.test');client._csrf='a'*64
        client._configuration_receipts['/api/v1/stereo']={'etag':'old'}
        with patch.object(client,'_request',return_value=b'{"closed":true}') as request:
            self.assertTrue(client.close());self.assertTrue(client.close())
        request.assert_called_once_with('/api/v1/session',method='DELETE',
            headers={'Origin':'https://nuvo.test','X-Nuvo-CSRF':'a'*64},maximum=4096)
        self.assertFalse(client._csrf);self.assertFalse(list(client._cookies));self.assertEqual(client._configuration_receipts,{})

    def test_legacy_or_unreachable_logout_does_not_mask_application_failure(self):
        for error in (client_module.urllib.error.HTTPError('https://nuvo.test/api/v1/session',404,'old gateway',{},None),
                      client_module.urllib.error.URLError('fixture offline')):
            client=NuvoClient('https://nuvo.test');client._csrf='a'*64
            with patch.object(client,'_request',side_effect=error):
                self.assertFalse(client.close())
                self.assertFalse(client.close())
                client._csrf='b'*64
                with self.assertRaisesRegex(RuntimeError,'application failure'):
                    with client:raise RuntimeError('application failure')
            self.assertFalse(client._csrf)
        self.assertIn('session',client_module.recovery_action(503,'/api/v1/session'))
        self.assertNotIn('Raw output',client_module.recovery_action(503,'/api/v1/session'))

    def test_full_filesystem_preserves_completed_recording_frames(self):
        class FailingPayload:
            def __init__(self,output):self.output=output
            def write(self,data):
                if len(data)>8:
                    self.output.write(data[:len(data)//2]);self.output.flush()
                    raise OSError(errno.ENOSPC,"injected full filesystem")
                return self.output.write(data)
            def flush(self):self.output.flush()
            def close(self):self.output.close()
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/"partial.nvr";first=decode_frame(sample(sequence=3));second=decode_frame(sample(sequence=4))
            with RecordingWriter(path) as writer:
                writer.write(first);writer._file=FailingPayload(writer._file)
                with self.assertRaises(OSError) as failure:writer.write(second)
                self.assertEqual(failure.exception.errno,errno.ENOSPC)
            with RecordingReader(path) as reader:
                frames=iter(reader);self.assertEqual(next(frames).to_bytes(),first.to_bytes())
                with self.assertRaisesRegex(ValueError,"truncated recording frame"):next(frames)

    def mock_client(self, sock, *, timeout=5):
        context = Mock()
        context.wrap_socket.return_value = sock
        with patch.object(client_module.ssl, "create_default_context", return_value=context):
            client = NuvoClient("https://nuvo.test:17443", timeout=timeout)
        client._csrf = "csrf-token"
        client._cookies.set_cookie(http.cookiejar.Cookie(
            version=0, name="nuvo_session", value="session-token", port=None,
            port_specified=False, domain="nuvo.test", domain_specified=False,
            domain_initial_dot=False, path="/", path_specified=True, secure=True,
            expires=None, discard=True, comment=None, comment_url=None,
            rest={}, rfc2109=False,
        ))
        return client, context

    def test_decode_and_recording_roundtrip(self):
        frame = decode_frame(sample())
        self.assertEqual(frame.capture_sequence, 3)
        self.assertEqual(frame.identity, ("22" * 16, "11" * 16, 2, 3))
        self.assertEqual(bytes(frame.products["left_rectified"].data), b"\x01\x02")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.nvr"
            with RecordingWriter(path) as writer: writer.write(frame)
            with RecordingReader(path) as reader:
                frames = list(reader)
            self.assertEqual(len(frames), 1)
            self.assertEqual(frames[0].capture_sequence, 3)

    def test_rejects_overlap_and_truncation(self):
        self.assertRaises(ValueError, decode_frame, sample()[:-1])
        data = bytearray(sample()); data[0] = 0
        self.assertRaises(ValueError, decode_frame, bytes(data))

    def test_decodes_bounded_encoded_envelope(self):
        payload=b"\xff\xd8jpeg\xff\xd9"
        metadata=json.dumps({"schema_version":1,"capture_epoch":2,"capture_sequence":3,
            "capture_time_ns":4,"configuration_epoch":5,"source":"left_rectified",
            "codec":"jpeg","mime_type":"image/jpeg","codec_string":"jpeg",
            "width":2,"height":2,"keyframe":True,"encode_ms":1.5},separators=(",",":")).encode()
        wire=struct.pack(">4sHHIQQI",b"NVE1",1,1,len(metadata),len(payload),7,0)+metadata+payload
        frame=decode_encoded_frame(wire)
        self.assertEqual(frame.stream_sequence,7)
        self.assertEqual(frame.codec,"jpeg")
        self.assertEqual(bytes(frame.data),payload)
        self.assertIs(frame.data.obj,wire)
        self.assertTrue(frame.data.readonly)
        for mutable in (bytearray(wire),memoryview(bytearray(wire))):
            frozen=decode_encoded_frame(mutable)
            mutable[-1]^=255
            self.assertEqual(bytes(frozen.data),payload)
            self.assertTrue(frozen.data.readonly)
        self.assertRaises(ValueError,decode_encoded_frame,wire[:-1])
        document=json.loads(metadata)
        for changed in ({"width":0},{"height":1081},{"codec":"unknown"},{"keyframe":"true"}):
            invalid=json.dumps({**document,**changed}).encode()
            packet=struct.pack(">4sHHIQQI",b"NVE1",1,1,len(invalid),len(payload),7,0)+invalid+payload
            self.assertRaises(ValueError,decode_encoded_frame,packet)

    def test_color_products_preserve_bytes_and_validate_channel_sizes(self):
        for data_format in ("bgr8", "rgb8"):
            wire = sample(data_format=data_format)
            frame = decode_frame(wire)
            product = frame.products["left_rectified"]
            self.assertEqual(product.format, data_format)
            self.assertEqual(bytes(product.data), b"\x01\x02\x03\x04\x05\x06")
            self.assertRaises(TypeError, product.float32_le)
            # Same payload length, wrong grid: three channels still count.
            malformed = wire.replace(b'"width":2', b'"width":3')
            self.assertRaises(ValueError, decode_frame, malformed)

    def test_all_output_kinds_have_consistent_formats_and_lengths(self):
        for name, formats in client_module.PRODUCT_FORMATS.items():
            data_format = next(iter(formats))
            channels = 4 if data_format == "float32_le" else 3 if data_format in {"rgb8", "bgr8"} else 1
            width = 4 if name.startswith("rectified_stereo") else 2
            payload = bytes(width * channels)
            metadata = {
                "schema_version": 1, "capture_epoch": 1, "capture_sequence": 2,
                "capture_time_ns": 3, "configuration_epoch": 4,
                "calibration_sha256": "00" * 32, "calibration_authority": 1,
                "products": [{"kind": name, "format": data_format, "width": width,
                              "height": 1, "offset": 0, "length": len(payload)}],
            }
            def encode():
                document = json.dumps(metadata, separators=(",", ":")).encode()
                return struct.pack(">4sHHHHIQQ", b"NUV1", 1, 0, 5, 0,
                                   len(document), len(payload), 1) + document + payload
            decoded = decode_frame(encode())
            self.assertEqual(tuple(decoded.products), (name,))
            self.assertEqual(decoded.products[name].width, width)
            metadata["products"][0]["format"] = "float32_le" if data_format != "float32_le" else "gray8"
            self.assertRaises(ValueError, decode_frame, encode())

    def test_best_effort_delivery_and_new_product_stream_selection(self):
        sock = MockSocket(websocket_frame(sample()), qualification="best-effort")
        client, _ = self.mock_client(sock)
        with patch.object(client_module.socket, "create_connection", return_value=sock):
            with client.frames(cadence_hz=10, products=("left_rgb", "rectified_stereo_rgb"),
                               delivery="best_effort") as stream:
                self.assertEqual(stream.qualification, "best-effort")
                self.assertEqual(next(stream).capture_sequence, 3)
        self.assertIn(b"delivery=best_effort", sock.sent[0])
        self.assertIn(b"products=left_rgb,rectified_stereo_rgb", sock.sent[0])
        self.assertRaises(ValueError, client.frames, delivery="reliable-ish")

    def test_declared_selection_masks_match_the_product_descriptors(self):
        original = sample()
        size = struct.unpack_from(">I", original, 12)[0]
        metadata = json.loads(original[32:32 + size])
        payload = original[32 + size:]
        def encode():
            document = json.dumps(metadata, separators=(",", ":")).encode()
            return struct.pack(">4sHHHHIQQ", b"NUV1", 1, 0, 5, 0,
                               len(document), len(payload), 1) + document + payload
        metadata.update(available_products=16383, selected_products=1)
        self.assertEqual(tuple(decode_frame(encode()).products), ("left_rectified",))
        metadata["selected_products"] = 2
        self.assertRaises(ValueError, decode_frame, encode())
        metadata["selected_products"] = 1
        metadata["available_products"] = 2
        self.assertRaises(ValueError, decode_frame, encode())

    def test_latest_and_stream_share_selection_and_defaults(self):
        client, _ = self.mock_client(MockSocket(b""))
        with patch.object(client, "_request", return_value=sample()) as request:
            self.assertEqual(client.latest(products=("depth", "validity"), cadence_hz=2,
                                           delivery="best_effort").capture_sequence, 3)
            path = request.call_args.args[0]
            self.assertEqual(path, "/api/v1/frame/latest?delivery=best_effort&cadence_hz=2&products=depth,validity")
            client.latest(products=("depth",))
            self.assertIn("cadence_hz=10&products=depth", request.call_args.args[0])
            client.latest(cadence_hz=1)
            self.assertIn("cadence_hz=1&products=left_rectified,right_rectified,disparity,depth,confidence,validity", request.call_args.args[0])
            for options in ({"products": ("unknown",)}, {"cadence_hz": True}, {"cadence_hz": "2"}):
                self.assertRaises(ValueError, client.latest, **options)
                self.assertRaises(ValueError, client.frames, **options)

    def test_decoded_products_keep_immutable_storage(self):
        original = sample()
        frame = decode_frame(original)
        self.assertIs(frame.to_bytes(), original)
        self.assertTrue(frame.products["left_rectified"].data.readonly)
        writable = bytearray(original)
        frozen = decode_frame(writable)
        writable[-1] ^= 0xFF
        self.assertEqual(frozen.to_bytes(), original)
        self.assertTrue(frozen.products["left_rectified"].data.readonly)

    def test_decoder_bounds_before_freezing_writable_input(self):
        class NoCopy(bytearray):
            def __bytes__(self):
                raise AssertionError("oversize input must not be copied")
        with patch("nuvo_depth.frame.MAX_METADATA", 1), patch("nuvo_depth.frame.MAX_PAYLOAD", 1):
            for data in (NoCopy(35), memoryview(bytearray(35))):
                self.assertRaises(ValueError, decode_frame, data)
        for data in (10**12, True, "NUV1", [1, 2, 3]):
            self.assertRaises(TypeError, decode_frame, data)
            self.assertRaises(TypeError, decode_encoded_frame, data)
        with patch("nuvo_depth.encoded.MAX_ENCODED_METADATA", 1), patch("nuvo_depth.encoded.MAX_ENCODED_PAYLOAD", 1):
            for data in (NoCopy(35), memoryview(bytearray(35))):
                self.assertRaises(ValueError, decode_encoded_frame, data)

    def test_product_dimensions_reject_implicit_numeric_conversion(self):
        original=sample()
        header=list(struct.unpack_from(">4sHHHHIQQ",original))
        end=32+header[5]
        metadata=json.loads(original[32:end])
        for value in (True, "2", 2.0):
            metadata["products"][0]["width"]=value
            document=json.dumps(metadata).encode()
            header[5]=len(document)
            packet=struct.pack(">4sHHHHIQQ",*header)+document+original[end:]
            self.assertRaises(ValueError,decode_frame,packet)

    def test_stream_uses_claimed_cookie_verified_tls_and_one_connection(self):
        close = websocket_frame(struct.pack(">H", 1000), opcode=8)
        sock = MockSocket(websocket_frame(sample()) + websocket_frame(sample(sequence=4)) + close,
                          chunk_size=7)
        client, context = self.mock_client(sock)
        with patch.object(client_module.socket, "create_connection", return_value=sock) as connect:
            with client.frames(cadence_hz=10, products=("left_rectified", "depth")) as stream:
                frames = list(stream)
        self.assertEqual([frame.capture_sequence for frame in frames], [3, 4])
        connect.assert_called_once()
        context.wrap_socket.assert_called_once_with(sock, server_hostname="nuvo.test")
        self.assertIn(b"GET /api/v1/stream?cadence_hz=10&products=left_rectified,depth HTTP/1.1", sock.sent[0])
        self.assertIn(b"Cookie: nuvo_session=session-token\r\n", sock.sent[0])
        self.assertEqual(sent_control(sock.sent[-1]), (8, struct.pack(">H", 1000)))
        self.assertTrue(sock.closed)

    def test_fragmented_stream_handles_ping_and_explicit_ping_and_close(self):
        wire = sample()
        sock = MockSocket(websocket_frame(wire[:90], final=False)
                          + websocket_frame(b"heartbeat", opcode=9)
                          + websocket_frame(b"check", opcode=10)
                          + websocket_frame(wire[90:], opcode=0))
        client, _ = self.mock_client(sock)
        with patch.object(client_module.socket, "create_connection", return_value=sock):
            with client.frames() as stream:
                stream.ping(b"check")
                self.assertEqual(next(stream).capture_sequence, 3)
                stream.close(reason="done")
        self.assertEqual(sent_control(sock.sent[1]), (9, b"check"))
        self.assertEqual(sent_control(sock.sent[2]), (10, b"heartbeat"))
        self.assertEqual(sent_control(sock.sent[3]), (8, struct.pack(">H", 1000) + b"done"))
        self.assertTrue(sock.closed)

    def test_stream_rejects_oversize_before_payload_allocation(self):
        sock = MockSocket(websocket_frame(declared_size=client_module._MAX_FRAME + 1))
        client, _ = self.mock_client(sock)
        with patch.object(client_module.socket, "create_connection", return_value=sock):
            with self.assertRaisesRegex(ValueError, "exceeds the client bound"):
                with client.frames() as stream:
                    next(stream)
        self.assertLessEqual(max(sock.read_sizes), 4096)
        self.assertTrue(sock.closed)

    def test_stream_bounds_fragmented_aggregate(self):
        sock = MockSocket(websocket_frame(b"x" * 70, final=False)
                          + websocket_frame(b"y" * 70, opcode=0))
        client, _ = self.mock_client(sock)
        with patch.object(client_module.socket, "create_connection", return_value=sock), \
                patch.object(client_module, "_MAX_FRAME", 100):
            with self.assertRaisesRegex(ValueError, "exceeds the client bound"):
                with client.frames() as stream:
                    next(stream)
        self.assertTrue(sock.closed)

    def test_stream_timeout_is_a_complete_message_deadline(self):
        sock = MockSocket(websocket_frame(sample()))
        client, _ = self.mock_client(sock, timeout=0.5)
        with patch.object(client_module.socket, "create_connection", return_value=sock):
            with client.frames() as stream:
                # A message already buffered during upgrade still has a fixed
                # deadline. Small pieces cannot each renew the timeout.
                with patch.object(client_module.time, "monotonic", side_effect=[0, 0.3, 0.6]):
                    self.assertRaisesRegex(TimeoutError, "timed out", next, stream)
        self.assertTrue(sock.closed)

    def test_stream_closes_socket_after_certificate_failure(self):
        sock = MockSocket(b"")
        client, context = self.mock_client(sock)
        context.wrap_socket.side_effect = client_module.ssl.SSLError("certificate rejected")
        with patch.object(client_module.socket, "create_connection", return_value=sock):
            with self.assertRaises(client_module.ssl.SSLError):
                with client.frames():
                    pass
        self.assertTrue(sock.closed)

    def test_stream_bounds_upgrade_headers(self):
        sock = MockSocket(b"")
        client, _ = self.mock_client(sock)

        def oversized_upgrade(data):
            sock.incoming.extend(b"HTTP/1.1 101 Switching Protocols\r\nX-Huge: "
                                 + b"x" * client_module._MAX_UPGRADE_HEADERS)

        with patch.object(client_module.socket, "create_connection", return_value=sock), \
                patch.object(sock, "sendall", side_effect=oversized_upgrade):
            with self.assertRaisesRegex(ValueError, "headers exceed"):
                with client.frames():
                    pass
        self.assertTrue(sock.closed)

    def test_stream_disconnect_timeout_and_error_close_are_explicit(self):
        cases = [
            (websocket_frame(sample()[:20], declared_size=len(sample())), None, ConnectionError),
            (b"", TimeoutError("idle"), TimeoutError),
            (websocket_frame(struct.pack(">H", 1013) + b"overloaded", opcode=8), None, ConnectionError),
        ]
        for incoming, failure, error in cases:
            with self.subTest(error=error, incoming=incoming):
                sock = MockSocket(incoming, failure=failure)
                client, _ = self.mock_client(sock)
                with patch.object(client_module.socket, "create_connection", return_value=sock):
                    with self.assertRaises(error):
                        with client.frames() as stream:
                            next(stream)
                self.assertTrue(sock.closed)

    def test_stream_rejects_invalid_server_frames_and_upgrade(self):
        for incoming in (b"\x82\x80", websocket_frame(b"text", opcode=1),
                         websocket_frame(b"ping", opcode=9, final=False),
                         websocket_frame(b"part", opcode=0)):
            sock = MockSocket(incoming)
            client, _ = self.mock_client(sock)
            with patch.object(client_module.socket, "create_connection", return_value=sock):
                with self.assertRaises(ValueError):
                    with client.frames() as stream:
                        next(stream)
            self.assertTrue(sock.closed)
        for status, accept_valid, expected in (("401 Unauthorized", True, ConnectionError),
                                                ("101 Switching Protocols", False, ValueError)):
            sock = MockSocket(b"", status=status, accept_valid=accept_valid)
            client, _ = self.mock_client(sock)
            with patch.object(client_module.socket, "create_connection", return_value=sock):
                with self.assertRaises(expected):
                    with client.frames():
                        pass
            self.assertTrue(sock.closed)

    def test_stream_requires_claim_and_validates_subscription_before_connection(self):
        sock = MockSocket(b"")
        client, _ = self.mock_client(sock)
        for options in ({"cadence_hz": float("nan")}, {"cadence_hz": 0},
                        {"products": ()}, {"products": "depth"},
                        {"products": ("depth", "depth")}, {"products": ("unknown",)}):
            self.assertRaises(ValueError, client.frames, **options)
        client._csrf = ""
        with patch.object(client_module.socket, "create_connection") as connect:
            with self.assertRaisesRegex(RuntimeError, "claim"):
                with client.frames():
                    pass
            connect.assert_not_called()

    def test_recorder_uses_stream_identity_across_restart_and_closes_at_limit(self):
        wire = (websocket_frame(sample()) + websocket_frame(sample())
                + websocket_frame(sample(run_uuid="33" * 16)) + websocket_frame(sample(sequence=4)))
        sock = MockSocket(wire)
        client, _ = self.mock_client(sock)
        client.latest = Mock(side_effect=AssertionError("recorder must use the stream"))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "stream.nvr"
            with patch.object(client_module.socket, "create_connection", return_value=sock), \
                    RecordingWriter(path) as writer:
                self.assertEqual(record_frames(client, writer, frames=2, cadence_hz=7.5), 2)
            with RecordingReader(path) as reader:
                frames = list(reader)
        self.assertEqual([frame.run_uuid for frame in frames], ["11" * 16, "33" * 16])
        self.assertIn(b"cadence_hz=7.5", sock.sent[0])
        self.assertTrue(sock.closed)

    def test_recorder_reports_early_normal_close_when_count_was_requested(self):
        sock = MockSocket(websocket_frame(sample()) + websocket_frame(struct.pack(">H", 1000), opcode=8))
        client, _ = self.mock_client(sock)
        with patch.object(client_module.socket, "create_connection", return_value=sock):
            with self.assertRaisesRegex(ConnectionError, "1 of 2"):
                record_frames(client, Mock(), frames=2)

    def test_configuration_revision_receipts_and_stale_updates(self):
        class Response:
            def __init__(self,state,etag):
                self.headers={"ETag":etag,"X-Nuvo-Configuration-State":state}
            def __enter__(self): return self
            def __exit__(self,*args): pass
            def read(self,size): return b'{"schema_version":1,"algorithm":"sgbm"}'
        client=NuvoClient("https://nuvo.test")
        client._csrf="csrf"
        old='"'+'a'*64+'"'; new='"'+'b'*64+'"'
        client._opener=Mock()
        client._opener.open.side_effect=[Response("requested",old),Response("pending",new)]
        with self.assertRaisesRegex(RuntimeError,"read the configuration"):
            client.configure_stereo({})
        requested=client.stereo()
        self.assertEqual(client.configuration_receipt("stereo")["etag"],old)
        client.configure_stereo(requested)
        posted=client._opener.open.call_args.args[0]
        self.assertEqual(posted.get_header("If-match"),old)
        self.assertEqual(client.configuration_receipt("stereo"),{
            "etag":new,"requested_revision":"b"*64,"state":"pending"})
        error=client_module.urllib.error.HTTPError("https://nuvo.test",412,"stale",{},None)
        client._opener.open.side_effect=error
        with self.assertRaises(client_module.urllib.error.HTTPError):
            client.configure_stereo(requested)
        self.assertEqual(client.configuration_receipt("stereo")["etag"],new)

    def test_configuration_helpers_use_claimed_csrf_session(self):
        class FakeClient(NuvoClient):
            def __init__(self):
                self._csrf = "csrf-token"
                self.requests = []
                self._configuration_receipts = {"/api/v1/"+kind:{"etag":'"'+"a"*64+'"'} for kind in ("encoding","stereo","exposure")}

            def _request(self, path, **kwargs):
                self.requests.append((path, kwargs))
                return kwargs.get("body", b'{"schema_version":1}')

        client = FakeClient()
        configuration = {"schema_version": 1, "mode": "manual"}
        self.assertEqual(client.configure_exposure(configuration), configuration)
        self.assertEqual(client.requests[-1][0], "/api/v1/exposure")
        self.assertEqual(client.requests[-1][1]["headers"]["X-Nuvo-CSRF"], "csrf-token")
        self.assertEqual(client.requests[-1][1]["headers"]["If-Match"],'"'+"a"*64+'"')
        client.configure_stereo({"schema_version": 1, "algorithm": "sgbm"})
        self.assertEqual(client.requests[-1][0], "/api/v1/stereo")
        client.configure_encoding({"schema_version": 1, "mode": "raw"})
        self.assertEqual(client.requests[-1][0], "/api/v1/encoding")


if __name__ == "__main__": unittest.main()
