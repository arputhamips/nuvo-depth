#!/usr/bin/env python3
"""Serve a temporary browser-viewable MJPEG stream for manual lens focus."""

from __future__ import annotations

import argparse
import html
import io
import logging
import socketserver
from http import server
from threading import Condition

from libcamera import Transform
from picamera2 import Picamera2
from picamera2.encoders import MJPEGEncoder
from picamera2.outputs import FileOutput


class StreamingOutput(io.BufferedIOBase):
    def __init__(self) -> None:
        self.frame: bytes | None = None
        self.condition = Condition()

    def write(self, buffer: bytes) -> int:
        with self.condition:
            self.frame = bytes(buffer)
            self.condition.notify_all()
        return len(buffer)


def page(camera_index: int, width: int, height: int) -> bytes:
    title = f"Depth daemon focus - logical left camera {camera_index}"
    return f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{html.escape(title)}</title>
  <style>
    body {{ margin: 0; background: #111; color: #eee; font-family: sans-serif; }}
    header {{ padding: .6rem 1rem; }}
    img {{ display: block; width: 100vw; height: auto; image-rendering: auto; }}
  </style>
</head>
<body>
  <header>{html.escape(title)} - {width}x{height}, 180-degree orientation</header>
  <img src="/stream.mjpg" alt="Live left camera focus stream">
</body>
</html>
""".encode()


def handler_type(output: StreamingOutput, page_bytes: bytes) -> type[server.BaseHTTPRequestHandler]:
    class StreamingHandler(server.BaseHTTPRequestHandler):
        def do_GET(self) -> None:
            if self.path in ("/", "/index.html"):
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(page_bytes)))
                self.end_headers()
                self.wfile.write(page_bytes)
                return
            if self.path == "/healthz":
                body = b"ok\n"
                self.send_response(200)
                self.send_header("Content-Type", "text/plain")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            if self.path != "/stream.mjpg":
                self.send_error(404)
                return

            self.send_response(200)
            self.send_header("Age", "0")
            self.send_header("Cache-Control", "no-cache, private")
            self.send_header("Pragma", "no-cache")
            self.send_header(
                "Content-Type", "multipart/x-mixed-replace; boundary=FRAME"
            )
            self.end_headers()
            try:
                while True:
                    with output.condition:
                        output.condition.wait()
                        frame = output.frame
                    if frame is None:
                        continue
                    self.wfile.write(b"--FRAME\r\n")
                    self.send_header("Content-Type", "image/jpeg")
                    self.send_header("Content-Length", str(len(frame)))
                    self.end_headers()
                    self.wfile.write(frame)
                    self.wfile.write(b"\r\n")
            except (BrokenPipeError, ConnectionResetError):
                pass

        def log_message(self, message: str, *args: object) -> None:
            logging.info("%s - %s", self.client_address[0], message % args)

    return StreamingHandler


class StreamingServer(socketserver.ThreadingMixIn, server.HTTPServer):
    allow_reuse_address = True
    daemon_threads = True


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--camera-index", type=int, default=1)
    parser.add_argument("--width", type=int, default=1456)
    parser.add_argument("--height", type=int, default=1088)
    parser.add_argument("--framerate", type=float, default=10.0)
    parser.add_argument("--port", type=int, default=8000)
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")

    camera = Picamera2(args.camera_index)
    frame_duration_us = round(1_000_000 / args.framerate)
    configuration = camera.create_video_configuration(
        main={"size": (args.width, args.height), "format": "RGB888"},
        controls={"FrameDurationLimits": (frame_duration_us, frame_duration_us)},
        transform=Transform(hflip=1, vflip=1),
        buffer_count=4,
    )
    camera.configure(configuration)
    output = StreamingOutput()
    camera.start_recording(MJPEGEncoder(), FileOutput(output))
    logging.info(
        "serving camera %d at http://0.0.0.0:%d (%dx%d at %.1f Hz)",
        args.camera_index,
        args.port,
        args.width,
        args.height,
        args.framerate,
    )
    try:
        StreamingServer(
            ("", args.port), handler_type(output, page(args.camera_index, args.width, args.height))
        ).serve_forever()
    finally:
        camera.stop_recording()
        camera.close()


if __name__ == "__main__":
    main()
