import http.cookiejar
import json
from pathlib import Path
import secrets
import socket
import ssl
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.request


class GatewayIntegrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary=Path(sys.argv[1]).resolve(); cls.web=Path(sys.argv[2]).resolve()
        cls.temporary=tempfile.TemporaryDirectory(); root=Path(cls.temporary.name)
        cls.token=secrets.token_hex(32); (root/"token").write_text(cls.token+"\n")
        cls.encoding={"schema_version":1,"mode":"both","codec":"jpeg","source":"left_rectified",
                      "width":0,"height":0,"cadence_hz":10,"bitrate_bps":4000000,
                      "jpeg_quality":80,"keyframe_interval":30,"threads":2,
                      "low_latency":True,"preset":"veryfast"}
        (root/"encoding.json").write_text(json.dumps(cls.encoding)+"\n")
        cls.stereo=json.loads((Path(__file__).parents[1]/"config/stereo.json").read_text())
        (root/"stereo.json").write_text(json.dumps(cls.stereo)+"\n")
        (root/"encoded.nve").write_bytes(b"NVE1"+bytes(60))
        subprocess.run(["openssl","req","-x509","-newkey","rsa:2048","-nodes","-days","1",
                        "-subj","/CN=127.0.0.1","-keyout",str(root/"key.pem"),"-out",str(root/"cert.pem")],
                       check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        with socket.socket(socket.AF_INET,socket.SOCK_STREAM) as reservation:
            reservation.bind(("127.0.0.1",0)); cls.port=reservation.getsockname()[1]
        cls.process=subprocess.Popen([
            str(cls.binary),"--address","127.0.0.1","--port",str(cls.port),
            "--certificate",str(root/"cert.pem"),"--private-key",str(root/"key.pem"),
            "--token-file",str(root/"token"),"--web-root",str(cls.web),
            "--status-file",str(root/"missing-status"),"--snapshot-file",str(root/"missing-frame"),
            "--encoded-file",str(root/"encoded.nve"),"--encoding-config-file",str(root/"encoding.json"),
            "--stereo-control-file",str(root/"stereo.json"),
            "--transport-file",str(root/"missing-transport"),"--system-agent-socket",str(root/"missing-agent")],
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
        cls.process.terminate()
        try: cls.process.wait(timeout=5)
        except subprocess.TimeoutExpired: cls.process.kill(); cls.process.wait(timeout=5)
        cls.temporary.cleanup()

    @classmethod
    def open(cls,path,method="GET",headers=None,body=None):
        request=urllib.request.Request(cls.origin+path,data=body,headers=headers or {},method=method)
        return cls.client.open(request,timeout=5)

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
        with self.assertRaises(urllib.error.HTTPError) as stereo_without_csrf:
            self.open("/api/v1/stereo","POST",{"Content-Type":"application/json"},
                      json.dumps(self.stereo).encode())
        self.assertEqual(stereo_without_csrf.exception.code,403)
        with self.open("/api/v1/encoded/latest") as response:
            self.assertEqual(response.headers["Content-Type"],"application/vnd.nuvo.encoded-image")
            self.assertTrue(response.read().startswith(b"NVE1"))


if __name__=="__main__":
    unittest.main(argv=[sys.argv[0]])
