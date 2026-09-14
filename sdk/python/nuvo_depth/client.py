from __future__ import annotations

import http.cookiejar
import json
import ssl
import urllib.request

from .frame import FrameSet, MAX_METADATA, MAX_PAYLOAD, decode_frame
from .encoded import EncodedFrame, MAX_ENCODED_METADATA, MAX_ENCODED_PAYLOAD, decode_encoded_frame


class NuvoClient:
    """Authenticated HTTPS client shared by Ethernet and USB Ethernet."""

    def __init__(self, base_url: str, *, ca_file: str | None = None, timeout: float = 5.0):
        if not base_url.startswith("https://"):
            raise ValueError("Nuvo devices require an https:// URL")
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout
        context = ssl.create_default_context(cafile=ca_file)
        self._cookies = http.cookiejar.CookieJar()
        self._csrf = ""
        self._opener = urllib.request.build_opener(
            urllib.request.HTTPCookieProcessor(self._cookies),
            urllib.request.HTTPSHandler(context=context),
        )

    def _request(self, path: str, *, method: str = "GET", headers: dict[str, str] | None = None, body: bytes | None = None, maximum: int = MAX_PAYLOAD + MAX_METADATA + 32) -> bytes:
        request = urllib.request.Request(
            self.base_url + path, method=method, headers=headers or {}, data=body
        )
        with self._opener.open(request, timeout=self.timeout) as response:
            length = response.headers.get("Content-Length")
            if length is not None and int(length) > maximum:
                raise ValueError("Nuvo response exceeds the client bound")
            data = response.read(maximum + 1)
            if len(data) > maximum:
                raise ValueError("Nuvo response exceeds the client bound")
            return data

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
        return str(document["role"])

    def health(self) -> dict:
        return json.loads(self._request("/api/v1/health", maximum=64 * 1024))

    def capabilities(self) -> dict:
        return json.loads(self._request("/api/v1/capabilities", maximum=64 * 1024))

    def latest(self) -> FrameSet:
        return decode_frame(self._request("/api/v1/frame/latest"))

    def encoding(self) -> dict:
        return json.loads(self._request("/api/v1/encoding", maximum=64 * 1024))

    def configure_encoding(self, configuration: dict) -> dict:
        if not self._csrf:
            raise RuntimeError("claim the device before changing encoding")
        body=json.dumps(configuration,separators=(",",":")).encode()
        return json.loads(self._request(
            "/api/v1/encoding",method="POST",body=body,
            headers={"Content-Type":"application/json","X-Nuvo-CSRF":self._csrf},
            maximum=64 * 1024))

    def latest_encoded(self) -> EncodedFrame:
        maximum=MAX_ENCODED_METADATA+MAX_ENCODED_PAYLOAD+32
        return decode_encoded_frame(self._request("/api/v1/encoded/latest",maximum=maximum))
