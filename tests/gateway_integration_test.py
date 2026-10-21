import hashlib
import importlib.util
import http.cookiejar
import http.client
import array
import fcntl
import json
import os
from pathlib import Path
import secrets
import shutil
import socket
import ssl
import subprocess
import sys
import struct
import threading
import tempfile
import time
import unittest
import urllib.error
import urllib.request

sys.path.insert(0, str(Path(__file__).parents[1] / "sdk/python"))
from nuvo_depth import NuvoClient
from nuvo_depth.client import _FrameStream
from nuvo_depth.encoded import decode_encoded_frame


def ethernet_address():
    for _, interface in socket.if_nameindex():
        root = Path("/sys/class/net") / interface
        if interface == "lo" or (root / "wireless").exists():
            continue
        try:
            if (root / "type").read_text().strip() != "1":
                continue
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as query:
                result = fcntl.ioctl(query.fileno(), 0x8915, struct.pack("256s", interface.encode()[:15]))
            return interface, socket.inet_ntoa(result[20:24])
        except OSError:
            continue
    return None


PRODUCTS = (("left_rectified", "gray8", 1), ("right_rectified", "gray8", 1),
            ("disparity", "float32_le", 4), ("depth", "float32_le", 4),
            ("confidence", "uint8", 1), ("validity", "uint8", 1),
            ("left_mono", "gray8", 1), ("right_mono", "gray8", 1),
            ("left_rgb", "rgb8", 3), ("right_rgb", "rgb8", 3),
            ("left_rectified_rgb", "rgb8", 3), ("right_rectified_rgb", "rgb8", 3),
            ("rectified_stereo", "gray8", 2), ("rectified_stereo_rgb", "rgb8", 6))


def raw_frame(sequence, run=2, configuration_epoch=1, width=1, height=1, mask=63, capture_time_ns=None):
    pixels = width * height
    payload = bytearray()
    products = []
    for bit, (kind, fmt, size) in enumerate(PRODUCTS):
        if not mask & (1 << bit):
            continue
        stereo = kind.startswith("rectified_stereo")
        length = size * pixels
        products.append(dict(kind=kind, format=fmt, offset=len(payload), length=length,
                             width=width * (2 if stereo else 1), height=height))
        if fmt == "float32_le":
            payload.extend(struct.pack("<f", float(sequence) if kind == "depth" else 1.) * pixels)
        elif fmt in ("rgb8", "bgr8"):
            payload.extend(bytes((11, 22, 33)) * pixels * (2 if stereo else 1))
        else:
            payload.extend(bytes((255 if kind == "confidence" else 2 if kind == "right_rectified" else 1,)) * length)
    metadata = json.dumps(dict(schema_version=1, device_uuid="01" + "00" * 15,
        run_uuid=f"{run:02x}" + "00" * 15, capture_epoch=1, capture_sequence=sequence,
        configuration_epoch=configuration_epoch, capture_time_ns=time.clock_gettime_ns(time.CLOCK_BOOTTIME) if capture_time_ns is None else capture_time_ns, capture_clock="boottime",
        stereo_configuration_generation=1, stereo_applied_revision="ab" * 32,
        stereo_requested_revision="ab" * 32, stereo_configuration_error="",
        calibration_sha256="01" + "00" * 31, calibration_authority=1,
        depth_unit="metres", depth_convention="optical_axis_z", available_products=mask,
        selected_products=mask, products=products)).encode()
    return struct.pack(">4sHHHHIQQ", b"NUV1", 1, 0, 5, 0, len(metadata), len(payload), sequence) + metadata + payload


def encoded_frame(sequence=1, capture_time_ns=None, **changes):
    # Match the real encoder's canonical UUID spelling. Raw NUV1 uses hex.
    metadata = dict(schema_version=1, device_uuid="01000000-0000-0000-0000-000000000000",
        run_uuid="02000000-0000-0000-0000-000000000000", capture_epoch=1, capture_sequence=sequence,
        configuration_epoch=1, capture_clock=1,
        capture_time_ns=time.clock_gettime_ns(time.CLOCK_BOOTTIME) if capture_time_ns is None else capture_time_ns,
        calibration_sha256="01" + "00" * 31, calibration_authority=1,
        encoding_configuration_generation=1, encoding_applied_revision="ab" * 32,
        source="left_rectified", codec="jpeg", mime_type="image/jpeg", codec_string="jpeg",
        width=1, height=1, keyframe=True, encode_ms=.1)
    metadata.update(changes)
    document = json.dumps(metadata).encode()
    # Codec decoding is tested with real encoders separately; this fixture
    # exercises a valid NVE1 envelope with a nonempty JPEG-marked payload.
    payload = bytes((255, 216, 255, 217))
    return struct.pack(">4sHHIQQI", b"NVE1", 1, 1, len(document), len(payload), sequence, 0) + document + payload


class LocalFrames:
    """Persistent demand/SCM_RIGHTS fixture; idle clients never fetch frames."""
    def __init__(self, path):
        self.condition = threading.Condition()
        self.parameters = (7, 2, 1, 1, 1)
        self.cursor = bytes((2,)) + bytes(15) + struct.pack(">QQ", 1, 7)
        self.stopping = False
        self.requests = 0
        self.subscriptions = {}
        self.subscription_updates = []
        self.connections = set()
        self.handlers = []
        self.encoded_path = Path(path).parent / "encoded.nve"
        self.auto_encode = True
        self.encoded_bytes = None
        self.encoded_cursor = bytes(32)
        self.encoded_sequence = 0
        self.encoded_waits = []
        self.encoded_deliveries = 0
        self.encoded_delivery_peers = []
        self.serve_frames = True
        self.stale_frames = False
        self.stale_deliveries = 0
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.listener.bind(str(path)); self.listener.listen(8); self.listener.settimeout(.1)
        self.worker = threading.Thread(target=self.serve, daemon=True); self.worker.start()
        self.encoder = threading.Thread(target=self.encode, daemon=True); self.encoder.start()

    def demand(self):
        with self.condition:
            mask, cadence, preview = 0, 0, 0
            for products, raw_hz, preview_hz in self.subscriptions.values():
                mask |= products; cadence = max(cadence, raw_hz); preview = max(preview, preview_hz)
            return mask, cadence, preview

    def publish(self, sequence, run=2, configuration_epoch=1, width=1, height=1):
        with self.condition:
            self.parameters = (sequence, run, configuration_epoch, width, height)
            self.cursor = bytes((run,)) + bytes(15) + struct.pack(">QQ", 1, sequence)
            self.condition.notify_all()

    def encode(self):
        previous = None
        while not self.stopping:
            if self.auto_encode and self.demand()[2]:
                temporary = self.encoded_path.with_suffix(".tmp")
                temporary.write_bytes(encoded_frame(self.encoded_sequence + 1)); temporary.replace(self.encoded_path)
            try:
                modified = self.encoded_path.stat().st_mtime_ns
                value = self.encoded_path.read_bytes()
            except FileNotFoundError:
                modified, value = None, None
            if modified != previous:
                with self.condition:
                    self.encoded_sequence += 1
                    self.encoded_cursor = bytes((2,)) + bytes(15) + struct.pack(">QQ", 1, self.encoded_sequence)
                    self.encoded_bytes = value
                    self.condition.notify_all()
                previous = modified
            time.sleep(.05)

    def serve(self):
        while not self.stopping:
            try: connection, _ = self.listener.accept()
            except socket.timeout: continue
            with self.condition: self.connections.add(connection)
            worker = threading.Thread(target=self.handle, args=(connection,), daemon=True)
            self.handlers.append(worker); worker.start()

    def handle(self, connection):
        try:
            connection.settimeout(.2)
            with connection:
                while not self.stopping:
                    try: request = connection.recv(48)
                    except socket.timeout: continue
                    if not request: break
                    if len(request) == 20 and request[:4] == b"NVS2":
                        mask, raw, preview, reserved = struct.unpack_from(">IIII", request, 4)
                        if reserved: break
                        with self.condition:
                            self.subscriptions[connection] = (mask, raw / 1000, preview / 1000)
                            self.subscription_updates.append(self.subscriptions[connection])
                            self.condition.notify_all()
                        connection.send(b"NVA2"); continue
                    if len(request) != 48 or request[:4] not in (b"NVW2", b"NVY2"): break
                    encoded = request[:4] == b"NVY2"
                    if encoded:
                        with self.condition:
                            self.encoded_waits.append((connection, request[16:]))
                            self.condition.notify_all()
                            self.condition.wait_for(lambda: self.stopping or
                                (self.encoded_bytes is not None and request[16:] != self.encoded_cursor) or
                                (self.encoded_bytes is None and request[16:] != bytes(32)),
                                timeout=struct.unpack_from(">I", request, 8)[0] / 1000)
                            value, cursor = self.encoded_bytes, self.encoded_cursor
                        if value is None:
                            connection.send(b"NVI2"); continue
                    else:
                        mask = struct.unpack_from(">I", request, 4)[0]
                        with self.condition:
                            self.requests += 1
                            self.condition.wait_for(lambda: self.stopping or request[16:] != self.cursor,
                                                    timeout=struct.unpack_from(">I", request, 8)[0] / 1000)
                            parameters, cursor = self.parameters, self.cursor
                            captured = time.clock_gettime_ns(time.CLOCK_BOOTTIME)
                        value = raw_frame(*parameters, mask=mask, capture_time_ns=1 if self.stale_frames else captured)
                    if request[16:] == cursor or self.stopping or (not encoded and not self.serve_frames):
                        connection.send(b"NVN2"); continue
                    if encoded:
                        with self.condition:
                            self.encoded_deliveries += 1
                            self.encoded_delivery_peers.append((connection, cursor))
                            self.condition.notify_all()
                    if not encoded and self.stale_frames:
                        with self.condition:
                            self.stale_deliveries += 1
                            self.condition.notify_all()
                    descriptor = os.memfd_create("gateway-test-frame", os.MFD_ALLOW_SEALING)
                    try:
                        os.write(descriptor, value)
                        fcntl.fcntl(descriptor, fcntl.F_ADD_SEALS,
                            fcntl.F_SEAL_WRITE | fcntl.F_SEAL_GROW | fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_SEAL)
                        connection.sendmsg([b"NVR2" + bytes(4) + struct.pack(">Q", len(value)) + cursor],
                            [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", [descriptor]))])
                    finally: os.close(descriptor)
        except (OSError, BrokenPipeError, ConnectionResetError): pass
        finally:
            with self.condition:
                self.subscriptions.pop(connection, None); self.connections.discard(connection)
                self.condition.notify_all()

    def close(self):
        with self.condition:
            self.stopping = True; self.condition.notify_all()
            for connection in self.connections:
                try: connection.shutdown(socket.SHUT_RDWR)
                except OSError: pass
        self.worker.join(timeout=2); self.encoder.join(timeout=2)
        for worker in self.handlers: worker.join(timeout=2)
        self.listener.close()


class ConfigurationAgent:
    """Run the privileged atomic writer through the actual seqpacket protocol."""
    def __init__(self,path,root):
        source=Path(__file__).parents[1]/"tools/nuvoctl.py"
        spec=importlib.util.spec_from_file_location("nuvoctl_gateway_fixture",source)
        self.module=importlib.util.module_from_spec(spec); spec.loader.exec_module(self.module)
        sys.path.insert(0,str(source.parent))
        import nuvo_setup
        self.setup=nuvo_setup
        self.root=root
        self.stopping=False
        self.before_write=None
        self.listener=socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET)
        self.listener.bind(str(path)); self.listener.listen(8); self.listener.settimeout(.1)
        self.worker=threading.Thread(target=self.serve,daemon=True); self.worker.start()

    def serve(self):
        while not self.stopping:
            try: connection,_=self.listener.accept()
            except socket.timeout: continue
            with connection:
                request=json.loads(connection.recv(8192))
                try:
                    if self.before_write:
                        callback,self.before_write=self.before_write,None
                        callback()
                    kind=request["operation"].split(".")[0]
                    if request["operation"]=="setup.authenticate":
                        result=self.setup.authenticate(self.root,request["secret"])
                    elif request["operation"]=="setup.owner":
                        result=self.setup.set_owner_password(self.root,request["password"])
                    elif request["operation"]=="setup.status":
                        result=self.setup.status(self.root)
                    else:
                        result=getattr(self.module,"set_"+kind)(
                            self.root,request["configuration"],request["expected_revision"])
                    response={"status":"ok","result":result}
                except self.module.ConfigurationConflict as error:
                    response={"status":"error","error_code":"precondition_failed","error":str(error)}
                except Exception as error:
                    response={"status":"error","error":str(error)}
                connection.send(json.dumps(response,separators=(",",":")).encode())

    def close(self):
        self.stopping=True; self.worker.join(timeout=2); self.listener.close()


class GatewayIntegrationTest(unittest.TestCase):
    def test_http_keepalive_is_bounded_and_authentication_is_per_request(self):
        address = urllib.parse.urlsplit(self.origin)
        connection = http.client.HTTPSConnection(address.hostname, address.port,
            context=ssl.create_default_context(cafile=str(self.certificate)), timeout=5)
        try:
            headers = {"Authorization": f"Bearer {self.token}"}
            for index in range(32):
                connection.request("GET", "/api/v1/encoding", headers=headers if index != 1 else {})
                response = connection.getresponse()
                self.assertEqual(response.status, 401 if index == 1 else 200)
                response.read()
                if index == 0:
                    identity = connection.sock
                if index < 31:
                    self.assertIs(connection.sock, identity)
                    self.assertEqual(response.getheader("Connection"), None)
                else:
                    self.assertEqual(response.getheader("Connection"), "close")
        finally:
            connection.close()

    def test_idle_http_keepalive_leaves_capacity_for_streams(self):
        address = urllib.parse.urlsplit(self.origin)
        connections = []
        try:
            for index in range(5):
                connection = http.client.HTTPSConnection(address.hostname, address.port,
                    context=ssl.create_default_context(cafile=str(self.certificate)), timeout=5)
                connections.append(connection)
                connection.request("GET", "/api/v1/encoding", headers={"Authorization": f"Bearer {self.token}"})
                response = connection.getresponse(); response.read()
                self.assertEqual(response.status, 200)
                if index == 4:
                    self.assertEqual(response.getheader("Connection"), "close")
            client = NuvoClient(self.origin, ca_file=self.certificate)
            client._cookies = self.cookies
            client._csrf = "existing-test-session"
            with client.frames(cadence_hz=1, products=["left_mono"]) as stream:
                self.assertIn("left_mono", next(stream).products)
        finally:
            for connection in connections:
                connection.close()

    def test_setup_code_password_handoff_and_csrf(self):
        setup=self.agent.setup
        setup.save(self.agent.root,setup.AUTH,{"schema_version":1,"claimed":False})
        code=setup.issue_pairing(self.agent.root)
        headers={"Content-Type":"application/json"}
        # Use an independent browser to ensure existing fixture cookies cannot
        # authenticate a deliberately bad/reused pairing code.
        jar=http.cookiejar.CookieJar()
        browser=urllib.request.build_opener(urllib.request.HTTPSHandler(context=ssl._create_unverified_context()),urllib.request.HTTPCookieProcessor(jar))
        def post(path,value,extra=None):
            return browser.open(urllib.request.Request(self.origin+path,data=json.dumps(value).encode(),headers=headers|dict(extra or {}),method="POST"),timeout=5)
        with post("/api/v1/session",{"secret":code}) as response:
            session=json.load(response)
        self.assertTrue(session["needs_owner_password"])
        with self.assertRaises(urllib.error.HTTPError) as error:
            post("/api/v1/setup/owner",{"password":"my new owner password"})
        self.assertEqual(error.exception.code,403)
        with post("/api/v1/setup/owner",{"password":"my new owner password"},{"X-Nuvo-CSRF":session["csrf"]}) as response:
            self.assertTrue(json.load(response)["owner_configured"])
        jar.clear()
        with self.assertRaises(urllib.error.HTTPError) as error:
            post("/api/v1/session",{"secret":code})
        self.assertEqual(error.exception.code,401)
        with post("/api/v1/session",{"secret":"my new owner password"}) as response:
            self.assertFalse(json.load(response)["needs_owner_password"])
        # Restore independent auth fixture state for subsequent legacy tests.
        (self.agent.root/setup.AUTH).unlink()

    @classmethod
    def setUpClass(cls):
        cls.binary=Path(sys.argv[1]).resolve(); web_source=Path(sys.argv[2]).resolve()
        cls.temporary=tempfile.TemporaryDirectory(); root=Path(cls.temporary.name)
        cls.web=root/'web'
        shutil.copytree(web_source, cls.web, ignore=shutil.ignore_patterns('pipeline-ui','node_modules','dist','__pycache__'))
        for name in ('pipeline.html','pipeline-model.js','pipeline.js','pipeline-host.js',
                     'pipeline-canvas.js','pipeline.css','pipeline-canvas.css','pipeline-licenses.txt'):
            (cls.web/name).write_text('stale prototype fixture')
        cls.local = LocalFrames(root / "data.sock")
        cls.agent=ConfigurationAgent(root/"agent.sock",root)
        cls.config_root=root/"etc/nuvo-depth"; cls.config_root.mkdir(parents=True)
        cls.token=secrets.token_hex(32); (root/"token").write_text(cls.token+"\n")
        cls.encoding={"schema_version":1,"mode":"both","codec":"jpeg","source":"left_rectified",
                      "width":0,"height":0,"cadence_hz":10,"bitrate_bps":4000000,
                      "jpeg_quality":80,"keyframe_interval":30,"threads":2,
                      "low_latency":True,"preset":"veryfast"}
        (cls.config_root/"encoding.json").write_text(json.dumps(cls.encoding)+"\n")
        cls.stereo=json.loads((Path(__file__).parents[1]/"config/stereo.json").read_text())
        (cls.config_root/"stereo.json").write_text(json.dumps(cls.stereo)+"\n")
        cls.exposure=json.loads((Path(__file__).parents[1]/"config/exposure.json").read_text())
        (cls.config_root/"exposure.json").write_text(json.dumps(cls.exposure)+"\n")
        (root/"encoded.nve").write_bytes(encoded_frame())
        cls.ethernet = ethernet_address()
        certificate_addresses = "IP:127.0.0.1"
        if cls.ethernet:
            certificate_addresses += ",IP:" + cls.ethernet[1]
        subprocess.run(["openssl","req","-x509","-newkey","rsa:2048","-nodes","-days","1",
                        "-subj","/CN=127.0.0.1","-addext","subjectAltName=" + certificate_addresses,
                        "-keyout",str(root/"key.pem"),"-out",str(root/"cert.pem")],
                       check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        cls.certificate = str(root / "cert.pem")
        with socket.socket(socket.AF_INET,socket.SOCK_STREAM) as reservation:
            reservation.bind(("127.0.0.1",0)); cls.port=reservation.getsockname()[1]
        cls.process=subprocess.Popen([
            str(cls.binary),"--address","127.0.0.1","--port",str(cls.port),
            "--certificate",str(root/"cert.pem"),"--private-key",str(root/"key.pem"),
            "--token-file",str(root/"token"),"--web-root",str(cls.web),
            "--status-file",str(root/"missing-status"),"--snapshot-file",str(root/"missing-frame"),
            "--data-socket", str(root / "data.sock"),
            "--encoded-file",str(root/"encoded.nve"),"--encoding-config-file",str(cls.config_root/"encoding.json"),
            "--stereo-control-file",str(cls.config_root/"stereo.json"),
            "--exposure-control-file",str(cls.config_root/"exposure.json"),
            "--transport-file",str(root/"missing-transport"),"--system-agent-socket",str(root/"agent.sock"),"--engineering-interface","lo"],
            stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,text=True)
        context=ssl.create_default_context(); context.check_hostname=False; context.verify_mode=ssl.CERT_NONE
        cls.cookies=http.cookiejar.CookieJar()
        cls.client=urllib.request.build_opener(urllib.request.HTTPCookieProcessor(cls.cookies),urllib.request.HTTPSHandler(context=context))
        cls.origin=f"https://127.0.0.1:{cls.port}"
        for _ in range(50):
            try:
                cls.open("/"); break
            except Exception:
                if cls.process.poll() is not None:
                    raise RuntimeError(cls.process.stderr.read())
                time.sleep(.1)
        else: raise RuntimeError("gateway did not become ready")

    @classmethod
    def tearDownClass(cls):
        # Leave TLS handshake and WebSocket readers idle during service stop.
        stalled = socket.create_connection(("127.0.0.1", cls.port), timeout=2)
        client = NuvoClient(cls.origin, ca_file=cls.certificate)
        client.claim(cls.token)
        idle_stream = client.frames().__enter__()
        cls.process.terminate()
        try: cls.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            cls.process.kill(); cls.process.wait(timeout=5)
            stalled.close(); idle_stream.close(); cls.local.close(); cls.agent.close(); cls.temporary.cleanup()
            raise AssertionError("gateway shutdown blocked on idle TLS/WebSocket clients")
        else:
            if cls.process.returncode != 0:
                raise AssertionError(cls.process.stderr.read())
        stalled.close()
        idle_stream.close()
        cls.local.close()
        cls.agent.close()
        cls.temporary.cleanup()

    @classmethod
    def open(cls,path,method="GET",headers=None,body=None):
        request=urllib.request.Request(cls.origin+path,data=body,headers=headers or {},method=method)
        return cls.client.open(request,timeout=5)

    def test_local_ui_assets(self):
        for asset in ("app.js", "device-settings.js", "frame-codec.js"):
            with self.open("/" + asset) as response:
                self.assertEqual(response.headers.get_content_type(), "text/javascript")
                self.assertEqual(response.read(), (self.web / asset).read_bytes())
        for asset in ("styles.css",):
            with self.open("/" + asset) as response:
                self.assertEqual(response.headers.get_content_type(), "text/css")
                self.assertEqual(response.read(), (self.web / asset).read_bytes())
        with self.open("/index.html") as response:
            self.assertEqual(response.read(), (self.web / "index.html").read_bytes())
        # Stale prototype files in an upgraded web root must stay inaccessible.
        for asset in ("pipeline.html", "pipeline-model.js", "pipeline.js", "pipeline-host.js",
                      "pipeline-canvas.js", "pipeline.css", "pipeline-canvas.css", "pipeline-licenses.txt"):
            with self.assertRaises(urllib.error.HTTPError) as error:
                self.open("/" + asset)
            self.assertEqual(error.exception.code, 404, asset)
        # Device typography must work offline under the existing self-only CSP.
        for weight in (400,500,600):
            asset=f"assets/fonts/hanken-grotesk-{weight}.ttf"
            with self.open("/"+asset) as response:
                self.assertEqual(response.headers.get_content_type(),"font/ttf")
                self.assertEqual(response.read(),(self.web/asset).read_bytes())
                self.assertIn("default-src 'self'",response.headers["Content-Security-Policy"])
        # Retired demo routes stay inaccessible even if old files survive upgrade.
        for scene in ("indoor", "outdoor"):
            for product in ("photo", "depth"):
                asset = f"assets/benchmark/diode-{scene}-{product}.jpg"
                path = self.web / asset; path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"stale demo fixture")
                with self.assertRaises(urllib.error.HTTPError) as retired:
                    self.open("/" + asset)
                self.assertEqual(retired.exception.code, 404)
        with self.assertRaises(urllib.error.HTTPError) as unknown:
            self.open("/assets/fonts/unknown.ttf")
        self.assertEqual(unknown.exception.code,404)

    def wait_demand(self, expected, timeout=1.5):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.local.demand() == expected:
                return
            time.sleep(.02)
        self.assertEqual(self.local.demand(), expected)

    def write_encoded_fixture(self, value):
        temporary = self.local.encoded_path.with_suffix(".fixture")
        temporary.write_bytes(value); temporary.replace(self.local.encoded_path)
        with self.local.condition:
            self.assertTrue(self.local.condition.wait_for(lambda: self.local.encoded_bytes == value, timeout=1))
            return self.local.encoded_cursor

    def wait_encoded_cursor(self, cursor, connection=None):
        with self.local.condition:
            self.assertTrue(self.local.condition.wait_for(lambda: any(
                after == cursor and (connection is None or peer is connection)
                for peer, after in self.local.encoded_waits), timeout=1))
            return next(peer for peer, after in reversed(self.local.encoded_waits)
                        if after == cursor and (connection is None or peer is connection))

    def fetch_encoded_fixture(self, websocket, result):
        try:
            if not websocket:
                with self.open("/api/v1/encoded/latest?cadence_hz=5",
                               headers={"Authorization": f"Bearer {self.token}"}) as response:
                    result.append(decode_encoded_frame(response.read()))
            else:
                client = NuvoClient(self.origin, ca_file=self.certificate)
                client._cookies, client._csrf = self.cookies, "existing-test-session"
                with _FrameStream(client, "/api/v1/encoded/stream?cadence_hz=5") as stream:
                    first, second = stream._read_exact(2)
                    self.assertEqual(first, 0x82)
                    length = second & 127
                    if length == 126: length = struct.unpack(">H", stream._read_exact(2))[0]
                    elif length == 127: length = struct.unpack(">Q", stream._read_exact(8))[0]
                    result.append(decode_encoded_frame(stream._read_exact(length)))
        except Exception as error:
            result.append(error)

    def test_encoded_stale_cache_cannot_satisfy_initial_http_or_websocket_wait(self):
        self.wait_demand((0, 0, 0))
        self.local.auto_encode = False
        time.sleep(.1)
        try:
            self.write_encoded_fixture(encoded_frame(capture_time_ns=time.clock_gettime_ns(time.CLOCK_BOOTTIME)-10_000_000_000))
            for websocket in (False, True):
                with self.subTest(websocket=websocket):
                    result = []
                    self.fetch_encoded_fixture(websocket, result)
                    self.assertEqual(len(result), 1)
                    if websocket:
                        self.assertIsInstance(result[0], ConnectionError)
                        self.assertIn("503", str(result[0]))
                    else:
                        self.assertIsInstance(result[0], urllib.error.HTTPError)
                        self.assertEqual(result[0].code, 503)
                    self.wait_demand((0, 0, 0))
        finally:
            self.local.auto_encode = True

    def test_encoded_stale_cache_recovers_on_same_demand_and_advanced_cursor(self):
        self.wait_demand((0, 0, 0))
        self.local.auto_encode = False
        time.sleep(.1)
        consumer = None
        try:
            for websocket in (False, True):
                with self.subTest(websocket=websocket):
                    stale_cursor = self.write_encoded_fixture(encoded_frame(capture_time_ns=time.clock_gettime_ns(time.CLOCK_BOOTTIME)-10_000_000_000))
                    updates = len(self.local.subscription_updates)
                    result = []
                    consumer = threading.Thread(target=self.fetch_encoded_fixture, args=(websocket, result))
                    consumer.start()
                    connection = self.wait_encoded_cursor(stale_cursor)
                    time.sleep(.25)
                    self.assertTrue(consumer.is_alive(), "old encoded capture satisfied the first-frame wait")
                    self.assertEqual(len(self.local.subscription_updates)-updates, 1, "stale preview reconnected instead of preserving demand")
                    self.wait_demand((0, 0, 5))
                    fresh = encoded_frame(202 if websocket else 201, **(
                        {} if websocket else {"device_uuid": "01" + "00" * 15, "run_uuid": "02" + "00" * 15}))
                    fresh_cursor = self.write_encoded_fixture(fresh)
                    consumer.join(timeout=2)
                    self.assertFalse(consumer.is_alive())
                    self.assertEqual(len(result), 1)
                    if isinstance(result[0], Exception): raise result[0]
                    self.assertEqual(result[0].capture_sequence, 202 if websocket else 201)
                    self.assertEqual(bytes(result[0].data), bytes(decode_encoded_frame(fresh).data))
                    self.assertNotEqual(fresh_cursor, stale_cursor)
                    # The same registered local connection delivered the fresh
                    # packet after waiting with the rejected packet's cursor.
                    self.assertIn((connection, fresh_cursor), self.local.encoded_delivery_peers)
                    self.assertEqual(len(self.local.subscription_updates)-updates, 1)
                    self.wait_demand((0, 0, 0))
        finally:
            self.local.auto_encode = True
            if consumer: consumer.join(timeout=5)

    def test_encoded_malformed_envelopes_and_metadata_are_not_published(self):
        def header(offset, fmt, value):
            result = bytearray(encoded_frame()); struct.pack_into(fmt, result, offset, value); return bytes(result)
        malformed = [
            ("version", lambda: header(4, ">H", 2)),
            ("flags", lambda: header(6, ">H", 2)),
            ("metadata bound", lambda: header(8, ">I", 65537)),
            ("payload bound", lambda: header(12, ">Q", 0)),
            ("stream identity", lambda: header(20, ">Q", 0)),
            ("reserved", lambda: header(28, ">I", 1)),
            ("schema type", lambda: encoded_frame(schema_version="1")),
            ("capture identity", lambda: encoded_frame(run_uuid="00"*16)),
            ("zero canonical identity", lambda: encoded_frame(run_uuid="00000000-0000-0000-0000-000000000000")),
            ("invalid UUID separators", lambda: encoded_frame(run_uuid="02000000_0000-0000-0000-000000000000")),
            ("clock", lambda: encoded_frame(capture_clock=0)),
            ("future capture", lambda: encoded_frame(capture_time_ns=time.clock_gettime_ns(time.CLOCK_BOOTTIME)+10_000_000_000)),
            ("dimensions", lambda: encoded_frame(width=0)),
            ("codec", lambda: encoded_frame(codec="unknown")),
            ("mime", lambda: encoded_frame(mime_type="text/plain")),
            ("keyframe type", lambda: encoded_frame(keyframe="true")),
            ("keyframe agreement", lambda: encoded_frame(keyframe=False)),
        ]
        self.wait_demand((0, 0, 0))
        self.local.auto_encode = False
        time.sleep(.1)
        consumer = None
        try:
            cursor = self.write_encoded_fixture(encoded_frame(capture_time_ns=1))
            result = []
            consumer = threading.Thread(target=self.fetch_encoded_fixture, args=(False, result)); consumer.start()
            connection = self.wait_encoded_cursor(cursor)
            for name, make_packet in malformed:
                with self.subTest(field=name):
                    cursor = self.write_encoded_fixture(make_packet())
                    self.wait_encoded_cursor(cursor, connection)
                    self.assertTrue(consumer.is_alive(), f"invalid {name} satisfied the first-frame wait")
            self.write_encoded_fixture(encoded_frame(301))
            consumer.join(timeout=2)
            self.assertFalse(consumer.is_alive())
            self.assertEqual(len(result), 1)
            if isinstance(result[0], Exception): raise result[0]
            self.assertEqual(result[0].capture_sequence, 301)
        finally:
            self.local.auto_encode = True
            if consumer: consumer.join(timeout=5)
        self.wait_demand((0, 0, 0))

    def test_a_idle_gateway_does_not_fetch_raw_or_request_encoding(self):
        self.wait_demand((0, 0, 0))
        time.sleep(.3)
        before = self.local.requests
        with self.open("/api/v1/capabilities", headers={"Authorization": f"Bearer {self.token}"}) as response:
            capabilities = json.load(response)
        self.assertEqual(capabilities["connection"]["kind"], "loopback")
        self.assertFalse(capabilities["connection"]["budget_is_measured_capacity"])
        self.assertEqual(len(capabilities["products"]), 14)
        expected_encoded_sources = {kind for kind, _, _ in PRODUCTS} - {"confidence", "validity"} | {"left_color", "right_color"}
        self.assertEqual(set(capabilities["encoded_sources"]), expected_encoded_sources)
        for source in capabilities["encoded_sources"]:
            self.agent.module.validate_encoding({**self.encoding, "source": source})
        self.assertIn("confidence", capabilities["products"])
        self.assertIn("validity", capabilities["products"])
        self.assertTrue(capabilities["stream_admission"]["demand_driven"])
        time.sleep(.3)
        self.assertEqual(self.local.requests, before)

    def test_demand_subset_union_and_disconnect_release(self):
        client = NuvoClient(self.origin, ca_file=self.certificate); client.claim(self.token)
        with client.frames(cadence_hz=5, products=["left_mono"], delivery="best_effort") as mono:
            self.assertEqual(set(next(mono).products), {"left_mono"})
            self.wait_demand((1 << 6, 5, 0))
            with client.frames(cadence_hz=10, products=["right_rgb", "rectified_stereo_rgb"], delivery="best_effort") as color:
                frame = next(color)
                self.assertEqual(set(frame.products), {"right_rgb", "rectified_stereo_rgb"})
                self.assertEqual(frame.products["right_rgb"].format, "rgb8")
                self.assertEqual(bytes(frame.products["right_rgb"].data), bytes((11, 22, 33)))
                self.assertEqual(frame.products["rectified_stereo_rgb"].width, 2)
                self.wait_demand(((1 << 6) | (1 << 9) | (1 << 13), 10, 0))
                self.local.publish(7)
                selected = next(mono)
                self.assertEqual(selected.metadata["selected_products"], 1 << 6)
                self.assertEqual(selected.metadata["available_products"], (1 << 6) | (1 << 9) | (1 << 13))
            self.wait_demand((1 << 6, 5, 0))
        self.wait_demand((0, 0, 0))
        time.sleep(.3); before = self.local.requests; time.sleep(.3)
        self.assertEqual(self.local.requests, before)

    def test_demand_snapshot_new_products_and_bounded_fresh_wait(self):
        with self.open("/api/v1/frame/latest?products=left_rgb,rectified_stereo&cadence_hz=5&delivery=best_effort",
                       headers={"Authorization": f"Bearer {self.token}"}) as response:
            frame = response.read()
            metadata_bytes = struct.unpack_from(">I", frame, 12)[0]
            metadata = json.loads(frame[32:32 + metadata_bytes])
            self.assertEqual([p["kind"] for p in metadata["products"]], ["left_rgb", "rectified_stereo"])
            self.assertEqual(metadata["products"][0]["format"], "rgb8")
            self.assertEqual(metadata["products"][1]["width"], 2)
        self.wait_demand((0, 0, 0))
        self.local.stale_frames = True
        try:
            start = time.monotonic()
            with self.assertRaises(urllib.error.HTTPError) as missing:
                self.open("/api/v1/frame/latest?products=left_mono&cadence_hz=5&delivery=best_effort",
                          headers={"Authorization": f"Bearer {self.token}"})
            self.assertEqual(missing.exception.code, 503)
            self.assertGreaterEqual(time.monotonic() - start, 2.5)
            self.assertLess(time.monotonic() - start, 4)
        finally:
            self.local.stale_frames = False
        self.wait_demand((0, 0, 0))

    def test_stale_cached_frame_keeps_demand_until_fresh_capture(self):
        self.wait_demand((0, 0, 0))
        previous = self.local.parameters
        self.local.stale_frames = True
        self.local.publish(100)
        updates = len(self.local.subscription_updates)
        stale_deliveries = self.local.stale_deliveries
        result = []

        def fetch():
            try:
                with self.open("/api/v1/frame/latest?products=left_mono&cadence_hz=5&delivery=best_effort",
                               headers={"Authorization": f"Bearer {self.token}"}) as response:
                    result.append(response.read())
            except Exception as error:
                result.append(error)

        consumer = threading.Thread(target=fetch)
        consumer.start()
        try:
            with self.local.condition:
                self.assertTrue(self.local.condition.wait_for(
                    lambda: self.local.stale_deliveries > stale_deliveries, timeout=1.5))
            # An idle producer needs time to observe demand. During that wait
            # the gateway must retain its connection instead of repeatedly
            # reconnecting to the cached capture with a reset cursor.
            time.sleep(.35)
            self.assertEqual(len(self.local.subscription_updates) - updates, 1)
            self.wait_demand((1 << 6, 5, 0))
            self.local.stale_frames = False
            self.local.publish(101)
            consumer.join(timeout=2)
            self.assertFalse(consumer.is_alive())
            self.assertEqual(len(result), 1)
            if isinstance(result[0], Exception):
                raise result[0]
            self.assertEqual(struct.unpack_from(">Q", result[0], 24)[0], 101)
        finally:
            self.local.stale_frames = False
            self.local.publish(*previous)
            consumer.join(timeout=5)
        self.wait_demand((0, 0, 0))

    def test_configuration_etags_pending_receipt_and_preconditions(self):
        with self.open("/api/v1/session","POST",{"Authorization":f"Bearer {self.token}"},b"") as response:
            csrf=json.load(response)["csrf"]
        for kind in ("encoding","stereo","exposure"):
            path="/api/v1/"+kind
            with self.open(path) as response:
                original=response.read(); etag=response.headers["ETag"]
                self.assertEqual(etag,'"'+hashlib.sha256(original).hexdigest()+'"')
                self.assertEqual(response.headers["X-Nuvo-Configuration-State"],"requested")
            headers={"Content-Type":"application/json","X-Nuvo-CSRF":csrf}
            with self.assertRaises(urllib.error.HTTPError) as missing:
                self.open(path,"POST",headers,original)
            self.assertEqual(missing.exception.code,428)
            with self.assertRaises(urllib.error.HTTPError) as stale:
                self.open(path,"POST",dict(headers,**{"If-Match":'"'+'0'*64+'"'}),original)
            self.assertEqual(stale.exception.code,412)
            with self.open(path,"POST",dict(headers,**{"If-Match":etag}),original) as response:
                saved=response.read()
                self.assertEqual(response.status,202)
                self.assertEqual(json.loads(saved),json.loads(original))
                self.assertEqual(response.headers["ETag"],'"'+hashlib.sha256(saved).hexdigest()+'"')
                self.assertEqual(response.headers["X-Nuvo-Configuration-State"],"pending")
        # Force a writer between the gateway's read and privileged CAS check.
        path="/api/v1/stereo"
        with self.open(path) as response:
            original=response.read(); etag=response.headers["ETag"]
        changed=dict(json.loads(original),uniqueness_ratio=3)
        self.agent.before_write=lambda:self.agent.module.set_stereo(self.agent.root,changed)
        with self.assertRaises(urllib.error.HTTPError) as raced:
            self.open(path,"POST",dict(headers,**{"If-Match":etag}),original)
        self.assertEqual(raced.exception.code,412)
        self.assertEqual(json.loads((self.config_root/"stereo.json").read_bytes()),changed)
        self.agent.module.set_stereo(self.agent.root,json.loads(original))

    def test_configuration_first_snapshot_observes_enabled_mode(self):
        with self.open("/api/v1/session", "POST", {"Authorization": f"Bearer {self.token}"}, b"") as response:
            csrf = json.load(response)["csrf"]
        with self.open("/api/v1/encoding") as response:
            original = json.load(response)

        def set_mode(mode):
            with self.open("/api/v1/encoding") as response:
                etag = response.headers["ETag"]
            headers = {"Content-Type": "application/json", "X-Nuvo-CSRF": csrf, "If-Match": etag}
            with self.open("/api/v1/encoding", "POST", headers,
                           json.dumps({**original, "mode": mode}).encode()) as response:
                self.assertEqual(response.status, 202)
                response.read()

        try:
            # Let the worker observe a disabled mode, then issue each first
            # snapshot immediately after the real privileged writer's receipt.
            # It must acquire native demand without a retry or polling delay.
            for _ in range(4):
                set_mode("raw"); self.wait_demand((0, 0, 0)); time.sleep(.25)
                updates = len(self.local.subscription_updates)
                set_mode("both")
                with self.open("/api/v1/encoded/latest") as response:
                    self.assertEqual(response.status, 200)
                    self.assertTrue(response.read().startswith(b"NVE1"))
                self.wait_demand((0, 0, 0))
                self.assertIn((0, 0, 1), self.local.subscription_updates[updates:])

                set_mode("encoded"); time.sleep(.25)
                updates = len(self.local.subscription_updates)
                set_mode("raw")
                with self.open("/api/v1/frame/latest?products=left_mono&cadence_hz=5&delivery=best_effort") as response:
                    frame = response.read()
                    length = struct.unpack_from(">I", frame, 12)[0]
                    metadata = json.loads(frame[32:32 + length])
                    self.assertEqual([product["kind"] for product in metadata["products"]], ["left_mono"])
                self.wait_demand((0, 0, 0))
                self.assertIn((1 << 6, 5, 0), self.local.subscription_updates[updates:])
        finally:
            set_mode(original["mode"])

    def test_exact_best_effort_interface_rejects_without_source_wait(self):
        self.wait_demand((0, 0, 0))
        client = NuvoClient(self.origin, ca_file=self.certificate); client.claim(self.token)
        self.local.serve_frames = False
        updates, requests = len(self.local.subscription_updates), self.local.requests
        try:
            start = time.monotonic()
            with self.assertRaises(urllib.error.HTTPError) as rejected:
                self.open("/api/v1/frame/latest?products=left_mono&cadence_hz=5&delivery=exact",
                          headers={"Authorization": f"Bearer {self.token}"})
            self.assertEqual(rejected.exception.code, 409)
            with self.assertRaisesRegex(ConnectionError, "409"):
                with client.frames(cadence_hz=5, products=["left_mono"], delivery="exact"):
                    self.fail("exact subscription admitted on engineering best-effort interface")
            self.assertLess(time.monotonic() - start, 1.5)
            self.assertEqual(len(self.local.subscription_updates), updates)
            self.assertEqual(self.local.requests, requests)
            self.wait_demand((0, 0, 0))
        finally:
            self.local.serve_frames = True

    def test_claim_restore_csrf_and_security_headers(self):
        with self.assertRaises(urllib.error.HTTPError) as unauthorized:
            self.open("/api/v1/health")
        self.assertEqual(unauthorized.exception.code,401)
        with self.assertRaises(urllib.error.HTTPError) as prefixed_cookie:
            self.open("/api/v1/health",headers={"Cookie":"not_nuvo_session=forged"})
        self.assertEqual(prefixed_cookie.exception.code,401)
        with self.open("/api/v1/session","POST",{"Authorization":f"Bearer {self.token}"},b"") as response:
            session=json.load(response); cookie=response.headers["Set-Cookie"]
        self.assertEqual(session["role"],"admin"); self.assertEqual(len(session["csrf"]),64)
        for attribute in ("Secure","HttpOnly","SameSite=Strict"): self.assertIn(attribute,cookie)
        with self.open("/api/v1/session") as response:
            self.assertEqual(json.load(response)["csrf"],session["csrf"])
        body=b'{"preference":"usb"}'
        with self.assertRaises(urllib.error.HTTPError) as missing_csrf:
            self.open("/api/v1/transport/stage","POST",{"Content-Type":"application/json"},body)
        self.assertEqual(missing_csrf.exception.code,403)
        with self.assertRaises(urllib.error.HTTPError) as no_agent:
            self.open("/api/v1/transport/stage","POST",{"Content-Type":"application/json","X-Nuvo-CSRF":session["csrf"]},body)
        self.assertEqual(no_agent.exception.code,400)
        with self.open("/") as response:
            self.assertIn("default-src 'self'",response.headers["Content-Security-Policy"])
            self.assertEqual(response.headers["X-Content-Type-Options"],"nosniff")
            self.assertEqual(response.headers["Cache-Control"],"no-store")
        with self.open("/api/v1/encoding") as response:
            self.assertEqual(json.load(response),self.encoding)
        with self.open("/api/v1/stereo") as response:
            self.assertEqual(json.load(response),self.stereo)
        with self.open("/api/v1/exposure") as response:
            self.assertEqual(json.load(response),self.exposure)
        with self.assertRaises(urllib.error.HTTPError) as stereo_without_csrf:
            self.open("/api/v1/stereo","POST",{"Content-Type":"application/json"},
                      json.dumps(self.stereo).encode())
        self.assertEqual(stereo_without_csrf.exception.code,403)
        with self.assertRaises(urllib.error.HTTPError) as exposure_without_csrf:
            self.open("/api/v1/exposure","POST",{"Content-Type":"application/json"},
                      json.dumps(self.exposure).encode())
        self.assertEqual(exposure_without_csrf.exception.code,403)
        with self.open("/api/v1/encoded/latest") as response:
            self.assertEqual(response.headers["Content-Type"],"application/vnd.nuvo.encoded-image")
            self.assertTrue(response.read().startswith(b"NVE1"))

    def test_raw_local_source_fanout_and_restart_identity(self):
        client = NuvoClient(self.origin, ca_file=self.certificate)
        client.claim(self.token)
        before = self.local.requests
        with client.frames() as left, client.frames() as right:
            first = next(left)
            self.assertEqual(first.metadata["capture_sequence"], 7)
            self.assertEqual(next(right).metadata["run_uuid"], first.metadata["run_uuid"])
            with self.open("/api/v1/frame/latest", headers={"Authorization": f"Bearer {self.token}"}) as response:
                value=response.read()
                metadata_length=struct.unpack_from(">I",value,12)[0]
                capture_time=json.loads(value[32:32+metadata_length])["capture_time_ns"]
                self.assertEqual(value, raw_frame(7,capture_time_ns=capture_time))
            self.local.publish(8)
            self.assertEqual(next(left).metadata["capture_sequence"], 8)
            self.assertEqual(next(right).metadata["capture_sequence"], 8)
            self.local.publish(8, run=3)
            self.assertTrue(next(left).metadata["run_uuid"].startswith("03"))
            self.assertTrue(next(right).metadata["run_uuid"].startswith("03"))
        # The gateway owns one source reader regardless of the number of clients.
        self.assertLess(self.local.requests - before, 25)

    def test_snapshot_writers_share_websocket_memory_budget(self):
        arguments = list(self.process.args)
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        arguments[arguments.index("--port") + 1] = str(port)
        # Keep byte-rate capacity ample so this exercises the shared 64 MiB
        # frame-memory limit rather than the independent network resource cap.
        arguments.extend(["--engineering-byte-budget", str(1024 ** 3)])
        process = subprocess.Popen(arguments, stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE, text=True)
        stalled = []
        try:
            self.local.publish(9, run=3, width=768, height=768)
            client = NuvoClient(f"https://127.0.0.1:{port}", ca_file=self.certificate)
            for _ in range(30):
                try:
                    client.claim(self.token)
                    break
                except (ConnectionError, urllib.error.URLError):
                    if process.poll() is not None:
                        self.fail(process.stderr.read())
                    time.sleep(.1)
            else:
                self.fail("snapshot budget fixture did not start")
            time.sleep(.3)

            expected_length = len(raw_frame(9, 3, width=768, height=768))
            for _ in range(4):
                connection = socket.create_connection(("127.0.0.1", port), timeout=2)
                connection.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
                connection = client._context.wrap_socket(connection, server_hostname="127.0.0.1")
                stalled.append(connection)
                connection.sendall((f"GET /api/v1/frame/latest HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n"
                    f"Authorization: Bearer {self.token}\r\nConnection: close\r\n\r\n").encode())
                header = bytearray()
                while not header.endswith(b"\r\n\r\n"):
                    header.extend(connection.recv(1))
                    self.assertLess(len(header), 16384)
                self.assertTrue(header.startswith(b"HTTP/1.1 200"))
                self.assertIn(f"Content-Length: {expected_length}".encode(), header)
            # Four reserved 6.75 MiB responses (two buffers each) fit. A fifth
            # HTTP or WebSocket response must reject before allocating its copy.
            request = urllib.request.Request(client.base_url + "/api/v1/frame/latest",
                headers={"Authorization": f"Bearer {self.token}"})
            with self.assertRaises(urllib.error.HTTPError) as exhausted:
                client._opener.open(request, timeout=2)
            self.assertEqual(exhausted.exception.code, 409)
            with self.assertRaisesRegex(ConnectionError, "409"):
                with client.frames():
                    self.fail("HTTP in-flight memory was not shared with WebSocket admission")
            stalled.pop().close()
            for _ in range(20):
                try:
                    with client._opener.open(request, timeout=2) as response:
                        self.assertEqual(response.status, 200)
                    break
                except urllib.error.HTTPError as error:
                    self.assertEqual(error.code, 409)
                    time.sleep(.05)
            else:
                self.fail("HTTP disconnect did not release its frame-memory reservation")
        finally:
            for connection in stalled:
                connection.close()
            process.terminate()
            process.wait(timeout=3)
            self.assertEqual(process.returncode, 0, process.stderr.read())
            process.stderr.close()
            # A resized publication needs a new capture identity; reusing9
            # correctly leaves the wait-for-next source on its earlier frame.
            self.local.publish(10, run=3)
            time.sleep(.3)

    def test_snapshot_single_response_default_rate_and_repeat_budget(self):
        arguments = list(self.process.args)
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as reservation:
            reservation.bind(("127.0.0.1", 0)); port = reservation.getsockname()[1]
        arguments[arguments.index("--port") + 1] = str(port)
        # Loopback engineering exercises the real HTTP path with the same
        # 8-MiB/s resource cap used by normal WiFi in the admission core test.
        arguments.extend(["--engineering-byte-budget", str(8 * 1024 * 1024)])
        process = subprocess.Popen(arguments, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        try:
            self.wait_demand((0, 0, 0)); self.local.publish(9, run=3, width=832, height=832)
            client = NuvoClient(f"https://127.0.0.1:{port}", ca_file=self.certificate)
            for _ in range(30):
                try: client.claim(self.token); break
                except (ConnectionError, urllib.error.URLError):
                    if process.poll() is not None: self.fail(process.stderr.read())
                    time.sleep(.1)
            else: self.fail("snapshot rate fixture did not start")
            request = urllib.request.Request(client.base_url + "/api/v1/frame/latest",
                headers={"Authorization": f"Bearer {self.token}"})
            updates_before = len(self.local.subscription_updates)
            with client._opener.open(request, timeout=5) as response:
                self.assertEqual(response.status, 200)
                value = response.read()
            metadata_bytes = struct.unpack_from(">I", value, 12)[0]
            self.assertEqual(len(json.loads(value[32:32 + metadata_bytes])["products"]), 6)
            self.assertIn((63, 1, 0), self.local.subscription_updates[updates_before:])
            self.wait_demand((0, 0, 0))
            # The next default snapshot has a low rate reservation too, but
            # the full-byte token charge must reject an immediate repeat.
            with self.assertRaises(urllib.error.HTTPError) as repeat:
                client._opener.open(request, timeout=5)
            self.assertEqual(repeat.exception.code, 409)
            self.wait_demand((0, 0, 0))
            with self.assertRaisesRegex(ConnectionError, "409"):
                with client.frames(): self.fail("default ten-Hz stream fit a one-shot byte window")
            self.wait_demand((0, 0, 0))
            query = "?cadence_hz=10&products=depth,confidence,validity&delivery=best_effort"
            with self.assertRaises(urllib.error.HTTPError) as explicit_rate:
                client._opener.open(urllib.request.Request(client.base_url + "/api/v1/frame/latest" + query,
                    headers={"Authorization": f"Bearer {self.token}"}), timeout=5)
            self.assertEqual(explicit_rate.exception.code, 409)
        finally:
            process.terminate(); process.wait(timeout=3)
            self.assertEqual(process.returncode, 0, process.stderr.read()); process.stderr.close()
            self.local.publish(10, run=3); self.wait_demand((0, 0, 0))

    def test_mixed_image_and_matcher_subscriptions_keep_separate_cadences(self):
        client = NuvoClient(self.origin, ca_file=self.certificate)
        client._cookies = self.cookies
        client._csrf = "existing-test-session"
        with client.frames(cadence_hz=1, products=["depth", "left_mono"]) as depth, \
             client.frames(cadence_hz=10, products=["left_mono"]) as mono:
            first = next(depth)
            next(mono)
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                with self.local.condition:
                    demands = list(self.local.subscriptions.values())
                if ((1 << 3) | (1 << 6), 1.0, 0.0) in demands and (1 << 6, 10.0, 0.0) in demands:
                    break
                time.sleep(.01)
            else:
                self.fail(f"mixed demand collapsed into one cadence: {demands}")
            for sequence in range(8, 19):
                self.local.publish(sequence)
                time.sleep(.1)
            fast = next(mono)
            slow = next(depth)
            self.assertEqual(set(fast.products), {"left_mono"})
            self.assertEqual(set(slow.products), {"depth", "left_mono"})
            self.assertGreater(slow.capture_sequence, first.capture_sequence)
            self.assertEqual(slow.run_uuid, fast.run_uuid)
            self.local.publish(19, run=3)
            # Already transmitted frames can remain in the network buffer.
            for stream in (mono, depth):
                for _ in range(20):
                    if next(stream).run_uuid.startswith("03"):
                        break
                else:
                    self.fail("mixed subscriber did not observe daemon restart")
        self.wait_demand((0, 0, 0))
        self.local.publish(7)

    def test_stream_products_and_cadence_are_explicit_engineering_limits(self):
        client = NuvoClient(self.origin, ca_file=self.certificate)
        client.claim(self.token)
        with client.frames(cadence_hz=2, products=["depth", "confidence", "validity"]) as stream:
            first = next(stream)
            self.assertEqual(stream.qualification, "engineering-best-effort")
            self.assertEqual(set(first.products), {"depth", "confidence", "validity"})
            self.assertEqual(len(first.products["depth"].data), 4)
            start = time.monotonic()
            # Several complete source frames arrive before the next allowed send.
            for sequence in range(10, 20):
                self.local.publish(sequence, run=3)
                time.sleep(.02)
            second = next(stream)
            self.assertGreaterEqual(time.monotonic() - start, .4)
            self.assertEqual(second.capture_sequence, 19)
            self.assertEqual(second.run_uuid, first.run_uuid)
            self.assertEqual(set(second.products), set(first.products))

    def test_subset_views_reserve_the_complete_backing_union(self):
        arguments = list(self.process.args)
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as reservation:
            reservation.bind(("127.0.0.1", 0)); port = reservation.getsockname()[1]
        arguments[arguments.index("--port") + 1] = str(port)
        arguments.extend(["--engineering-byte-budget", str(1024 ** 3)])
        process = subprocess.Popen(arguments, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        try:
            self.wait_demand((0, 0, 0)); self.local.publish(50, run=3, width=720, height=540)
            client = NuvoClient(f"https://127.0.0.1:{port}", ca_file=self.certificate)
            for _ in range(30):
                try: client.claim(self.token); break
                except (ConnectionError, urllib.error.URLError):
                    if process.poll() is not None: self.fail(process.stderr.read())
                    time.sleep(.1)
            # The first consumer's union owns about 15 MiB. Each selected
            # view can retain two such owners despite sending very few bytes.
            with client.frames(cadence_hz=1, products=[p[0] for p in PRODUCTS], delivery="best_effort") as complete:
                with client.frames(cadence_hz=1, products=["depth"], delivery="best_effort") as depth:
                    self.assertEqual(set(next(depth).products), {"depth"})
                    with self.assertRaisesRegex(ConnectionError, "409"):
                        with client.frames(cadence_hz=1, products=["confidence"], delivery="best_effort"):
                            self.fail("small view bypassed retained-source memory admission")
        finally:
            process.terminate(); process.wait(timeout=3)
            self.assertEqual(process.returncode, 0, process.stderr.read()); process.stderr.close()
            self.local.publish(51, run=3)
            self.wait_demand((0, 0, 0))

    def test_stream_ping_and_close_are_processed_while_source_idle(self):
        client = NuvoClient(self.origin, ca_file=self.certificate)
        client.claim(self.token)
        with client.frames() as stream:
            next(stream)
            stream.ping(b"nuvo")
            stream._set_deadline()
            first, second = stream._read_exact(2)
            self.assertEqual(first, 0x8A)
            self.assertEqual(second, 4)
            self.assertEqual(stream._read_exact(4), b"nuvo")
            stream._send_control(8, struct.pack(">H", 1000))
            first, second = stream._read_exact(2)
            self.assertEqual(first, 0x88)
            self.assertEqual(struct.unpack(">H", stream._read_exact(second))[0], 1000)

    def test_stream_raw_disabled_mode_and_encoded_cache_removal(self):
        encoded = Path(self.process.args[self.process.args.index("--encoded-file") + 1])
        configuration = self.config_root / "encoding.json"
        previous = configuration.read_bytes()
        try:
            encoded.write_bytes(encoded_frame())
            time.sleep(.2)
            with self.open("/api/v1/encoded/latest", headers={"Authorization": f"Bearer {self.token}"}) as response:
                self.assertTrue(response.read().startswith(b"NVE1"))
            self.local.auto_encode = False
            encoded.unlink()
            time.sleep(.2)
            with self.assertRaises(urllib.error.HTTPError) as missing:
                self.open("/api/v1/encoded/latest", headers={"Authorization": f"Bearer {self.token}"})
            self.assertEqual(missing.exception.code, 503)
            requested = json.loads(previous)
            requested["mode"] = "encoded"
            configuration.write_text(json.dumps(requested))
            time.sleep(.4)

            with self.assertRaises(urllib.error.HTTPError) as disabled:
                self.open("/api/v1/frame/latest", headers={"Authorization": f"Bearer {self.token}"})
            self.assertEqual(disabled.exception.code, 503)
            client = NuvoClient(self.origin, ca_file=self.certificate)
            client.claim(self.token)
            with self.assertRaisesRegex(ConnectionError, "503"):
                with client.frames():
                    self.fail("raw stream remained available in encoded-only mode")
        finally:
            self.local.auto_encode = True
            configuration.write_bytes(previous)
            encoded.write_bytes(encoded_frame())
            self.local.publish(25, run=3)
            time.sleep(.4)

    def test_stream_qualified_exact_admission_and_capacity(self):
        if not self.ethernet:
            self.skipTest("test host has no existing Ethernet IPv4 interface")
        interface, address = self.ethernet
        arguments = list(self.process.args)
        index = arguments.index("--engineering-interface")
        del arguments[index:index + 2]
        arguments[arguments.index("--address") + 1] = address
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as reservation:
            reservation.bind((address, 0))
            port = reservation.getsockname()[1]
        arguments[arguments.index("--port") + 1] = str(port)
        evidence = dict(schema_version=1, enabled=True,
            measurement_id="synthetic-integration-fixture-no-hardware-qualification",
            source=dict(stereo_applied_revision="ab" * 32,
                calibration_sha256="01" + "00" * 31, configuration_epoch=1,
                minimum_cadence_hz=20, maximum_frame_bytes=100000),
            ethernet=dict(enabled=True, interface=interface, payload_bytes_per_second=1000000,
                maximum_frame_bytes=100000, maximum_sessions=2),
            usb_ethernet=dict(enabled=False))
        path = Path(self.temporary.name) / "qualified-fixture.json"
        path.write_text(json.dumps(evidence))
        arguments.extend(["--qualification-file", str(path)])
        process = subprocess.Popen(arguments, stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE, text=True)
        try:
            client = NuvoClient(f"https://{address}:{port}", ca_file=self.certificate)
            # The local fixture must bypass developer-machine HTTP proxies.
            client._opener = urllib.request.build_opener(urllib.request.ProxyHandler({}),
                urllib.request.HTTPCookieProcessor(client._cookies),
                urllib.request.HTTPSHandler(context=client._context))
            for _ in range(30):
                try:
                    client.claim(self.token)
                    break
                except (ConnectionError, urllib.error.URLError):
                    if process.poll() is not None:
                        self.fail(process.stderr.read())
                    time.sleep(.1)
            else:
                self.fail("qualified fixture gateway did not start")
            time.sleep(.3)
            with self.assertRaisesRegex(ConnectionError, "409"):
                with client.frames(cadence_hz=30, products=["depth"]):
                    self.fail("source cadence was silently downgraded")
            with client.frames(cadence_hz=10, products=["depth"]) as stream:
                self.assertEqual(stream.qualification, "qualified-exact")
                self.assertEqual(set(next(stream).products), {"depth"})
                with self.assertRaisesRegex(ConnectionError, "409"):
                    with client.frames(cadence_hz=10, products=["depth"]):
                        self.fail("reserved byte capacity was oversubscribed")
                evidence["enabled"] = False
                path.write_text(json.dumps(evidence))
                with self.assertRaises((ConnectionError, StopIteration)):
                    next(stream)
        finally:
            process.terminate()
            process.wait(timeout=3)
            self.assertEqual(process.returncode, 0, process.stderr.read())
            process.stderr.close()

    def test_stream_unqualified_wired_auto_is_explicit_best_effort(self):
        if not self.ethernet:
            self.skipTest("test host has no existing Ethernet IPv4 interface")
        _, address = self.ethernet
        arguments = list(self.process.args)
        index = arguments.index("--engineering-interface"); del arguments[index:index + 2]
        arguments[arguments.index("--address") + 1] = address
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as reservation:
            reservation.bind((address, 0)); port = reservation.getsockname()[1]
        arguments[arguments.index("--port") + 1] = str(port)
        evidence = Path(self.temporary.name) / "unqualified-fixture.json"
        evidence.write_text('{"enabled":false}')
        arguments.extend(["--qualification-file", str(evidence)])
        process = subprocess.Popen(arguments, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        try:
            client = NuvoClient(f"https://{address}:{port}", ca_file=self.certificate)
            client._opener = urllib.request.build_opener(urllib.request.ProxyHandler({}),
                urllib.request.HTTPCookieProcessor(client._cookies), urllib.request.HTTPSHandler(context=client._context))
            for _ in range(30):
                try: client.claim(self.token); break
                except (ConnectionError, urllib.error.URLError):
                    if process.poll() is not None: self.fail(process.stderr.read())
                    time.sleep(.1)
            else: self.fail("unqualified fixture gateway did not start")
            with client.frames(cadence_hz=5, products=["left_rgb"], delivery="auto") as stream:
                self.assertEqual(stream.qualification, "best-effort")
                self.assertEqual(set(next(stream).products), {"left_rgb"})
            self.wait_demand((0, 0, 0))
            for delivery in (None, "exact"):
                with self.assertRaisesRegex(ConnectionError, "409"):
                    with client.frames(cadence_hz=5, products=["left_rgb"], delivery=delivery):
                        self.fail("unqualified wired request claimed an exact contract")
                self.wait_demand((0, 0, 0))
        finally:
            process.terminate(); process.wait(timeout=3)
            self.assertEqual(process.returncode, 0, process.stderr.read()); process.stderr.close()

    def test_stream_source_configuration_change_invalidates_contract(self):
        client = NuvoClient(self.origin, ca_file=self.certificate)
        client.claim(self.token)
        try:
            with client.frames(cadence_hz=10, products=["depth"]) as stream:
                next(stream)
                self.local.publish(30, run=3, configuration_epoch=2)
                with self.assertRaises((ConnectionError, StopIteration)):
                    next(stream)
        finally:
            self.local.publish(31, run=3, configuration_epoch=1)
            time.sleep(.1)

    def test_stream_slow_writer_is_bounded_and_releases_admission(self):
        client = NuvoClient(self.origin, ca_file=self.certificate)
        client.claim(self.token)
        try:
            self.local.publish(40, run=3, width=512, height=512)
            time.sleep(.2)
            with client.frames() as blocked:
                blocked._socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
                # Each full frame is 3 MiB. A writer with a closed receive window
                # must not block other streams or retain admission indefinitely.
                with client.frames() as healthy:
                    self.assertEqual(next(healthy).products["depth"].width, 512)
                for sequence in range(41, 46):
                    self.local.publish(sequence, run=3, width=512, height=512)
                    time.sleep(.05)
                time.sleep(5.5)
                # Two reservations fit the 64 MiB/s engineering budget. Three
                # do not, so accepting these proves the stalled lease released.
                self.local.publish(46, run=3, width=512, height=512)
                with client.frames() as first, client.frames() as second:
                    self.assertEqual(next(first).products["depth"].width, 512)
                    self.assertEqual(next(second).products["depth"].width, 512)
        finally:
            self.local.publish(49, run=3)
            time.sleep(.2)

    def test_stream_subscription_errors_are_not_silently_downgraded(self):
        from nuvo_depth.client import _FrameStream
        client = NuvoClient(self.origin, ca_file=self.certificate)
        client.claim(self.token)
        for query in ("?cadence_hz=10&products=depth,depth", "?cadence_hz=nan&products=depth",
                      "?cadence_hz=10", "?cadence_hz=10&products=depth&surprise=1"):
            with self.assertRaisesRegex(ConnectionError, "400"):
                with _FrameStream(client, "/api/v1/stream" + query):
                    self.fail("invalid subscription upgraded successfully")

    def test_stream_unqualified_interface_rejects_exact_subscription(self):
        # A second gateway has no engineering-interface override and cannot
        # mislabel loopback (or WiFi) as qualified Ethernet.
        arguments = list(self.process.args)
        index = arguments.index("--engineering-interface")
        del arguments[index:index + 2]
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        arguments[arguments.index("--port") + 1] = str(port)
        process = subprocess.Popen(arguments, stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE, text=True)
        try:
            client = NuvoClient(f"https://127.0.0.1:{port}", ca_file=self.certificate)
            for _ in range(30):
                try:
                    client.claim(self.token)
                    break
                except ConnectionError:
                    time.sleep(.1)
                except urllib.error.URLError:
                    time.sleep(.1)
            else:
                self.fail("second gateway did not start")
            time.sleep(.3)
            with self.assertRaisesRegex(ConnectionError, "409"):
                with client.frames(cadence_hz=10, products=["depth"]):
                    self.fail("unqualified interface admitted exact subscription")
        finally:
            process.terminate()
            process.wait(timeout=3)
            self.assertEqual(process.returncode, 0, process.stderr.read())
            process.stderr.close()


if __name__=="__main__":
    unittest.main(argv=[sys.argv[0]])
