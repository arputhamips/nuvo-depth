from __future__ import annotations

import http.cookiejar
import base64
import hashlib
import json
import math
import os
import re
import socket
import ssl
import struct
import time
import urllib.parse
import urllib.request
from collections.abc import Iterable, Iterator
from itertools import islice

from .frame import FrameSet, MAX_METADATA, MAX_PAYLOAD, PRODUCT_FORMATS, decode_frame
from .encoded import EncodedFrame, MAX_ENCODED_METADATA, MAX_ENCODED_PAYLOAD, decode_encoded_frame


_PRODUCTS = frozenset(PRODUCT_FORMATS)
_MAX_FRAME = MAX_PAYLOAD + MAX_METADATA + 32
_MAX_UPGRADE_HEADERS = 16 * 1024
_WEBSOCKET_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
_DEFAULT_PRODUCTS = tuple(PRODUCT_FORMATS)[:6]


def _subscription_path(path: str, cadence_hz: float | None,
                       products: Iterable[str] | None, delivery: str | None) -> str:
    query: dict[str, str] = {}
    if delivery is not None:
        if delivery not in {"best_effort", "exact", "auto"}:
            raise ValueError("stream delivery must be best_effort, exact, or auto")
        query["delivery"] = delivery
    # The gateway requires an explicit pair if either selection is supplied.
    if cadence_hz is not None or products is not None:
        cadence_hz = 10 if cadence_hz is None else cadence_hz
        products = _DEFAULT_PRODUCTS if products is None else products
        if (isinstance(cadence_hz, bool) or not isinstance(cadence_hz, (int, float))
                or not math.isfinite(cadence_hz) or not 0 < cadence_hz <= 60):
            raise ValueError("stream cadence must be finite and between 0 and 60 Hz")
        if isinstance(products, str):
            raise ValueError("products must be an iterable of product names")
        selected = tuple(islice(products, len(_PRODUCTS) + 1))
        if (not selected or len(selected) > len(_PRODUCTS)
                or any(not isinstance(product, str) or product not in _PRODUCTS for product in selected)
                or len(set(selected)) != len(selected)):
            raise ValueError("stream products must be distinct known product names")
        query["cadence_hz"] = str(cadence_hz)
        query["products"] = ",".join(selected)
    return path + ("?" + urllib.parse.urlencode(query, safe=",") if query else "")

def recovery_action(code: int, path: str = '') -> str:
    if code in (401,403): return 'SDK credentials/session rejected; obtain the long administrator token from the correct unit through trusted SSH and claim a new session.'
    if code==412: return 'Configuration changed; reload the requested document before saving your edits.'
    if code in (409,422,429): return 'Subscription or capacity rejected; reduce products/cadence, use best effort where appropriate, or inspect measured link admission. Exact requests are not silently degraded.'
    if code==503:
        if path=='/api/v1/session': return 'SDK session unavailable or capacity reached; close finished SDK clients and retry. On an older gateway without session cleanup, restart nuvo-gateway through trusted SSH when its connections can be interrupted. Capture and calibration may still be healthy.'
        return 'Monitoring unavailable; inspect the optional encoder setting and capture readiness.' if '/encoded/' in path else 'Raw output/capture unavailable; enable raw delivery and inspect calibration, services and required reboot.'
    return 'Inspect device diagnostics and the requested supported endpoint; do not disable TLS verification.'


class _FrameStream(Iterator[FrameSet]):
    """A synchronous, bounded RFC 6455 client with no background frame queue."""

    def __init__(self, client: "NuvoClient", path: str):
        self._client = client
        self._url = client.base_url + path
        self._socket: ssl.SSLSocket | None = None
        self._buffer = bytearray()
        self._closed = False
        self._deadline = 0.0
        self._qualification = "unknown"

    @property
    def qualification(self) -> str:
        """Gateway admission label; available after opening the stream."""
        return self._qualification

    def _set_deadline(self, duration: float | None = None) -> None:
        self._deadline = time.monotonic() + (
            self._client.timeout if duration is None else duration
        )

    def _remaining(self) -> float:
        remaining = self._deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("Nuvo frame stream timed out")
        return remaining

    def _read_exact(self, size: int) -> bytes:
        # Validate the WebSocket length before allocating, and receive directly
        # into bounded storage rather than accumulating an unbounded chunk list.
        if not 0 <= size <= _MAX_FRAME:
            raise ValueError("Nuvo WebSocket frame exceeds the client bound")
        data = bytearray(size)
        with memoryview(data) as view:
            self._read_into(view)
        return bytes(data)

    def _read_into(self, view: memoryview) -> None:
        """Fill caller-owned storage without an intermediate payload allocation."""
        self._remaining()
        size = len(view)
        buffered = min(size, len(self._buffer))
        if buffered:
            with memoryview(self._buffer) as pending:
                view[:buffered] = pending[:buffered]
            del self._buffer[:buffered]
        assert self._socket is not None
        offset = buffered
        while offset < size:
            self._socket.settimeout(self._remaining())
            received = self._socket.recv_into(view[offset:])
            if not received:
                raise ConnectionError("Nuvo frame stream disconnected before a complete message")
            offset += received

    def _connect(self) -> None:
        if self._closed:
            raise RuntimeError("Nuvo frame stream is closed")
        if self._socket is not None:
            return
        if not self._client._csrf:
            raise RuntimeError("claim the device before opening a frame stream")
        parsed = urllib.parse.urlsplit(self._url)
        request = urllib.request.Request(self._url)
        self._client._cookies.add_cookie_header(request)
        cookie = request.get_header("Cookie")
        if not cookie:
            raise RuntimeError("the claimed Nuvo session has expired; claim the device again")
        key = base64.b64encode(os.urandom(16))
        target = urllib.parse.urlunsplit(("", "", parsed.path or "/", parsed.query, ""))
        headers = (
            f"GET {target} HTTP/1.1\r\nHost: {parsed.netloc}\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key.decode('ascii')}\r\n"
            f"Sec-WebSocket-Version: 13\r\nCookie: {cookie}\r\n\r\n"
        ).encode("ascii")
        self._set_deadline()
        raw_socket = socket.create_connection(
            (parsed.hostname, parsed.port or 443), timeout=self._remaining()
        )
        try:
            raw_socket.settimeout(self._remaining())
            self._socket = self._client._context.wrap_socket(
                raw_socket, server_hostname=parsed.hostname
            )
        except BaseException:
            raw_socket.close()
            raise
        try:
            self._socket.settimeout(self._remaining())
            self._socket.sendall(headers)
            while b"\r\n\r\n" not in self._buffer:
                if len(self._buffer) >= _MAX_UPGRADE_HEADERS:
                    raise ValueError("Nuvo WebSocket upgrade headers exceed the client bound")
                self._socket.settimeout(self._remaining())
                chunk = self._socket.recv(min(4096, _MAX_UPGRADE_HEADERS - len(self._buffer)))
                if not chunk:
                    raise ConnectionError("Nuvo disconnected during WebSocket upgrade")
                self._buffer.extend(chunk)
            head, remainder = self._buffer.split(b"\r\n\r\n", 1)
            self._buffer = remainder
            lines = head.decode("ascii").split("\r\n")
            if lines[0].split(" ", 2)[:2] != ["HTTP/1.1", "101"]:
                # Include only the status, never cookie or other response headers.
                parts=lines[0].split(' ',2)
                code=int(parts[1]) if len(parts)>1 and parts[1].isdigit() else 0
                raise ConnectionError(f"Nuvo WebSocket upgrade rejected (HTTP {code}): {recovery_action(code,self._url)}")
            response_headers: dict[str, list[str]] = {}
            for line in lines[1:]:
                name, separator, value = line.partition(":")
                if not separator or not name or name.strip() != name:
                    raise ValueError("invalid Nuvo WebSocket upgrade headers")
                response_headers.setdefault(name.lower(), []).append(value.strip())
            expected = base64.b64encode(hashlib.sha1(key + _WEBSOCKET_GUID).digest()).decode("ascii")
            connection = ",".join(response_headers.get("connection", [])).lower().split(",")
            if (
                response_headers.get("sec-websocket-accept") != [expected]
                or response_headers.get("upgrade", [""])[0].lower() != "websocket"
                or "upgrade" not in [token.strip() for token in connection]
                or "sec-websocket-extensions" in response_headers
                or "sec-websocket-protocol" in response_headers
            ):
                raise ValueError("invalid or unsupported Nuvo WebSocket upgrade")
            qualification = response_headers.get("x-nuvo-stream-qualification", ["unknown"])
            if len(qualification) != 1 or qualification[0] not in {"unknown", "qualified-exact", "engineering-best-effort", "best-effort"}:
                raise ValueError("invalid Nuvo stream qualification")
            self._qualification = qualification[0]
        except BaseException:
            self._disconnect()
            raise

    def _send_control(self, opcode: int, data: bytes) -> None:
        if len(data) > 125:
            raise ValueError("WebSocket control payload exceeds 125 bytes")
        assert self._socket is not None
        mask = os.urandom(4)
        masked = bytes(value ^ mask[index % 4] for index, value in enumerate(data))
        self._socket.settimeout(self._remaining())
        self._socket.sendall(bytes((0x80 | opcode, 0x80 | len(data))) + mask + masked)

    def ping(self, payload: bytes = b"") -> None:
        """Send a masked ping; iteration handles the server's pong response."""
        if not isinstance(payload, bytes) or len(payload) > 125:
            raise ValueError("WebSocket ping requires at most 125 bytes")
        self._connect()
        self._set_deadline()
        try:
            self._send_control(9, payload)
        except BaseException:
            self._disconnect()
            raise

    def __next__(self) -> FrameSet:
        if self._closed:
            raise StopIteration
        try:
            self._connect()
            self._set_deadline()
            message = bytearray()
            fragmented = False
            control_frames = 0
            while True:
                first, second = self._read_exact(2)
                final, opcode = bool(first & 0x80), first & 0x0F
                if first & 0x70 or second & 0x80 or opcode not in {0, 1, 2, 8, 9, 10}:
                    raise ValueError("invalid or unsupported Nuvo WebSocket frame")
                length = second & 0x7F
                if length == 126:
                    length = struct.unpack(">H", self._read_exact(2))[0]
                    if length < 126:
                        raise ValueError("noncanonical WebSocket frame length")
                elif length == 127:
                    length = struct.unpack(">Q", self._read_exact(8))[0]
                    if length < 65536 or length & (1 << 63):
                        raise ValueError("invalid WebSocket frame length")
                if opcode >= 8:
                    control_frames += 1
                    if not final or length > 125 or control_frames > 128:
                        raise ValueError("invalid or excessive WebSocket control frames")
                    data = self._read_exact(length)
                    if opcode == 9:
                        self._send_control(10, data)
                    elif opcode == 8:
                        if len(data) == 1:
                            raise ValueError("invalid WebSocket close payload")
                        code = struct.unpack(">H", data[:2])[0] if data else 1000
                        if code < 1000 or code >= 5000 or code in {1004, 1005, 1006, 1015}:
                            raise ValueError("invalid WebSocket close code")
                        reason = data[2:].decode("utf-8")
                        self._send_control(8, data)
                        self._disconnect()
                        if message or fragmented or code not in {1000, 1001}:
                            raise ConnectionError(f"Nuvo frame stream closed ({code}): {reason}")
                        raise StopIteration
                    continue
                if opcode == 1 or (opcode == 0) != fragmented:
                    raise ValueError("Nuvo stream requires associated binary messages")
                if length > _MAX_FRAME - len(message):
                    raise ValueError("Nuvo WebSocket message exceeds the client bound")
                if final and not fragmented:
                    # The gateway normally sends a single binary frame. Avoid
                    # copying it through the fragmentation accumulator.
                    return decode_frame(self._read_exact(length))
                start = len(message)
                message.extend(bytearray(length))
                with memoryview(message)[start:] as destination:
                    self._read_into(destination)
                if final:
                    return decode_frame(bytes(message))
                fragmented = True
        except StopIteration:
            raise
        except BaseException:
            self._disconnect()
            raise

    def _disconnect(self) -> None:
        self._closed = True
        self._buffer.clear()
        if self._socket is not None:
            self._socket.close()
            self._socket = None

    def close(self, code: int = 1000, reason: str = "") -> None:
        """Close immediately after sending a bounded close notification."""
        if code < 1000 or code >= 5000 or code in {1004, 1005, 1006, 1015}:
            raise ValueError("invalid WebSocket close code")
        data = struct.pack(">H", code) + reason.encode("utf-8")
        if len(data) > 125:
            raise ValueError("WebSocket close reason exceeds 123 UTF-8 bytes")
        try:
            if self._socket is not None:
                self._set_deadline(min(self._client.timeout, 0.25))
                self._send_control(8, data)
        except OSError:
            pass
        finally:
            self._disconnect()

    def __enter__(self) -> "_FrameStream":
        try:
            self._connect()
        except BaseException:
            self._disconnect()
            raise
        return self

    def __exit__(self, *_: object) -> None:
        self.close()


class NuvoClient:
    """Authenticated HTTPS client for WiFi, Ethernet, and USB Ethernet."""

    def __init__(self, base_url: str, *, ca_file: str | None = None, timeout: float = 5.0):
        parsed = urllib.parse.urlsplit(base_url)
        if (parsed.scheme != "https" or not parsed.hostname or parsed.username
                or parsed.password or parsed.query or parsed.fragment
                or any(ord(character) <= 32 for character in base_url)):
            raise ValueError("Nuvo devices require an https:// URL")
        if not math.isfinite(timeout) or timeout <= 0:
            raise ValueError("Nuvo client timeout must be finite and positive")
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout
        context = ssl.create_default_context(cafile=ca_file)
        self._context = context
        self._cookies = http.cookiejar.CookieJar()
        self._csrf = ""
        self._close_confirmed = True
        self._configuration_receipts: dict[str, dict[str, str]] = {}
        self._opener = urllib.request.build_opener(
            urllib.request.HTTPCookieProcessor(self._cookies),
            urllib.request.HTTPSHandler(context=context),
        )

    def close(self) -> bool:
        """Release this session and clear local authorization; safe to repeat.

        Close frame-stream contexts first. False means remote revocation could
        not be confirmed (including older gateways without this operation).
        Cleanup never masks an existing acquisition or application exception.
        """
        if not self._csrf:
            return self._close_confirmed
        revoked = False
        try:
            if self._csrf:
                self._request('/api/v1/session', method='DELETE',
                              headers={'Origin': self.base_url, 'X-Nuvo-CSRF': self._csrf}, maximum=4096)
                revoked = True
        except urllib.error.HTTPError as error:
            error.close()
        except (OSError, ValueError, RuntimeError):
            pass
        finally:
            self._cookies.clear()
            self._csrf = ''
            self._configuration_receipts.clear()
            self._close_confirmed = revoked
        return revoked

    def __enter__(self) -> 'NuvoClient': return self
    def __exit__(self, *_: object) -> None: self.close()

    def _request(self, path: str, *, method: str = "GET", headers: dict[str, str] | None = None, body: bytes | None = None, maximum: int = MAX_PAYLOAD + MAX_METADATA + 32) -> bytes:
        request = urllib.request.Request(
            self.base_url + path, method=method, headers=headers or {}, data=body
        )
        try:
            response = self._opener.open(request, timeout=self.timeout)
        except urllib.error.HTTPError as error:
            # Preserve HTTPError/code compatibility and exclude response bodies,
            # request tokens and cookies from user-facing recovery text.
            error.msg = recovery_action(error.code,path)
            raise
        except urllib.error.URLError as error:
            error.add_note('Verify the device hostname and trusted public certificate fingerprint; retain TLS verification and check network reachability.')
            raise
        with response:
            length = response.headers.get("Content-Length")
            if length is not None and int(length) > maximum:
                raise ValueError("Nuvo response exceeds the client bound")
            data = response.read(maximum + 1)
            if len(data) > maximum:
                raise ValueError("Nuvo response exceeds the client bound")
            if path in {"/api/v1/encoding", "/api/v1/stereo", "/api/v1/exposure"}:
                etag = response.headers.get("ETag", "")
                if not re.fullmatch(r'"[a-f0-9]{64}"', etag):
                    raise ValueError("Nuvo configuration response has no strong revision")
                state = response.headers.get("X-Nuvo-Configuration-State", "requested")
                self._configuration_receipts[path] = {
                    "etag":etag,"requested_revision":etag[1:-1],"state":state,
                }
            return data

    def configuration_receipt(self, kind: str) -> dict[str, str]:
        """Return the last requested/saved revision and state, never an apply guarantee.

        After configure_* succeeds, state is pending. A stereo revision is
        applied only when stereo_applied_revision in a frame/health matches it.
        Camera-reported exposure values describe actual per-camera settings.
        """
        if kind not in {"encoding", "stereo", "exposure"}:
            raise ValueError("unknown configuration kind")
        return dict(self._configuration_receipts.get("/api/v1/" + kind, {}))

    def claim(self, token: str) -> str:
        if not 32 <= len(token) <= 256:
            raise ValueError("claim token length is invalid")
        document = json.loads(
            self._request(
                "/api/v1/session",
                method="POST",
                headers={"Authorization": f"Bearer {token}"},
                maximum=4096,
            )
        )
        self._csrf = str(document["csrf"])
        self._close_confirmed = False
        return str(document["role"])

    def health(self) -> dict:
        return json.loads(self._request("/api/v1/health", maximum=64 * 1024))

    def capabilities(self) -> dict:
        return json.loads(self._request("/api/v1/capabilities", maximum=64 * 1024))

    def latest(self, *, cadence_hz: float | None = None,
               products: Iterable[str] | None = None,
               delivery: str | None = None) -> FrameSet:
        """Fetch one associated frame with the same selection rules as frames()."""
        path = _subscription_path("/api/v1/frame/latest", cadence_hz, products, delivery)
        return decode_frame(self._request(path))

    def frames(self, *, cadence_hz: float | None = None,
               products: Iterable[str] | None = None,
               delivery: str | None = None) -> _FrameStream:
        """Open an authenticated persistent frame stream.

        Use as a context manager to release the connection when stopping early.
        Delivery may be auto, exact, or best_effort. Check qualification after
        opening; best-effort cadence is an upper bound. Without options, use
        the legacy six-product default subscription. With only products, the
        default ceiling is 10 Hz; with only cadence, the legacy products apply.
        Prefer an explicit small selection such as depth + validity at 2 Hz.
        """
        path = _subscription_path("/api/v1/stream", cadence_hz, products, delivery)
        return _FrameStream(self, path)

    def encoding(self) -> dict:
        return json.loads(self._request("/api/v1/encoding", maximum=64 * 1024))

    def _configure(self, path: str, configuration: dict, expected_etag: str | None = None) -> dict:
        if not self._csrf:
            raise RuntimeError("claim the device before changing configuration")
        etag = expected_etag if expected_etag is not None else self._configuration_receipts.get(path, {}).get("etag")
        if not etag:
            raise RuntimeError("read the configuration before saving, or provide expected_etag")
        if not re.fullmatch(r'"[a-f0-9]{64}"', etag):
            raise ValueError("expected_etag must be one strong configuration ETag")
        body = json.dumps(configuration, separators=(",", ":")).encode()
        return json.loads(self._request(
            path, method="POST", body=body,
            headers={"Content-Type": "application/json", "X-Nuvo-CSRF": self._csrf, "If-Match":etag},
            maximum=64 * 1024,
        ))

    def configure_encoding(self, configuration: dict, *, expected_etag: str | None = None) -> dict:
        """Select raw delivery, monitoring encoding, or both."""
        return self._configure("/api/v1/encoding", configuration, expected_etag)

    def stereo(self) -> dict:
        """Return requested stereo controls and remember their revision for saving."""
        return json.loads(self._request("/api/v1/stereo", maximum=64 * 1024))

    def configure_stereo(self, configuration: dict, *, expected_etag: str | None = None) -> dict:
        """Save validated stereo controls conditionally; receipt remains pending."""
        return self._configure("/api/v1/stereo", configuration, expected_etag)

    def exposure(self) -> dict:
        """Return requested exposure controls and remember their revision."""
        return json.loads(self._request("/api/v1/exposure", maximum=64 * 1024))

    def configure_exposure(self, configuration: dict, *, expected_etag: str | None = None) -> dict:
        """Save exposure commands conditionally; check camera metadata for effects."""
        return self._configure("/api/v1/exposure", configuration, expected_etag)

    def latest_encoded(self) -> EncodedFrame:
        maximum=MAX_ENCODED_METADATA+MAX_ENCODED_PAYLOAD+32
        return decode_encoded_frame(self._request("/api/v1/encoded/latest",maximum=maximum))
