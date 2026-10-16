#!/usr/bin/env python3
"""Bounded, reversible profiling of the installed native Pi appliance.

Requires root. Stops the gateway while isolating native capacity, temporarily
changes capture/matcher/encoding controls, and restores their exact bytes.
Viewer tests use a temporary gateway port so open UI clients cannot retune the
workload. The original service command is restored on exit. This does not
install or activate calibration and does not change boot settings.
"""
from __future__ import annotations

import argparse
import array
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import re
import signal
import shlex
import socket
import statistics
import struct
import subprocess
import sys
import tempfile
import threading
import time
import types

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "sdk/python"))
from nuvo_depth.frame import decode_frame
from nuvo_depth.encoded import decode_encoded_frame
from nuvo_depth import NuvoClient
from nuvo_depth.client import _FrameStream
from nuvo_depth.frame import PRODUCT_FORMATS

ETC = Path("/etc/nuvo-depth")
STATUS = Path("/run/nuvo-depth/status.json")
ENCODED = Path("/run/nuvo-depth/snapshots/latest.nve")
NATIVE = "nuvo-depthd-native.service"
GATEWAY = "nuvo-gateway.service"
GATEWAY_PORT = 7443
MIB = 1024 * 1024


def read_json(path):
    return json.loads(path.read_text())


def atomic_bytes(path, value):
    original = path.stat()
    temporary = path.with_name(path.name + ".resource-profile.tmp")
    temporary.write_bytes(value)
    os.chmod(temporary, original.st_mode & 0o777)
    os.chown(temporary, original.st_uid, original.st_gid)
    os.replace(temporary, path)


def stats(values):
    if not values:
        return None
    ordered = sorted(values)
    return {"min": ordered[0], "p50": statistics.median(ordered),
            "p95": ordered[min(len(ordered)-1, math.ceil(.95*len(ordered))-1)],
            "max": ordered[-1]}


def kv_bytes(path):
    result = {}
    for line in path.read_text().splitlines():
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        fields = value.split()
        if fields and fields[0].isdigit():
            result[key] = int(fields[0]) * (1024 if len(fields)>1 and fields[1]=="kB" else 1)
    return result


def service_pid(unit):
    return int(subprocess.check_output(["systemctl", "show", unit, "-p", "MainPID", "--value"], text=True))


def numa_placement(pid):
    nodes={};policies=set()
    for line in Path(f"/proc/{pid}/numa_maps").read_text().splitlines():
        if "anon=" not in line and " heap " not in line and " stack " not in line:
            continue
        policies.add(line.split()[1])
        for node,count in re.findall(r"\bN(\d+)=(\d+)",line):
            nodes[node]=nodes.get(node,0)+int(count)
    return {"anonymous_pages_by_node":nodes,"policies":sorted(policies),
            "page_bytes":os.sysconf("SC_PAGE_SIZE")}


def cgroup_memory(unit):
    base = Path("/sys/fs/cgroup/system.slice") / unit
    values = {}
    for name in ("memory.current", "memory.peak", "memory.events", "memory.stat"):
        p = base/name
        if p.exists():
            if name.endswith(("events", "stat")):
                values[name] = {k:int(v) for k,v in (line.split() for line in p.read_text().splitlines())}
            else:
                values[name] = int(p.read_text())
    return values


class Consumer:
    def __init__(self, mask, raw_hz, preview_hz):
        self.mask, self.raw_hz, self.preview_hz = mask, raw_hz, preview_hz
        self.stopped = threading.Event()
        self.lock = threading.Lock()
        self.samples = []
        self.stale_ages_ms = []
        self.errors = []
        self.connection = None
        self.thread = threading.Thread(target=self.run, daemon=True)

    def run(self):
        cursor = (bytes(16), 0, 0)
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as connection:
                self.connection = connection
                connection.settimeout(3)
                connection.connect("/run/nuvo-depth/data.sock")
                subscription=struct.pack(">4sIIII", b"NVS2", self.mask,
                    round(self.raw_hz*1000), round(self.preview_hz*1000), 0)
                connection.sendall(subscription)
                if connection.recv(100) != b"NVA2":
                    raise RuntimeError("native subscription was rejected")
                if self.mask == 0:
                    # Keep the image-only lease alive across the server's
                    # ten-second idle timeout without requesting raw data.
                    while not self.stopped.wait(2):
                        connection.sendall(subscription)
                        if connection.recv(100) != b"NVA2":
                            raise RuntimeError("native preview heartbeat was rejected")
                    return
                while not self.stopped.is_set():
                    connection.sendall(struct.pack(">4sIII16sQQ", b"NVW2", self.mask, 200, 0, *cursor))
                    packet, ancillary, flags, _ = connection.recvmsg(1032, socket.CMSG_SPACE(4))
                    descriptors = []
                    for level, kind, data in ancillary:
                        if level==socket.SOL_SOCKET and kind==socket.SCM_RIGHTS:
                            items=array.array("i");items.frombytes(data[:len(data)//4*4]);descriptors.extend(items)
                    try:
                        if packet == b"NVN2":
                            continue
                        if packet[:4]!=b"NVR2" or len(descriptors)!=1:
                            raise RuntimeError("invalid native frame response")
                        length=struct.unpack_from(">Q",packet,8)[0]
                        wire=os.pread(descriptors[0],length,0)
                        frame=decode_frame(wire)
                        cursor=(bytes.fromhex(frame.run_uuid),frame.capture_epoch,frame.capture_sequence)
                        now=time.clock_gettime_ns(time.CLOCK_BOOTTIME)
                        age=(now-frame.metadata["capture_time_ns"])/1e6
                        if age<0 or age>1500:
                            self.stale_ages_ms.append(age)
                            continue
                        with self.lock:
                            self.samples.append({"time":time.monotonic(),"sequence":frame.capture_sequence,
                                "capture_ns":frame.metadata["capture_time_ns"],"age_ms":age,"wire_bytes":length})
                    finally:
                        for descriptor in descriptors:
                            os.close(descriptor)
        except Exception as error:
            if not self.stopped.is_set():
                self.errors.append(repr(error))

    def start(self):
        self.thread.start()

    def close(self):
        self.stopped.set()
        if self.connection is not None:
            try:self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:pass
        self.thread.join(timeout=4)


def cpu_ticks():
    values=[int(x) for x in Path("/proc/stat").read_text().splitlines()[0].split()[1:9]]
    return sum(values),values[3]+values[4]


def vmstat():
    wanted={"pswpin","pswpout","oom_kill","pgmajfault"}
    return {k:int(v) for k,v in (line.split() for line in Path("/proc/vmstat").read_text().splitlines()) if k in wanted}


def calibration_workload(attempts):
    """Run the real synthetic workflow with a bounded number of capture poses."""
    if not isinstance(attempts,int) or not 27<=attempts<=400:
        raise ValueError("calibration_attempts must be between 27 and 400")
    fixture=REPO/"tests/calibration_charuco_synthetic_test.py"
    return f'''import json,resource,sys,types,unittest
from pathlib import Path
sys.path.insert(0,{str(REPO/"tests")!r})
try:
 source=Path({str(fixture)!r}).read_text()
 needle='enumerate(poses):'
 assert source.count(needle)==1
 source=source.replace(needle,'enumerate([(*pose[:5],pose[5]+0.095*repeat) for repeat in range({math.ceil(attempts/27)}) for pose in poses][:{attempts}]):')
 solve='            solved = CALIBRATION.solve_web_session(state_root, session_id)'
 assert source.count(solve)==1
 source=source.replace(solve,"            print('PROFILE_CALIBRATION_DATA '+json.dumps({{'accepted_pairs':status['accepted_pairs'],'rejected_pairs':status['rejected_pairs']}}),flush=True)\\n"+solve)
 module=types.ModuleType('nuvo_resource_calibration_fixture')
 module.__file__={str(fixture)!r}
 exec(compile(source,module.__file__,'exec'),module.__dict__)
 result=unittest.TextTestRunner(verbosity=1).run(unittest.defaultTestLoader.loadTestsFromModule(module))
 raise SystemExit(0 if result.wasSuccessful() else 1)
finally:
 print('PROFILE_CHILD_USAGE '+json.dumps({{'peak_rss_kib':resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,'capture_attempts':{attempts}}}),flush=True)
'''


def sample(pids):
    memory=kv_bytes(Path("/proc/meminfo"))
    processes={}
    for name,pid in pids.items():
        try:
            data=kv_bytes(Path(f"/proc/{pid}/smaps_rollup"))
            data.update({"VmHWM":kv_bytes(Path(f"/proc/{pid}/status")).get("VmHWM",0)})
            processes[name]=data
        except (FileNotFoundError,ProcessLookupError):pass
    temp=int(Path("/sys/class/thermal/thermal_zone0/temp").read_text())/1000
    hz=int(Path("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq").read_text())*1000
    return {"time":time.monotonic(),"mem":memory,"processes":processes,"temperature_c":temp,
            "clock_hz":hz,"cpu":cpu_ticks(),"vmstat":vmstat(),"native_cgroup":cgroup_memory(NATIVE),
            "gateway_cgroup":cgroup_memory(GATEWAY)}


class Viewers:
    """Local TLS clients use the LAN address; client RAM is observer RAM.

    This exercises interface admission and server buffers, not radio throughput.
    The production gateway intentionally rejects the loopback interface.
    """
    def __init__(self, count, products):
        self.stopped=threading.Event();self.threads=[];self.streams=[];self.results=[]
        self.products=products
        route=json.loads(subprocess.check_output(
            ["ip","-j","-4","route","get","192.0.2.1"],text=True))[0]
        self.address=route.get("prefsrc",route.get("src"))
        if not self.address:
            raise RuntimeError("cannot determine the Pi LAN address")
        self.resolver=socket.getaddrinfo
        socket.getaddrinfo=lambda host,*a,**k:self.resolver(self.address if host=="nuvomow.local" else host,*a,**k)
        try:
            identity=Path("/var/lib/nuvo-depth/identity")
            self.client=NuvoClient(f"https://nuvomow.local:{GATEWAY_PORT}",ca_file=str(identity/"device.crt"),timeout=5)
            self.client.claim((identity/"claim.token").read_text().strip())
        except BaseException:
            socket.getaddrinfo=self.resolver
            raise
        for i in range(count):
            thread=threading.Thread(target=self.run,args=(i,),daemon=True)
            self.threads.append(thread);thread.start()

    def run(self,index):
        row={"index":index,"frames":0,"bytes":0,"error":None,"started_time":time.monotonic()}
        self.results.append(row)
        try:
            client=self.client
            if index==0 and self.products:
                stream=client.frames(cadence_hz=1,products=self.products,delivery="best_effort")
                receive=stream.__next__
            else:
                stream=_FrameStream(client,"/api/v1/encoded/stream?cadence_hz=1")
                method=_FrameStream.__next__
                receive=types.MethodType(types.FunctionType(method.__code__,dict(method.__globals__,decode_frame=decode_encoded_frame)),stream)
            with stream:
                self.streams.append(stream)
                while not self.stopped.is_set():
                    frame=receive()
                    row["frames"]+=1
                    row["last_frame_time"]=time.monotonic()
                    row["bytes"]+=sum(len(p.data) for p in frame.products.values()) if index==0 and self.products else len(frame.data)
        except Exception as error:
            if not self.stopped.is_set():
                row["error"]=str(error);row["error_time"]=time.monotonic()

    def close(self):
        self.stopped.set()
        for stream in self.streams:stream.close()
        for thread in self.threads:thread.join(timeout=6)
        socket.getaddrinfo=self.resolver


def apply_case(case, originals):
    subprocess.run(["systemctl","stop",NATIVE],check=True)
    daemon=originals[ETC/"daemon.yaml"].decode()
    daemon,count=re.subn(r"(?m)^(\s*target_hz:\s*)[0-9.]+",lambda m:m[1]+str(case["capture_hz"]),daemon)
    assert count==1
    daemon,count=re.subn(r"(?m)^(\s*buffer_count:\s*)\d+",lambda m:m[1]+str(8 if case["capture_hz"]>30 else 4),daemon)
    assert count==1
    if case.get("rectifier"):
        if case["rectifier"] not in {"cpu_opencv", "egl_gl_dmabuf"}:
            raise ValueError("unsupported rectifier")
        daemon,count=re.subn(r"(?m)(^rectification:\r?\n\s*backend:\s*)\S+",
            lambda m:m[1]+case["rectifier"],daemon)
        assert count==1
        fallback="cpu_opencv" if case["rectifier"]=="egl_gl_dmabuf" else "egl_gl_dmabuf"
        daemon,count=re.subn(r"(?m)^(\s*fallback_backend:\s*)\S+",
            lambda m:m[1]+fallback,daemon)
        assert count==1
    atomic_bytes(ETC/"daemon.yaml",daemon.encode())
    stereo=json.loads(originals[ETC/"stereo.json"])
    stereo.update(algorithm="sgbm",sgbm_mode=case.get("mode","3way"),block_size=5,
        min_disparity=0,max_disparity=case["disparities"],search_range=case["disparities"],
        median_filter_size=case.get("median",0),uniqueness_ratio=case.get("unique",0),
        opencv_threads=case.get("threads",2))
    atomic_bytes(ETC/"stereo.json",(json.dumps(stereo,separators=(",",":"),sort_keys=True)+"\n").encode())
    encoding=json.loads(originals[ETC/"encoding.json"])
    encoding.update(mode="both",codec=case.get("codec","h264"),source=case.get("preview_source","rectified_stereo_rgb"),
        cadence_hz=max(1,case.get("preview_hz",0)),threads=1,keyframe_interval=1,width=0,height=0,
        preset=case.get("encoding_preset",encoding["preset"]))
    atomic_bytes(ETC/"encoding.json",(json.dumps(encoding,separators=(",",":"),sort_keys=True)+"\n").encode())
    subprocess.run(["systemctl","start",NATIVE],check=True)
    for _ in range(40):
        try:
            if read_json(STATUS).get("capture_target_hz")==case["capture_hz"] and service_pid(NATIVE)>0:
                break
        except (OSError,ValueError):pass
        time.sleep(.1)
    time.sleep(1)
    return stereo,encoding


def run_case(case, output, originals):
    print("START",case["name"],json.dumps(case),flush=True)
    stereo,encoding=apply_case(case,originals)
    expected_daemon=(ETC/"daemon.yaml").read_bytes()
    if case.get("gateway_clients"):
        subprocess.run(["systemctl","start",GATEWAY],check=True)
        time.sleep(.4)
        viewers=Viewers(case["gateway_clients"],case.get("gateway_products",list(PRODUCT_FORMATS)))
    else:
        subprocess.run(["systemctl","stop",GATEWAY],check=True)
        viewers=None
    pid=service_pid(NATIVE)
    consumer=Consumer(case.get("mask",(1<<3)|(1<<5)),case.get("raw_hz",case["capture_hz"]),case.get("preview_hz",0))
    consumer.start()
    samples=[];health=[];encoded=[];seen=set();seen_encoded=set()
    started=time.monotonic();warmup=case.get("warmup",4)
    before=vmstat();calibration=None;calibration_status=None;calibration_log=None;current=None
    calibration_workspace=None
    measurement_finished=None
    native_cpu_before=int(subprocess.check_output(["systemctl","show",NATIVE,"-p","CPUUsageNSec","--value"],text=True))
    try:
        while time.monotonic()-started < case["seconds"]+warmup:
            elapsed=time.monotonic()-started
            if case.get("calibration") and elapsed>warmup+3 and calibration is None:
                calibration_log=(output/(case["name"]+"-calibration.log")).open("w")
                calibration_code=calibration_workload(case.get("calibration_attempts",27))
                calibration_workspace=tempfile.TemporaryDirectory(prefix=".calibration-work-",dir=output)
                calibration=subprocess.Popen(["python3","-c",calibration_code],stdout=calibration_log,
                    stderr=subprocess.STDOUT,env=dict(os.environ,TMPDIR=calibration_workspace.name))
            pids={"native":pid,"observer":os.getpid()}
            gateway=service_pid(GATEWAY)
            if gateway:pids["gateway"]=gateway
            if calibration is not None and calibration.poll() is None:
                pids["calibration"]=calibration.pid
                for p in Path("/proc").iterdir():
                    if not p.name.isdigit():continue
                    try:
                        parent=re.search(r"^PPid:\s+(\d+)", (p/"status").read_text(),re.M)
                        if parent and int(parent[1])==calibration.pid:pids["calibration"]=int(p.name)
                    except (OSError,ProcessLookupError):pass
            current=sample(pids)
            if ((ETC/"daemon.yaml").read_bytes()!=expected_daemon
                    or read_json(ETC/"stereo.json")!=stereo
                    or read_json(ETC/"encoding.json")!=encoding):
                raise RuntimeError("camera controls changed during measurement; discard this comparison")
            if elapsed>=warmup:
                samples.append(current)
                try:
                    h=read_json(STATUS)
                    key=(h.get("run_uuid"),h.get("processing_statistics_capture_sequence")
                         if case.get("mask",40) else h.get("capture_sequence"))
                    if key not in seen:
                        seen.add(key);health.append(h)
                    if case.get("preview_hz") and ENCODED.exists():
                        e=decode_encoded_frame(ENCODED.read_bytes())
                        age_ns=time.clock_gettime_ns(time.CLOCK_BOOTTIME)-e.metadata["capture_time_ns"]
                        if 0<=age_ns<1_500_000_000 and e.stream_sequence not in seen_encoded:
                            seen_encoded.add(e.stream_sequence)
                            encoded.append(dict(e.metadata,stream_sequence=e.stream_sequence,packet_bytes=len(e.data),observed_time=time.monotonic()))
                except (OSError,ValueError):pass
            minimum=case.get("minimum_available_mib",48)
            if current["mem"]["MemAvailable"]<minimum*MIB:
                raise RuntimeError(f"available RAM fell below {minimum} MiB; aborted before system exhaustion")
            if service_pid(NATIVE)!=pid:
                raise RuntimeError("native service restarted during load")
            time.sleep(.5)
    except BaseException as error:
        (output/(case["name"]+"-partial-samples.json")).write_text(json.dumps({
            "system":samples,"health":health,"encoded":encoded,"raw":consumer.samples})+"\n")
        (output/(case["name"]+"-failure.json")).write_text(json.dumps({
            "case":case,"error":repr(error),"last_sample":current,
            "elapsed_seconds":time.monotonic()-started},indent=2)+"\n")
        if calibration is not None and calibration.poll() is None:
            calibration.terminate()
        raise
    finally:
        measurement_finished=time.monotonic()
        if viewers:viewers.close()
        consumer.close()
        if calibration:
            try:calibration_status=calibration.wait(timeout=30)
            except subprocess.TimeoutExpired:
                calibration.terminate();calibration_status=calibration.wait(timeout=5)
            calibration_log.close()
        if calibration_workspace:calibration_workspace.cleanup()
    native_cpu_after=int(subprocess.check_output(["systemctl","show",NATIVE,"-p","CPUUsageNSec","--value"],text=True))
    cgroup_currents=[r["native_cgroup"]["memory.current"] for r in samples
                     if "memory.current" in r["native_cgroup"]]
    rows=[r for r in consumer.samples if started+warmup<=r["time"]<=measurement_finished]
    duration=samples[-1]["time"]-samples[0]["time"] if len(samples)>1 else 0
    first,last=samples[0],samples[-1]
    ticks=last["cpu"][0]-first["cpu"][0];idle=last["cpu"][1]-first["cpu"][1]
    result={"case":case,"stereo":stereo,"encoding":encoding,"samples":len(samples),
        "mem_total_mib":first["mem"]["MemTotal"]/MIB,
        "system_used_peak_mib":max((r["mem"]["MemTotal"]-r["mem"]["MemAvailable"])/MIB for r in samples),
        "mem_available_min_mib":min(r["mem"]["MemAvailable"]/MIB for r in samples),
        "native_rss_mib":stats([r["processes"]["native"]["Rss"]/MIB for r in samples]),
        "native_pss_mib":stats([r["processes"]["native"]["Pss"]/MIB for r in samples]),
        "native_cgroup_current_peak_mib":max(cgroup_currents)/MIB if cgroup_currents else None,
        "native_cgroup_events":last["native_cgroup"].get("memory.events"),
        "temperature_c":stats([r["temperature_c"] for r in samples]),
        "clock_hz":stats([r["clock_hz"] for r in samples]),
        "system_cpu_percent":100*(ticks-idle)/ticks if ticks else None,
        "native_cpu_core_percent":(native_cpu_after-native_cpu_before)/1e9/(time.monotonic()-started)*100,
        "raw_frames":len(rows),"raw_rate_hz":(len(rows)-1)/(rows[-1]["time"]-rows[0]["time"]) if len(rows)>1 else 0,
        "capture_rate_hz":(rows[-1]["sequence"]-rows[0]["sequence"])/((rows[-1]["capture_ns"]-rows[0]["capture_ns"])/1e9) if len(rows)>1 else None,
        "raw_capture_to_local_receive_ms":stats([r["age_ms"] for r in rows]),
        "raw_wire_bytes":stats([r["wire_bytes"] for r in rows]),
        "raw_sequence_gaps":sum(max(0,b["sequence"]-a["sequence"]-1) for a,b in zip(rows,rows[1:])),
        "consumer_errors":consumer.errors,"calibration_exit":calibration_status,
        "stale_raw_frames":len(consumer.stale_ages_ms),"stale_raw_age_ms":stats(consumer.stale_ages_ms),
        "viewer_clients":viewers.results if viewers else [],
        "gateway_admitted_clients":sum(r["frames"]>=2 for r in viewers.results) if viewers else 0,
        "observer_rss_peak_mib":max(r["processes"]["observer"]["Rss"]/MIB for r in samples),
        "gateway_rss_peak_mib":max((r["processes"].get("gateway",{}).get("Rss",0))/MIB for r in samples),
        "stages":{k:stats([h[k] for h in health if h.get("rectification_active")
                          and (k=="rectification_ms" or h.get("stereo_backend")!="not_requested")])
            for k in ["rectification_ms","stereo_match_ms","stereo_postprocess_ms","processing_ms"]},
        "encoded_unique_samples":len(encoded),"encode_ms":stats([e["encode_ms"] for e in encoded]),
        "vmstat_delta":{k:last["vmstat"][k]-before[k] for k in before},
        "throttle_flags":subprocess.check_output(["vcgencmd","get_throttled"],text=True).strip(),
        "duration_observed_seconds":duration}
    result["last_health"]=health[-1] if health else None
    result["native_numa_placement"]=numa_placement(pid)
    result["validation_failures"]=list(consumer.errors)
    if case.get("mask",40) and len(rows)<2 and not case.get("allow_no_fresh_raw",False):
        result["validation_failures"].append("requested raw stream did not produce fresh frames")
    if case.get("preview_hz") and len(encoded)<2:
        result["validation_failures"].append("requested preview did not produce fresh encoded frames")
    if viewers and not result["gateway_admitted_clients"]:
        result["validation_failures"].append("no requested TLS viewers received fresh frames")
    if viewers and not case.get("allow_viewer_drops",False):
        if result["gateway_admitted_clients"] != case["gateway_clients"]:
            result["validation_failures"].append("not all requested TLS viewers received fresh frames")
        for viewer in viewers.results:
            if viewer["error"]:
                result["validation_failures"].append(f"TLS viewer {viewer['index']}: {viewer['error']}")
    if calibration is not None and calibration_status!=0:
        result["validation_failures"].append("synthetic calibration workflow failed")
    if health and health[-1].get("encoding_configuration_error") and case.get("preview_hz"):
        result["validation_failures"].append(health[-1]["encoding_configuration_error"])
    if encoded and len(encoded)>1:
        result["encoded_rate_hz"]=(encoded[-1]["stream_sequence"]-encoded[0]["stream_sequence"])/((encoded[-1]["capture_time_ns"]-encoded[0]["capture_time_ns"])/1e9) if "stream_sequence" in encoded[-1] else None
    if calibration:
        text=(output/(case["name"]+"-calibration.log")).read_text()
        usage=re.search(r'PROFILE_CHILD_USAGE (\{[^\n]+\})',text)
        if usage:result["calibration_peak_rss_mib"]=json.loads(usage[1])["peak_rss_kib"]/1024
        dataset=re.search(r'PROFILE_CALIBRATION_DATA (\{[^\n]+\})',text)
        if dataset:result["calibration_dataset"]=json.loads(dataset[1])
    (output/(case["name"]+".json")).write_text(json.dumps(result,indent=2)+"\n")
    (output/(case["name"]+"-samples.json")).write_text(json.dumps({"system":samples,"health":health,"encoded":encoded,"raw":rows})+"\n")
    print("RESULT",json.dumps({k:result[k] for k in ["case","raw_rate_hz","system_used_peak_mib","native_rss_mib","temperature_c","throttle_flags"]}),flush=True)
    if result["validation_failures"]:
        raise RuntimeError("workload validation failed: "+"; ".join(result["validation_failures"]))
    return result


def main():
    global GATEWAY_PORT
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output",type=Path,required=True)
    parser.add_argument("--cases",type=Path,required=True)
    parser.add_argument("--benchmark-port",type=int,default=17443,
        help="temporary gateway port, keeping open UI clients out of the test; 0 keeps the normal port")
    args=parser.parse_args()
    if os.geteuid()!=0:parser.error("run as root")
    if args.benchmark_port and not 1024<=args.benchmark_port<=65535:
        parser.error("benchmark port must be 0 or between 1024 and 65535")
    lock=Path('/run/nuvo-depth-resource-profile.lock').open('a')
    os.fchmod(lock.fileno(),0o600)
    try:fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    except BlockingIOError:parser.error("another resource profile is already running")
    if (args.output/"originals.json").exists():
        parser.error("output already contains a run; choose a fresh evidence directory")
    args.output.mkdir(parents=True,exist_ok=True)
    paths=[ETC/name for name in ["daemon.yaml","stereo.json","encoding.json"]]
    originals={p:p.read_bytes() for p in paths}
    native_was_running=service_pid(NATIVE)>0
    gateways_was_running=service_pid(GATEWAY)>0
    (args.output/"originals.json").write_text(json.dumps({str(p):v.decode() for p,v in originals.items()},indent=2))
    initial={"meminfo":kv_bytes(Path("/proc/meminfo")),"cmdline":Path("/proc/cmdline").read_text(),
        "cgroup_controllers":Path("/sys/fs/cgroup/cgroup.controllers").read_text(),
        "source_commit":Path("/usr/share/nuvo-depth/source-commit").read_text().strip(),
        "boot_id":Path("/proc/sys/kernel/random/boot_id").read_text().strip(),
        "swap":Path("/proc/swaps").read_text(),
        "profiler_sha256":hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "benchmark_port":args.benchmark_port}
    (args.output/"profiler-source.py").write_bytes(Path(__file__).read_bytes())
    (args.output/"system.json").write_text(json.dumps(initial,indent=2))
    def stop(*_):raise KeyboardInterrupt()
    signal.signal(signal.SIGTERM,stop)
    results=[]
    override=Path('/run/systemd/system/nuvo-gateway.service.d/99-nuvo-resource-profile-port.conf')
    override_owned=False
    try:
        subprocess.run(["systemctl","stop",GATEWAY],check=True)
        if args.benchmark_port:
            if override.exists():raise RuntimeError("a gateway profiling override already exists")
            command=subprocess.check_output(["systemctl","show",GATEWAY,"-p","ExecStart","--value"],text=True)
            match=re.search(r'argv\[\]=([^;]+);',command)
            if not match:raise RuntimeError("cannot inspect gateway command")
            arguments=shlex.split(match[1])
            if arguments[0]!='/usr/lib/nuvo-depth/nuvo-gateway' or arguments.count('--port')!=1:
                raise RuntimeError("unexpected gateway command")
            arguments[arguments.index('--port')+1]=str(args.benchmark_port)
            with socket.socket() as probe:probe.bind(('0.0.0.0',args.benchmark_port))
            override.parent.mkdir(parents=True,exist_ok=True)
            override_owned=True
            override.write_text('[Service]\nExecStart=\nExecStart='+' '.join(shlex.quote(a) for a in arguments)+'\n')
            subprocess.run(['systemctl','daemon-reload'],check=True)
            GATEWAY_PORT=args.benchmark_port
        for case in read_json(args.cases):
            results.append(run_case(case,args.output,originals))
        (args.output/"summary.json").write_text(json.dumps(results,indent=2)+"\n")
    except BaseException as error:
        (args.output/"failure.json").write_text(json.dumps({
            "error":repr(error),"completed_cases":len(results)},indent=2)+"\n")
        raise
    finally:
        subprocess.run(["systemctl","stop",GATEWAY],check=True)
        subprocess.run(["systemctl","stop",NATIVE],check=True)
        for path,value in originals.items():atomic_bytes(path,value)
        if override_owned:
            override.unlink(missing_ok=True)
            subprocess.run(['systemctl','daemon-reload'],check=True)
        subprocess.run(["systemctl","start" if native_was_running else "stop",NATIVE],check=True)
        subprocess.run(["systemctl","start" if gateways_was_running else "stop",GATEWAY],check=True)
        assert all(p.read_bytes()==v for p,v in originals.items())
        (args.output/"restored.json").write_text(json.dumps({"settings_restored":True,"completed_cases":len(results)}))


if __name__=="__main__":main()
