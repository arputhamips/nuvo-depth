"""Launch the real HTTPS gateway with labeled synthetic IPC/agent fixtures.

No root privileges, camera, GPIO, systemd or installed-state writes. The public
certificate and private random test token remain in a private temporary root.
"""
import argparse
import base64
import hashlib
import importlib.util
import json
from pathlib import Path
import signal
import subprocess
import sys
import threading
import time

ROOT=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(ROOT/'tools'),str(ROOT/'tests'),str(ROOT/'sdk/python')]
from nuvo_calibration_store import atomic_json

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--manifest',type=Path,required=True)
    parser.add_argument('--control',type=Path,required=True)
    args=parser.parse_args()
    if args.manifest.exists(): parser.error('choose a new fixture manifest path')
    args.manifest.parent.mkdir(parents=True,exist_ok=True)
    spec=importlib.util.spec_from_file_location('nuvo_gateway_fixture',ROOT/'tests/gateway_integration_test.py')
    fixture_module=importlib.util.module_from_spec(spec);spec.loader.exec_module(fixture_module)
    fixture=fixture_module.GatewayIntegrationTest
    original=sys.argv
    try:
        sys.argv=[str(__file__),str(args.binary.resolve()),str(ROOT/'web')]
        fixture.setUpClass()
    finally:sys.argv=original
    stop=threading.Event()
    for item in (signal.SIGINT,signal.SIGTERM):signal.signal(item,lambda *_:stop.set())
    directory=Path(fixture.temporary.name)
    control={'state':'preview_enabled','run':2,'gateway_restart':0}
    atomic_json(args.control,control)
    index=directory/'web/index.html'
    html=index.read_text();html=html.replace('<body>','<body><p id="fixtureBanner" role="status">TEST FIXTURE — synthetic replay data; no physical device or accuracy claim.</p>',1)
    index.write_text(html)
    def readiness():
        state=control.get('state','unavailable')
        labels={'preview_enabled':'Preview enabled','calibration_accepted':'Calibration accepted',
                'calibration_needed':'Calibration needed','unavailable':'Status unavailable/error'}
        if state not in labels:state='unavailable'
        return {'schema_version':1,'display_name':'TEST FIXTURE','phase':'installed',
                'calibration_readiness':{'schema_version':1,'state':state,'label':labels[state],
                'accepted':state=='calibration_accepted','capture_enabled':state in ('preview_enabled','calibration_accepted'),
                'message':'Synthetic test state; distance accuracy is not evaluated.',
                'next_action':'Inspect the fixture controls; use a real Pi for physical qualification.'}}
    fixture.agent.setup.status=lambda _:readiness()
    pub=subprocess.check_output(['openssl','x509','-in',fixture.certificate,'-pubkey','-noout'])
    der=subprocess.check_output(['openssl','pkey','-pubin','-outform','DER'],input=pub)
    manifest={'schema_version':1,'fixture':True,'origin':fixture.origin,
              'certificate':fixture.certificate,'certificate_spki_sha256':base64.b64encode(hashlib.sha256(der).digest()).decode(),
              'token_file':str(directory/'token'),'control_file':str(args.control.resolve()),
              'binary_sha256':hashlib.file_digest(open(args.binary,'rb'),'sha256').hexdigest(),
              'identities':'Fixed synthetic device; configurable synthetic run; advancing capture sequence.',
              'calibration':'Provisional synthetic data by default; no accuracy evidence.'}
    atomic_json(args.manifest,manifest)
    print(json.dumps({'fixture':True,'origin':fixture.origin,'manifest':str(args.manifest)}),flush=True)
    sequence=0; current_run=2; restarts=0
    try:
        while not stop.wait(.1):
            try:
                value=json.loads(args.control.read_text())
                if isinstance(value,dict):control=value
            except (OSError,ValueError):pass
            requested=control.get('run',2)
            if type(requested) is not int or not 1<=requested<=255:requested=2
            if requested!=current_run:sequence=0;current_run=requested
            sequence+=1
            state=control.get('state','unavailable')
            fixture.local.serve_frames=state in ('preview_enabled','calibration_accepted')
            fixture.local.authority=1 if state=='calibration_accepted' else 0
            fixture.local.publish(sequence,run=current_run,width=16,height=12)
            health={'schema_version':1,'status':'running','fixture':True,'capture_sequence':sequence,
                    'run_uuid':f'{current_run:02x}'+'00'*15,'image_width':16,'image_height':12,'depth_width':16,'depth_height':12,
                    'capture_target_hz':10,'captured_pairs':sequence,'matched_frames':sequence,'published_frames':sequence,
                    'calibration_sha256':'01'+'00'*31,'calibration_authority':fixture.local.authority,'processing_ms':1.,
                    'left_camera_id':'fixture-left','right_camera_id':'fixture-right','stereo_backend':'fixture',
                    'profile_id':'synthetic_fixture','matcher_active':bool(fixture.local.demand()[0]&24)}
            path=directory/'missing-status'
            if fixture.local.serve_frames:atomic_json(path,health)
            else:path.unlink(missing_ok=True)
            if control.get('gateway_restart',0)!=restarts:
                restarts=control.get('gateway_restart',0)
                command=fixture.process.args
                fixture.process.terminate();fixture.process.wait(timeout=5)
                fixture.process=subprocess.Popen(command,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,text=True)
    finally:
        fixture.tearDownClass()
        args.manifest.unlink(missing_ok=True)

if __name__=='__main__':main()
