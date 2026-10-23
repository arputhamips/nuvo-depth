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
import struct
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
    # The protocol integration fixture intentionally uses undecodable bytes.
    # This rendered-browser fixture must supply an actual synthetic picture.
    fixture.local.auto_encode=False
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
    transport_path=fixture.config_root/"transport.json"
    transport_off=False; transport_original=None
    preview_revision=None; preview_payload=None; preview_generation=0; preview_sequence=0; next_preview=0.
    try:
        while not stop.wait(.1):
            try:
                value=json.loads(args.control.read_text())
                if isinstance(value,dict):control=value
            except (OSError,ValueError):pass
            # Model a local operator policy write only inside this temporary root.
            # The HTTP API deliberately does not permit remote master Off.
            requested_off=control.get('transport_off') is True
            if requested_off!=transport_off:
                if requested_off:
                    transport_original=transport_path.read_bytes() if transport_path.exists() else None
                    policy=json.loads(transport_original) if transport_original is not None else {'schema_version':1}
                    policy['preference']='off';atomic_json(transport_path,policy)
                elif transport_original is None:transport_path.unlink(missing_ok=True)
                else:
                    temporary=transport_path.with_suffix('.restore')
                    temporary.write_bytes(transport_original);temporary.replace(transport_path)
                transport_off=requested_off
            requested=control.get('run',2)
            if type(requested) is not int or not 1<=requested<=255:requested=2
            if requested!=current_run:sequence=0;current_run=requested
            sequence+=1
            state=control.get('state','unavailable')
            fixture.local.serve_frames=state in ('preview_enabled','calibration_accepted')
            fixture.local.authority=1 if state=='calibration_accepted' else 0
            fixture.local.publish(sequence,run=current_run,width=16,height=12)
            demand=fixture.local.demand()
            encoding_bytes=(fixture.config_root/'encoding.json').read_bytes()
            encoding=json.loads(encoding_bytes)
            revision=hashlib.sha256(encoding_bytes).hexdigest()
            if encoding['codec']=='jpeg' and encoding['mode']!='raw' and demand[2]>0 and time.monotonic()>=next_preview:
                if preview_revision!=revision:
                    import cv2
                    import numpy as np
                    width,height=encoding['width'] or 16,encoding['height'] or 12
                    image=np.zeros((height,width,3),dtype=np.uint8)
                    image[:,:,0]=np.arange(width,dtype=np.uint32)%256
                    image[:,:,1]=(np.arange(height,dtype=np.uint32)%256)[:,None]
                    image[:,:,2]=100
                    valid,payload=cv2.imencode('.jpg',image,[cv2.IMWRITE_JPEG_QUALITY,encoding['jpeg_quality']])
                    if not valid:raise RuntimeError('cannot encode synthetic browser fixture JPEG')
                    preview_payload=payload.tobytes();preview_revision=revision;preview_generation+=1
                preview_sequence+=1
                envelope=fixture_module.encoded_frame(preview_sequence,
                    run_uuid=f'{current_run:02x}000000-0000-0000-0000-000000000000',capture_sequence=sequence,
                    source=encoding['source'],width=encoding['width'] or 16,height=encoding['height'] or 12,
                    encoding_applied_revision=revision,encoding_configuration_generation=preview_generation)
                metadata_size=struct.unpack_from('>I',envelope,8)[0]
                header=struct.pack('>4sHHIQQI',b'NVE1',1,1,metadata_size,len(preview_payload),preview_sequence,0)
                temporary=fixture.local.encoded_path.with_suffix('.tmp')
                temporary.write_bytes(header+envelope[32:32+metadata_size]+preview_payload)
                temporary.replace(fixture.local.encoded_path)
                next_preview=time.monotonic()+1/min(encoding['cadence_hz'],demand[2])
            health={'schema_version':1,'status':'running','fixture':True,'capture_sequence':sequence,
                    'run_uuid':f'{current_run:02x}'+'00'*15,'image_width':16,'image_height':12,'depth_width':16,'depth_height':12,
                    'capture_target_hz':10,'captured_pairs':sequence,'matched_frames':sequence,'published_frames':sequence,
                    'calibration_sha256':'01'+'00'*31,'calibration_authority':fixture.local.authority,'processing_ms':1.,
                    'left_camera_id':'fixture-left','right_camera_id':'fixture-right','stereo_backend':'fixture',
                    'profile_id':'synthetic_fixture','matcher_active':bool(demand[0]&60),
                    'raw_product_demand':demand[0],'raw_demand_cadence_hz':demand[1],
                    'preview_demand_cadence_hz':demand[2],'encoding_active':demand[2]>0,
                    'encoding_applied_revision':preview_revision or '',
                    'encoding_configuration_generation':preview_generation,'encoded_frames':preview_sequence}
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
