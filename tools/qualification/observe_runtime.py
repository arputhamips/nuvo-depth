#!/usr/bin/env python3
"""Bounded read-only Pi resource observer; consumers run separately."""
import argparse
from datetime import datetime,timezone
import json
from pathlib import Path
import subprocess
import time

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds',type=int,default=3600)
    parser.add_argument('--interval',type=float,default=2)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    if not 1<=args.seconds<=14400 or not .5<=args.interval<=60:parser.error('seconds 1..14400; interval .5..60')
    started=time.monotonic();deadline=started+args.seconds
    with args.output.open('x',encoding='utf-8') as output:
        while time.monotonic()<deadline:
            sample={'at':datetime.now(timezone.utc).isoformat(),'elapsed_seconds':time.monotonic()-started,'processes':{}}
            for name in ('depthd','nuvo-gateway'):
                result=subprocess.run(['pgrep','-x',name],capture_output=True,text=True,timeout=3)
                for pid in result.stdout.split():
                    try:
                        fields={}
                        for line in Path('/proc',pid,'status').read_text().splitlines():
                            key,_,value=line.partition(':')
                            if key in ('VmRSS','VmSwap','Threads'):fields[key]=int(value.split()[0])
                        fields['stat']=Path('/proc',pid,'stat').read_text().split(') ',1)[1].split()[11:13]
                        sample['processes'][name+':'+pid]=fields
                    except (OSError,ValueError,IndexError):pass
            try:sample['temperature_millidegrees']=int(Path('/sys/class/thermal/thermal_zone0/temp').read_text())
            except (OSError,ValueError):sample['temperature_millidegrees']=None
            try:
                result=subprocess.run(['vcgencmd','get_throttled'],capture_output=True,text=True,timeout=3)
                sample['throttled']=int(result.stdout.strip().split('=')[1],16) if result.returncode==0 else None
            except (OSError,ValueError,IndexError):sample['throttled']=None
            try:
                health=json.loads(Path('/run/nuvo-depth/status.json').read_text())
                sample['counters']={key:value for key,value in health.items() if type(value) in (int,float) and any(part in key for part in ('frame','pair','drop','error','ms','hz','sequence'))}
            except (OSError,ValueError):sample['counters']=None
            output.write(json.dumps(sample,allow_nan=False)+'\n');output.flush()
            time.sleep(max(0,min(args.interval,deadline-time.monotonic())))

if __name__=='__main__':main()
