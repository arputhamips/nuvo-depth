"""Real TLS trust/hostname/changed-key, SDK credentials and actionable failures."""
from datetime import datetime, timedelta, timezone
import http.server
import ipaddress
import json
import os
from pathlib import Path
import secrets
import ssl
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
