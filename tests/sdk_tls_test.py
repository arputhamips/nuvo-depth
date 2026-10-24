"""Real TLS trust/hostname/changed-key, SDK credentials and actionable failures."""
from datetime import datetime, timedelta, timezone
import base64
import hashlib
import http.server
import ipaddress
import json
import os
from pathlib import Path
import secrets
import ssl
import struct
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
import urllib.error
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'sdk/python'))
from nuvo_depth import NuvoClient
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID


def stream_packet(sequence, encoded=False):
    metadata = {"schema_version": 1, "device_uuid": "22" * 16, "run_uuid": "11" * 16,
                "capture_epoch": 2, "capture_sequence": sequence, "capture_time_ns": 4,
                "configuration_epoch": 5, "calibration_sha256": "00" * 32,
                "calibration_authority": 0}
    if encoded:
        payload = b"\xff\xd8fixture\xff\xd9"
        # Match src/image_encoder.cpp's spelling; raw frame_wire uses compact hex.
        metadata.update(device_uuid="22222222-2222-2222-2222-222222222222",
                        run_uuid="11111111-1111-1111-1111-111111111111")
        metadata.update(source="left_rectified", codec="jpeg", mime_type="image/jpeg",
                        width=1, height=1, keyframe=True)
    else:
        payload = struct.pack("<fB", 1.5, 1)
        metadata["products"] = [
            {"kind": "depth", "format": "float32_le", "width": 1, "height": 1, "offset": 0, "length": 4},
            {"kind": "validity", "format": "uint8", "width": 1, "height": 1, "offset": 4, "length": 1},
        ]
    document = json.dumps(metadata).encode()
    header = (struct.pack(">4sHHIQQI", b"NVE1", 1, 1, len(document), len(payload), sequence, 0)
              if encoded else struct.pack(">4sHHHHIQQ", b"NUV1", 1, 0, 5, 0, len(document), len(payload), sequence))
    packet = header + document + payload
    return b"\x82\x7e" + struct.pack(">H", len(packet)) + packet


def paired_packet(sequence):
    raw, encoded = stream_packet(sequence)[4:], stream_packet(sequence, True)[4:]
    packet = struct.pack(">4sHHQQQ", b"NVP1", 1, 0, len(raw), len(encoded), 0) + raw + encoded
    return b"\x82\x7e" + struct.pack(">H", len(packet)) + packet


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self,*_):pass
    def send(self,code,document,cookie=False):
        data=json.dumps(document).encode();self.send_response(code)
        self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(data)))
        if cookie:self.send_header('Set-Cookie',f'nuvo_session={self.server.session}; Path=/; Secure; HttpOnly')
        self.end_headers();self.wfile.write(data)
    def do_POST(self):
        self.server.observed_hosts.append(self.headers.get('Host'))
        if self.path=='/api/v1/session' and self.headers.get('Authorization')=='Bearer '+self.server.token:
            self.server.valid_session=True;return self.send(200,{'role':'admin','csrf':'b'*64},True)
        self.send(401,{'error':'TOKEN-CANARY-MUST-NOT-LEAK'})
    def do_GET(self):
        self.server.observed_hosts.append(self.headers.get('Host'))
        if self.path=='/api/v1/health' and getattr(self.server,'redirect_target',None):
            self.send_response(302);self.send_header('Location',self.server.redirect_target)
            self.send_header('Content-Length','0');self.end_headers();return
        if self.path=='/api/v1/session':return self.send(200,{'role':'admin','csrf':'b'*64},True)
        if not self.server.valid_session or self.headers.get('Cookie')!='nuvo_session='+self.server.session:
            return self.send(401,{'error':'session expired'})
        if self.path == '/api/v1/capabilities' and '/api/v1/paired/stream' in getattr(self.server, 'stream_frames', {}):
            return self.send(200, {'paired_stream': {'supported': True, 'path': '/api/v1/paired/stream',
                                                   'protocol': 'NVP1', 'maximum_wire_bytes': 20971552}})
        route = self.path.split('?', 1)[0]
        if route in getattr(self.server, 'stream_frames', {}):
            self.protocol_version = 'HTTP/1.1'
            key = self.headers['Sec-WebSocket-Key'].encode()
            accept = base64.b64encode(hashlib.sha1(key+b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest()).decode()
            self.send_response(101); self.send_header('Upgrade', 'websocket')
            self.send_header('Connection', 'Upgrade'); self.send_header('Sec-WebSocket-Accept', accept)
            self.end_headers()
            self.server.stream_requests.append(self.path)
            self.connection.settimeout(3)
            try:
                barrier = getattr(self.server, 'stream_barrier', None)
                if barrier is not None: barrier.wait(3)
                for packet in self.server.stream_frames[route]: self.wfile.write(packet)
                self.wfile.flush()
                self.rfile.read(2)
            except (OSError, threading.BrokenBarrierError):
                pass
            finally:
                self.close_connection = True
                self.server.stream_closed[route].set()
            return
        if self.path.startswith('/api/v1/stream'):return self.send(422,{'error':'budget exceeded'})
        if '/frame/' in self.path or '/encoded/' in self.path:return self.send(503,{'error':'output disabled'})
        self.send(200,{'status':'running','fixture':True})

class TlsTest(unittest.TestCase):
    def setUp(self):
        self.temporary=tempfile.TemporaryDirectory();self.addCleanup(self.temporary.cleanup)
        self.root=Path(self.temporary.name);self.context,self.certificate=self.certificate_pair('first')
        self.server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
        self.server.token=secrets.token_hex(32);self.server.session=secrets.token_hex(32);self.server.valid_session=True
        self.server.observed_hosts=[]
        self.server.socket=self.context.wrap_socket(self.server.socket,server_side=True)
        self.thread=threading.Thread(target=self.server.serve_forever,daemon=True);self.thread.start()
        self.addCleanup(self.stop)
        self.url='https://127.0.0.1:'+str(self.server.server_port)
    def stop(self):self.server.shutdown();self.server.server_close();self.thread.join(3)
    def certificate_pair(self,name):
        key=rsa.generate_private_key(public_exponent=65537,key_size=2048)
        subject=x509.Name([x509.NameAttribute(NameOID.COMMON_NAME,'localhost')]);now=datetime.now(timezone.utc)
        certificate=(x509.CertificateBuilder().subject_name(subject).issuer_name(subject).public_key(key.public_key())
                     .serial_number(x509.random_serial_number()).not_valid_before(now-timedelta(minutes=1))
                     .not_valid_after(now+timedelta(days=1)).add_extension(x509.SubjectAlternativeName([
                         x509.IPAddress(ipaddress.ip_address('127.0.0.1')),x509.DNSName('nuvo.test')]),False)
                     .sign(key,hashes.SHA256()))
        cert=self.root/(name+'.pem');private=self.root/(name+'.key')
        cert.write_bytes(certificate.public_bytes(serialization.Encoding.PEM))
        private.write_bytes(key.private_bytes(serialization.Encoding.PEM,serialization.PrivateFormat.PKCS8,serialization.NoEncryption()))
        context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);context.load_cert_chain(cert,private)
        return context,str(cert)
    def client(self):return NuvoClient(self.url,ca_file=self.certificate,timeout=2)
    def enable_streams(self, frames, concurrent=False):
        self.server.stream_frames = frames
        self.server.stream_requests = []
        self.server.stream_closed = {route: threading.Event() for route in frames}
        if concurrent: self.server.stream_barrier = threading.Barrier(len(frames))

    def test_persistent_encoded_frames_over_real_tls_preserve_selected_address_and_identity(self):
        self.enable_streams({'/api/v1/encoded/stream': [stream_packet(3, True), stream_packet(4, True)]})
        url = self.url.replace('127.0.0.1', 'nuvo.test')
        with NuvoClient(url, ca_file=self.certificate, connect_address='127.0.0.1', timeout=2) as client:
            client.claim(self.server.token)
            with client.encoded_frames(cadence_hz=5) as frames:
                self.assertEqual(next(frames).identity, ('22'*16, '11'*16, 2, 3))
                self.assertEqual(next(frames).capture_sequence, 4)
            self.assertTrue(self.server.stream_closed['/api/v1/encoded/stream'].wait(1))
        self.assertEqual(self.server.stream_requests, ['/api/v1/encoded/stream?cadence_hz=5'])
        self.assertEqual(set(self.server.observed_hosts), {'nuvo.test:'+str(self.server.server_port)})

    def test_associated_stream_delivers_original_inner_frames_and_closes_over_real_tls(self):
        self.enable_streams({'/api/v1/paired/stream': [paired_packet(3), paired_packet(5)]})
        with self.client() as client:
            client.claim(self.server.token)
            with client.paired_frames(cadence_hz=2, preview_cadence_hz=2) as pairs:
                frame, picture = next(pairs)
                self.assertEqual(frame.identity, picture.identity)
                self.assertEqual(frame.capture_sequence, 3)
                self.assertEqual(tuple(frame.products), ('depth', 'validity'))
                self.assertEqual(next(pairs).identity[-1], 5)
            self.assertTrue(all(event.wait(1) for event in self.server.stream_closed.values()))
        self.assertEqual(self.server.stream_requests, ['/api/v1/paired/stream?delivery=best_effort&cadence_hz=2&products=depth,validity'])

    def test_old_gateway_rejects_pairing_without_opening_unassociated_streams(self):
        self.enable_streams({})
        with self.client() as client:
            client.claim(self.server.token)
            with self.assertRaisesRegex(RuntimeError, 'does not support.*paired'):
                with client.paired_frames(): pass
        self.assertEqual(self.server.stream_requests, [])

    def test_encoded_stream_still_verifies_tls_hostname(self):
        good = self.client(); good.claim(self.server.token)
        bad = NuvoClient(self.url.replace('127.0.0.1', 'wrong.test'), ca_file=self.certificate,
                         connect_address='127.0.0.1', timeout=2)
        bad._csrf = good._csrf
        # A valid session cookie for this host cannot bypass certificate identity.
        for cookie in good._cookies:
            cookie.domain = 'wrong.test'; bad._cookies.set_cookie(cookie)
        with self.assertRaises(ssl.SSLCertVerificationError):
            with bad.encoded_frames(): pass

    def test_real_trust_and_hostname_verification(self):
        client=self.client();client.claim(self.server.token);self.assertEqual(client.health()['status'],'running')
        for client in (NuvoClient(self.url,timeout=2),NuvoClient(self.url.replace('127.0.0.1','localhost'),ca_file=self.certificate,timeout=5)):
            with self.assertRaises(urllib.error.URLError):client.health()
    def test_changed_device_certificate_rejects_old_trust(self):
        context,certificate=self.certificate_pair('replacement')
        port=self.server.server_port
        self.stop()
        self.server=http.server.ThreadingHTTPServer(('127.0.0.1',port),Handler)
        self.server.token=secrets.token_hex(32);self.server.session=secrets.token_hex(32);self.server.valid_session=True
        self.server.observed_hosts=[]
        self.server.socket=context.wrap_socket(self.server.socket,server_side=True)
        self.thread=threading.Thread(target=self.server.serve_forever,daemon=True);self.thread.start()
        with self.assertRaises(urllib.error.URLError):self.client().health()
        client=NuvoClient(self.url,ca_file=certificate,timeout=2);client.claim(self.server.token)
        self.assertTrue(client.health()['fixture'])

    def test_direct_address_preserves_tls_identity_for_https_and_websocket(self):
        url=self.url.replace('127.0.0.1','nuvo.test')
        with patch.dict(os.environ,{'https_proxy':'http://127.0.0.1:1','HTTPS_PROXY':'http://127.0.0.1:1','no_proxy':'','NO_PROXY':''}):
            with NuvoClient(url,ca_file=self.certificate,connect_address='127.0.0.1',timeout=2) as client:
                client.claim(self.server.token)
                self.assertTrue(client.health()['fixture'])
                with self.assertRaisesRegex(ConnectionError,'reduce products/cadence'):
                    with client.frames(products=('depth','validity'),cadence_hz=2) as stream:next(stream)
        self.assertTrue(self.server.observed_hosts)
        self.assertEqual(set(self.server.observed_hosts),{'nuvo.test:'+str(self.server.server_port)})
        for bad_url,ca_file in ((url,None),(url.replace('nuvo.test','wrong.test'),self.certificate)):
            with self.subTest(url=bad_url,ca_file=ca_file),self.assertRaises(urllib.error.URLError):
                NuvoClient(bad_url,ca_file=ca_file,connect_address='127.0.0.1',timeout=2).health()

    def test_direct_address_rejects_redirects_and_has_no_dns_fallback(self):
        url=self.url.replace('127.0.0.1','nuvo.test')
        with NuvoClient(url,ca_file=self.certificate,connect_address='127.0.0.1',timeout=2) as client:
            client.claim(self.server.token)
            for target in ('https://other.invalid/steal','http://127.0.0.1:1/steal',url+'/other'):
                self.server.redirect_target=target
                count=len(self.server.observed_hosts)
                with self.assertRaises(urllib.error.HTTPError) as failure:client.health()
                self.assertEqual(failure.exception.code,302);failure.exception.close()
                self.assertEqual(len(self.server.observed_hosts),count+1)
        with patch('nuvo_depth.client.socket.create_connection',side_effect=OSError('selected path unavailable')) as dial:
            with self.assertRaises(urllib.error.URLError):
                NuvoClient(url,ca_file=self.certificate,connect_address='192.0.2.42',timeout=2).health()
        self.assertEqual(dial.call_count,1)
        self.assertEqual(dial.call_args.args[0],('192.0.2.42',self.server.server_port))
    def test_credentials_expiry_and_errors_do_not_echo_token(self):
        client=self.client()
        for operation in (lambda:client.health(),lambda:client.claim('invalid-token-'+'x'*32)):
            with self.assertRaises(urllib.error.HTTPError) as failure:operation()
            self.assertIn('credentials/session rejected',str(failure.exception))
            self.assertNotIn('TOKEN-CANARY',str(failure.exception))
            self.assertNotIn(self.server.token,str(failure.exception))
            failure.exception.close()
        client.claim(self.server.token);self.server.valid_session=False
        with self.assertRaises(urllib.error.HTTPError) as failure:client.health()
        self.assertIn('claim a new session',str(failure.exception))
        failure.exception.close()
    def test_disabled_output_and_excessive_subscription_are_actionable(self):
        client=self.client();client.claim(self.server.token)
        for operation,action in ((client.latest,'Raw output'),(client.latest_encoded,'Monitoring')):
            with self.assertRaises(urllib.error.HTTPError) as failure:operation()
            self.assertIn(action,str(failure.exception))
            failure.exception.close()
        with self.assertRaisesRegex(ConnectionError,'reduce products/cadence'):
            with client.frames(products=('depth','validity'),cadence_hz=60,delivery='exact') as stream:next(stream)

if __name__=='__main__':unittest.main()
