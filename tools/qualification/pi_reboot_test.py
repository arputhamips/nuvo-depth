#!/usr/bin/env python3
"""Host-only audited graceful reboot test; never counts as physical power cycles.

Requires paramiko on the operator host, password via hidden prompt, and an
independently verified known-hosts entry. No device/package configuration writes.
"""
import argparse
from datetime import datetime,timezone
import getpass
import hashlib
import json
from pathlib import Path
import shlex
import socket
import time
import paramiko

PROBE="""set -e
python3 - <<'PY'
import hashlib,json,struct,subprocess,time
from pathlib import Path
for attempt in range(30):
    if subprocess.run(['systemctl','is-active','--quiet','nuvo-depthd-native.service']).returncode==0:break
    time.sleep(1)
result=subprocess.run(['/usr/lib/nuvo-depth/nuvo-depth-read','--frames','2','--hz','1','--timeout-seconds','30'],capture_output=True,timeout=40,check=True)
data=result.stdout;frames=[]
while data:
    if len(data)<32:raise ValueError('Truncated frame header')
    magic,major,minor,kind,flags,ms,ps,seq=struct.unpack_from('>4sHHHHIQQ',data)
    if (magic,major,minor,kind,flags)!=(b'NUV1',1,0,5,0) or not 0<ms<=65536 or not 0<ps<=67108864 or len(data)<32+ms+ps:raise ValueError('Invalid frame bounds')
    metadata=json.loads(data[32:32+ms]);frames.append(metadata);data=data[32+ms+ps:]
assert len(frames)==2 and frames[0]['capture_sequence']<frames[1]['capture_sequence']
assert frames[0]['run_uuid']==frames[1]['run_uuid']
assert all({p['kind'] for p in f['products']}=={'depth','validity'} for f in frames)
paths=[Path('/var/lib/nuvo-depth/identity/device.uuid'),*Path('/etc/nuvo-depth').rglob('*')]
hashes={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths if p.is_file() and not p.is_symlink()}
health=json.loads(Path('/run/nuvo-depth/status.json').read_text())
print(json.dumps({'boot_id':Path('/proc/sys/kernel/random/boot_id').read_text().strip(),'frames':frames,'protected_hashes':hashes,'dpkg_audit':subprocess.check_output(['dpkg','--audit'],text=True),'core_errors':{k:health.get(k) for k in ('trigger_errors','frame_completion_errors','request_requeue_failures','scheduler_processing_failures')}}))
PY
"""

def now():return datetime.now(timezone.utc).isoformat()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host',required=True);parser.add_argument('--address',required=True)
    parser.add_argument('--user',required=True);parser.add_argument('--known-hosts',type=Path,required=True)
    parser.add_argument('--cycles',type=int,default=10);parser.add_argument('--profile',choices=('minimal','appliance'),required=True)
    parser.add_argument('--output',type=Path,required=True);parser.add_argument('--audit',type=Path,required=True)
    args=parser.parse_args()
    if not 1<=args.cycles<=10:parser.error('cycles must be 1..10')
    args.output.mkdir(parents=True,exist_ok=False);args.audit.parent.mkdir(parents=True,exist_ok=True)
    password=getpass.getpass('Pi SSH/sudo password (hidden): ')
    def audit(value):
        with args.audit.open('a',encoding='utf-8') as output:output.write(json.dumps({'at':now(),**value})+'\n');output.flush()
    def connect():
        client=paramiko.SSHClient();client.load_host_keys(str(args.known_hosts))
        sock=socket.create_connection((args.address,22),timeout=5)
        try:client.connect(args.host,username=args.user,password=password,sock=sock,look_for_keys=False,allow_agent=False,timeout=8,banner_timeout=8,auth_timeout=8)
        except BaseException:sock.close();client.close();raise
        return client
    def execute(client,command,timeout=60):
        identity=now();wrapper='sudo -S -p "" bash -c '+shlex.quote(command)
        audit({'event':'command_started','id':identity,'sudo':True,'command':command,'executed_shell_command':wrapper})
        stdin,stdout,stderr=client.exec_command(wrapper,timeout=timeout)
        stdin.write(password+'\n');stdin.flush();stdin.channel.shutdown_write()
        out,err=stdout.read().decode(),stderr.read().decode();code=stdout.channel.recv_exit_status()
        audit({'event':'command_finished','id':identity,'exit_code':code,'stdout_sha256':hashlib.sha256(out.encode()).hexdigest(),'stderr_sha256':hashlib.sha256(err.encode()).hexdigest()})
        if code:raise RuntimeError(f'Pi command failed ({code}): '+err[-1000:])
        return out
    client=connect();previous=json.loads(execute(client,PROBE));baseline=previous['protected_hashes']
    (args.output/'before.json').write_text(json.dumps(previous,indent=2)+'\n')
    results=[]
    try:
        for index in range(1,args.cycles+1):
            start=now();execute(client,'systemctl reboot --no-block\n',10);client.close()
            # Wait for the old transport/boot to disappear before reconnecting.
            time.sleep(8);deadline=time.monotonic()+120
            while True:
                try:client=connect();current=json.loads(execute(client,PROBE));break
                except (OSError,paramiko.SSHException):
                    if time.monotonic()>deadline:raise
                    time.sleep(3)
            problems=[]
            if current['boot_id']==previous['boot_id']:problems.append('boot did not change')
            if current['frames'][0]['run_uuid']==previous['frames'][0]['run_uuid']:problems.append('daemon run did not change')
            if current['protected_hashes']!=baseline:problems.append('protected configuration/identity changed')
            if current['dpkg_audit']:problems.append('dpkg audit is not clean')
            if any(value not in (None,0) for value in current['core_errors'].values()):problems.append('core errors reported')
            for field in ('calibration_sha256','calibration_authority','device_uuid'):
                if current['frames'][0][field]!=previous['frames'][0][field]:problems.append(field+' changed')
            result={'cycle':index,'profile':args.profile,'event':'graceful_kernel_reboot','started_at':start,'finished_at':now(),'status':'fail' if problems else 'pass','problems':problems,**current}
            (args.output/f'cycle-{index:02}.json').write_text(json.dumps(result,indent=2)+'\n');results.append(result)
            print(json.dumps({'cycle':index,'status':result['status'],'boot_id':current['boot_id'],'problems':problems}),flush=True)
            if problems:raise RuntimeError('Cycle qualification failed: '+', '.join(problems))
            previous=current
    finally:
        client.close()
        (args.output/'summary.json').write_text(json.dumps({'profile':args.profile,'requested':args.cycles,'completed':len(results),'physical_power_cycles':0,'results':[{'cycle':r['cycle'],'status':r['status']} for r in results]},indent=2)+'\n')

if __name__=='__main__':main()
