#!/usr/bin/env python3
"""Measure a bounded local depth/validity consumer without changing controls.

Run on the designated Pi. Supply cohort/condition hashes frozen by the operator.
Capture-to-receive latency uses the frame's declared boottime clock. This is a
local reader workload, not network throughput, independent accuracy or a power test.
"""
import argparse
from datetime import datetime,timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import select
import signal
import statistics
import struct
import subprocess
import time

HEADER=struct.Struct('>4sHHHHIQQ')
def now():return datetime.now(timezone.utc).isoformat()
def percentile(values,p):return sorted(values)[min(len(values)-1,math.ceil(len(values)*p)-1)] if values else None
def health():return json.loads(Path('/run/nuvo-depth/status.json').read_text())
ERRORS=('trigger_errors','frame_completion_errors','request_requeue_failures','scheduler_processing_failures')

def verify_conditions(args):
    data=args.conditions_file.read_bytes()
    if hashlib.sha256(data).hexdigest()!=args.conditions_sha256:raise ValueError('Frozen condition manifest changed')
    conditions=json.loads(data)
    if not conditions.get('protected_hashes'):raise ValueError('Frozen conditions need protected control/identity hashes')
    for name,expected in conditions['protected_hashes'].items():
        if hashlib.file_digest(Path(name).open('rb'),'sha256').hexdigest()!=expected:raise ValueError('Protected control/identity changed: '+name)
    current=health()
    if any(current.get(key)!=conditions.get(key) for key in ('calibration_sha256','calibration_authority')):raise ValueError('Calibration changed from frozen conditions')
    return conditions

def resources(pid):
    fields={}
    for line in Path('/proc',str(pid),'status').read_text().splitlines():
        key,_,value=line.partition(':')
        if key in ('VmRSS','VmSwap'):fields[key]=int(value.split()[0])*1024
    fields['cpu_ticks']=sum(map(int,Path('/proc',str(pid),'stat').read_text().split(') ',1)[1].split()[11:13]))
    fields['temperature_millidegrees']=int(Path('/sys/class/thermal/thermal_zone0/temp').read_text())
    result=subprocess.check_output(['vcgencmd','get_throttled'],text=True,timeout=3)
    fields['throttled']=int(result.strip().split('=')[1],16)
    return fields

def measure(args,index):
    conditions=verify_conditions(args)
    pid=int(subprocess.check_output(['systemctl','show','nuvo-depthd-native.service','--property=MainPID','--value'],text=True))
    before=health();started=now();clock=time.monotonic();end=clock+args.warmup+args.seconds
    process=subprocess.Popen([args.reader,'--frames','0','--hz',str(args.hz),'--timeout-seconds','30'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    buffer=bytearray();latencies=[];samples=[];identities=0;last=None;first=None;first_packet=None;first_hash=None;next_sample=clock;warm_end=clock+args.warmup
    trace_path=args.output/f'frames-{index:02}.jsonl'
    try:
        with trace_path.open('x') as trace:
            while time.monotonic()<end:
                current=time.monotonic()
                if current>=next_sample:
                    value=resources(pid);value.update(elapsed_seconds=current-clock,measured=current>=warm_end)
                    samples.append(value);next_sample=current+2
                readable,_,_=select.select([process.stdout],[],[],min(1,max(.001,end-current)))
                if not readable:continue
                chunk=os.read(process.stdout.fileno(),1024*1024)
                if not chunk:raise RuntimeError('Depth reader ended early: '+process.stderr.read(4096).decode(errors='replace'))
                buffer.extend(chunk)
                while len(buffer)>=HEADER.size:
                    magic,major,minor,kind,flags,ms,ps,sequence=HEADER.unpack_from(buffer)
                    if (magic,major,minor,kind,flags)!=(b'NUV1',1,0,5,0) or not 0<ms<=65536 or not 0<ps<=67108864:raise ValueError('Invalid bounded NUV1 header')
                    size=32+ms+ps
                    if len(buffer)<size:break
                    packet=bytes(buffer[:size]);del buffer[:size]
                    frame=json.loads(packet[32:32+ms]);received=time.clock_gettime_ns(time.CLOCK_BOOTTIME)
                    if any(frame.get(key)!=conditions.get(key) for key in ('calibration_sha256','calibration_authority')):raise ValueError('Frame calibration changed')
                    if frame.get('capture_clock')!='boottime' or frame.get('capture_sequence')!=sequence:raise ValueError('Unexpected capture clock/identity')
                    if {p['kind'] for p in frame['products']}!={'depth','validity'}:raise ValueError('Unexpected products')
                    identity=(frame['device_uuid'],frame['run_uuid'],frame['capture_epoch'],sequence)
                    if last and (identity[:3]!=last[:3] or sequence<=last[3]):raise ValueError('Duplicate/reversed identity or unexpected run/epoch change')
                    last=identity
                    if time.monotonic()<warm_end:continue
                    latency=(received-frame['capture_time_ns'])/1e6
                    if latency<0:raise ValueError('Capture/receive clocks disagree')
                    if first is None:
                        first=identity;first_packet=packet;first_hash=hashlib.sha256(packet).hexdigest()
                    identities+=1;latencies.append(latency)
                    trace.write(json.dumps({'at':now(),'identity':identity,'capture_time_ns':frame['capture_time_ns'],'received_boottime_ns':received,'latency_ms':latency,'calibration_sha256':frame['calibration_sha256'],'authority':frame['calibration_authority']})+'\n')
    finally:
        process.send_signal(signal.SIGINT)
        try:process.communicate(timeout=5)
        except subprocess.TimeoutExpired:process.kill();process.communicate(timeout=5)
    if identities<2:raise RuntimeError('No useful advancing measurement')
    verify_conditions(args)
    after=health();measured=[s for s in samples if s['measured']]
    if len(measured)<2:raise RuntimeError('Insufficient resource samples')
    errors=sum(max(0,after.get(key,0)-before.get(key,0)) for key in ERRORS)
    retained=hashlib.sha256(first_packet).hexdigest()==first_hash
    result={'index':index,'started_at':started,'finished_at':now(),'warmup_seconds':args.warmup,'seconds':args.seconds,
            'requested_hz':args.hz,'frames':identities,'depth_hz':identities/args.seconds,'latency_p50_ms':percentile(latencies,.5),
            'latency_p95_ms':percentile(latencies,.95),'latency_p99_ms':percentile(latencies,.99),
            'rss_steady_bytes':statistics.median(s['VmRSS'] for s in measured),'rss_peak_bytes':max(s['VmRSS'] for s in measured),
            'swap_bytes':max(s['VmSwap'] for s in measured),'throttled':max(s['throttled'] for s in samples),'new_core_errors':errors,
            'temperature_peak_millidegrees':max(s['temperature_millidegrees'] for s in samples),
            'process_cpu_percent_one_core':100*(measured[-1]['cpu_ticks']-measured[0]['cpu_ticks'])/os.sysconf('SC_CLK_TCK')/(measured[-1]['elapsed_seconds']-measured[0]['elapsed_seconds']),
            'first_identity':first,'last_identity':last,'retained_packet_immutable':retained,'samples':samples,
            'frame_trace':trace_path.name,'frame_trace_sha256':hashlib.file_digest(trace_path.open('rb'),'sha256').hexdigest()}
    (args.output/f'run-{index:02}.json').write_text(json.dumps(result,indent=2)+'\n')
    if errors or not retained or result['throttled'] or result['swap_bytes']:raise RuntimeError('Runtime error, throttle, swap or ownership failure; retain failed run')
    return result

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reader',default='/usr/lib/nuvo-depth/nuvo-depth-read')
    parser.add_argument('--warmup',type=int,default=300);parser.add_argument('--seconds',type=int,default=600)
    parser.add_argument('--runs',type=int,default=3);parser.add_argument('--hz',type=float,default=10)
    parser.add_argument('--source-commit',required=True);parser.add_argument('--artifact-sha256',required=True);parser.add_argument('--conditions-sha256',required=True)
    parser.add_argument('--conditions-file',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    if not 0<=args.warmup<=600 or not 10<=args.seconds<=7200 or not 1<=args.runs<=3 or not math.isfinite(args.hz) or not 0<args.hz<=60:parser.error('Invalid bounded duration/runs/cadence')
    for key,length in (('source_commit',40),('artifact_sha256',64),('conditions_sha256',64)):
        if not re.fullmatch('[a-f0-9]{'+str(length)+'}',getattr(args,key)):parser.error('Invalid '+key)
    args.output.mkdir(parents=True,exist_ok=False)
    report={key:getattr(args,key) for key in ('source_commit','artifact_sha256','conditions_sha256')}
    report.update(schema_version=1,scope='Local depth/validity consumer; explicit provisional authority remains unverified metric accuracy',runs=[])
    for index in range(1,args.runs+1):
        result=measure(args,index);report['runs'].append(result)
        (args.output/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps({key:result[key] for key in ('index','depth_hz','latency_p95_ms','rss_steady_bytes','new_core_errors')}),flush=True)

if __name__=='__main__':main()
