"""Read-only diagnostics and an allowlisted, credential-free support schema."""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import re
import subprocess

import nuvo_calibration_control as calibration

PACKAGES = ('nuvo-depth-minimal','nuvo-depth','nuvo-depth-core','nuvo-depth-platform-pi5',
            'nuvo-depth-gateway','nuvo-depth-tools','nuvo-depth-calibration','libnuvo-depth1','python3-nuvo-depth')
STATES = ('calibration_accepted','preview_enabled','calibration_needed','unavailable')
AUTHORITIES = ('user_validated','factory_validated','provisional_recalibration_required')
VERSION = re.compile(r'\d+\.\d+\.\d+(?:[~+.-][A-Za-z0-9.+~-]{1,60})?\Z')
MMC_ERROR_LABELS = {
    'Command Timeout Occurred':'command_timeout',
    'Command CRC Errors Occurred':'command_crc',
    'Data Timeout Occurred':'data_timeout',
    'Data CRC Errors Occurred':'data_crc',
    'Auto-Cmd Error Occurred':'auto_command',
    'ADMA Error Occurred':'adma',
    'Tuning Error Occurred':'tuning',
    'CMDQ RED Errors':'cmdq_red',
    'CMDQ GCE Errors':'cmdq_gce',
    'CMDQ ICCE Errors':'cmdq_icce',
    'Request Timedout':'request_timeout',
    'CMDQ Request Timedout':'cmdq_request_timeout',
    'ICE Config Errors':'ice_configuration',
    'Controller Timedout errors':'controller_timeout',
    'Unexpected IRQ errors':'unexpected_irq',
}
MMC_STATS_LIMIT = 4096
MMC_ERROR_MAX = 2**32-1

def mmc_error_counts(value):
    """Validate the entire counter schema before allowing any export."""
    if not isinstance(value,dict) or set(value) != set(MMC_ERROR_LABELS.values()):
        return None
    if any(type(count) is not int or not 0<=count<=MMC_ERROR_MAX for count in value.values()):
        return None
    return {key:value[key] for key in MMC_ERROR_LABELS.values()}

def parse_mmc_errors(text):
    # Kernel debugfs err_stats labels are fixed; never retain arbitrary lines.
    if not isinstance(text,str) or len(text)>MMC_STATS_LIMIT:
        return None
    counts = {}
    for line in text.splitlines():
        if not line.strip():
            continue
        match = re.fullmatch(r'# ([^:\r\n]+):[ \t]*([0-9]{1,10})',line)
        if not match or match[1] not in MMC_ERROR_LABELS:
            return None
        key = MMC_ERROR_LABELS[match[1]]
        if key in counts:
            return None
        counts[key] = int(match[2])
    return mmc_error_counts(counts)

def read_mmc_errors(path):
    """Optional read only: do not mount debugfs, reset counters or probe hardware."""
    try:
        if path.is_symlink() or not path.is_file():
            return None
        # debugfs files report a zero size; bound the read itself.
        with path.open('rb') as source:
            raw = source.read(MMC_STATS_LIMIT+1)
        return parse_mmc_errors(raw.decode('ascii')) if len(raw)<=MMC_STATS_LIMIT else None
    except (OSError,UnicodeError):
        return None

def small(path, limit=65536):
    try:
        if path.is_symlink() or not path.is_file() or path.stat().st_size > limit:
            return None
        return path.read_text()
    except (OSError, UnicodeError):
        return None

def object_file(path):
    try:
        value = json.loads(small(path) or 'null')
        return value if isinstance(value,dict) else {}
    except (ValueError,TypeError):
        return {}

def os_release(root):
    path = root/'etc/os-release'
    # Debian/Pi OS normally links this file to /usr/lib/os-release. Accept
    # only that standard target; other diagnostic inputs remain no-follow.
    if path.is_symlink():
        target = root/'usr/lib/os-release'
        try:
            if path.resolve() != target.absolute() or target.is_symlink():
                return None
        except OSError:
            return None
        return small(target)
    return small(path)

def number(value):
    return value if type(value) in (int,float) and math.isfinite(value) and value >= 0 else None

def collect(root=Path('/'), profile='appliance', run=subprocess.run):
    root = Path(root)
    health = calibration.daemon_health(root,run)
    ready = calibration.readiness(root,run)
    os_values = {}
    for line in (os_release(root) or '').splitlines():
        if '=' in line:
            key,value = line.split('=',1); os_values[key] = value.strip('"')
    packages = {}
    service = 'unknown'
    try:
        result = run(['dpkg-query','-W','-f=${binary:Package}\t${Version}\t${db:Status-Status}\n',*PACKAGES],
                     capture_output=True,text=True,timeout=8,check=False)
        for line in result.stdout.splitlines():
            parts = line.split('\t')
            if len(parts)==3 and parts[2]=='installed':
                packages[parts[0].split(':')[0]] = parts[1]
        result = run(['systemctl','is-active',calibration.SERVICE],capture_output=True,text=True,timeout=8,check=False)
        service = result.stdout.strip()
    except (OSError,subprocess.SubprocessError):
        pass
    cameras = None
    if health.get('status')=='running':
        ids = [health.get(key) for key in ('left_camera_id','right_camera_id')]
        if all(isinstance(item,str) and item for item in ids): cameras = len(set(ids))
    else:
        binary = root/'usr/bin/rpicam-hello'
        if binary.is_file():
            try:
                result = run([str(binary),'--list-cameras'],capture_output=True,text=True,timeout=8,check=False)
                if result.returncode==0:
                    cameras = len(re.findall(r'^\s*\d+\s*:\s*imx296',result.stdout,re.M|re.I))
            except (OSError,subprocess.SubprocessError):
                pass
    directory = root/'usr/share/nuvo-depth/installation'
    return build_report({'profile':profile,'os_id':os_values.get('ID'),'codename':os_values.get('VERSION_CODENAME'),
                         'model':small(root/'proc/device-tree/model'),
                         'kernel':(small(root/'proc/sys/kernel/osrelease') or '').strip(),
                         'platform':object_file(directory/'release-platform.json'),
                         'approval':object_file(directory/'public-release.json'),
                         'mmc_errors':read_mmc_errors(root/'sys/kernel/debug/mmc0/err_stats'),
                         'trigger':small(root/'sys/module/imx296/parameters/trigger_mode'),
                         'service':service,'camera_count':cameras,'health':health,'readiness':ready,
                         'encoding':object_file(root/'etc/nuvo-depth/encoding.json'),'versions':packages})

def build_report(inputs):
    """Only explicit enums, bounded numbers, checked hashes/versions leave here.

    Raw health/config/errors/logs and free-form source strings are NEVER copied.
    This is also the browser export surface, not a best-effort key-name scrubber.
    """
    profile = inputs.get('profile') if inputs.get('profile') in ('appliance','minimal') else 'unknown'
    ready = inputs.get('readiness') if isinstance(inputs.get('readiness'),dict) else {}
    health = inputs.get('health') if isinstance(inputs.get('health'),dict) else {}
    state = ready.get('state') if ready.get('state') in STATES else 'unavailable'
    checks = []
    def check(identity,status,reason,action):
        checks.append({'id':identity,'status':status,'reason':reason,'next_action':action})
    codename = inputs.get('codename')
    model = inputs.get('model')
    platform_ok = codename=='trixie' and isinstance(model,str) and model.startswith('Raspberry Pi 5')
    known_platform = codename is not None and model is not None
    check('platform.os','pass' if platform_ok else 'fail' if known_platform else 'unknown',
          'Supported Pi 5/Trixie platform.' if platform_ok else 'Platform is unsupported or could not be identified.',
          'Use the approved Pi OS image and supported dual-IMX296 head; inspect /etc/os-release and device model.')
    manifest = inputs.get('platform') if isinstance(inputs.get('platform'),dict) else {}
    kernel = inputs.get('kernel')
    kernels = manifest.get('kernels')
    kernel_known = isinstance(kernel,str) and bool(kernel) and isinstance(kernels,list) and bool(kernels)
    kernel_ok = kernel_known and kernel in kernels
    check('platform.kernel','pass' if kernel_ok else 'fail' if kernel_known else 'unknown',
          'Kernel matches the package cohort.' if kernel_ok else 'Kernel qualification is unavailable or does not match.',
          'Compare uname -r with the selected release prerequisites; do not perform an unqualified kernel upgrade.')
    mmc_errors = mmc_error_counts(inputs.get('mmc_errors'))
    storage_errors = mmc_errors is not None and any(mmc_errors.values())
    check('platform.storage','unknown' if mmc_errors is None else 'warning' if storage_errors else 'pass',
          'MMC controller counters are unavailable or malformed; storage health is unconfirmed.' if mmc_errors is None else
          'MMC controller errors are recorded since boot or the last counter reset; recovered errors may be absent from the journal.' if storage_errors else
          'No MMC controller errors are recorded since boot or the last counter reset; storage health is unverified.',
          'Read existing /sys/kernel/debug/mmc0/err_stats with sudo and inspect kernel I/O logs; check the SD card and power, then retest startup. Do not reset the counters before collecting evidence.')
    count = inputs.get('camera_count')
    count = count if type(count) is int and 0<=count<=16 else None
    check('capture.cameras','pass' if count==2 else 'unknown' if count is None else 'fail',
          'Two distinct IMX296 cameras are enumerated.' if count==2 else 'The required camera pair could not be established.',
          'Run rpicam-hello --list-cameras; power off before checking both camera cables and sensor compatibility.')
    trigger = inputs.get('trigger')
    check('capture.trigger','pass' if trigger is not None and trigger.strip()=='1' else 'unknown' if trigger is None else 'fail',
          'External trigger mode is enabled.' if trigger is not None and trigger.strip()=='1' else 'External trigger readiness is unconfirmed.',
          'Complete the installer-requested reboot; inspect the managed camera configuration and approved trigger wiring.')
    service = inputs.get('service') if inputs.get('service') in ('active','inactive','failed','activating','deactivating') else 'unknown'
    enabled = ready.get('capture_enabled') is True
    service_status = 'pass' if service=='active' else 'warning' if not enabled and service=='inactive' else 'unknown' if service=='unknown' else 'fail'
    check('capture.service',service_status,
          'Capture service is active.' if service=='active' else 'Capture is deliberately disabled.' if not enabled and service=='inactive' else 'Capture service needs inspection.',
          'Inspect sudo nuvoctl calibration status and systemctl status nuvo-depthd-native.service; service activity alone is not fresh-depth verification.')
    check('calibration.readiness','pass' if state=='calibration_accepted' else 'warning' if state in ('preview_enabled','calibration_needed') else 'unknown',
          {'calibration_accepted':'A compatible bundle passed declared validation; metric accuracy is a separate gate.',
           'preview_enabled':'Preview policy is enabled; distance accuracy is unverified.',
           'calibration_needed':'Capture is waiting for calibration or an allowed preview selection.',
           'unavailable':'Calibration readiness could not be established.'}[state],
          'Use sudo nuvoctl calibration status; import the original matching bundle or select preview on supported hardware.')
    active = health.get('matcher_active')
    check('output.depth','pass' if active is True else 'warning' if state=='calibration_needed' else 'unknown' if health.get('status')!='running' else 'pass',
          'Depth processing is active.' if active is True else 'The demand-driven matcher can be healthy and idle with no depth reader.' if health.get('status')=='running' else 'Fresh output is unconfirmed.',
          'Request depth and validity, then verify advancing associated frame identities and valid pixels.')
    encoding = inputs.get('encoding') if isinstance(inputs.get('encoding'),dict) else {}
    mode = encoding.get('mode') if encoding.get('mode') in ('raw','encoded','both') else None
    check('output.monitoring','warning' if profile=='minimal' or mode=='raw' else 'pass' if mode else 'unknown',
          'Minimal intentionally omits browser/network SDK, Wi-Fi onboarding, solver and monitoring encoder.' if profile=='minimal' else 'The optional monitoring encoder is disabled.' if mode=='raw' else 'Monitoring configuration is available.' if mode else 'Monitoring configuration is unavailable.',
          'Use local nuvoctl depth capture on minimal.' if profile=='minimal' else 'Enable only the products needed for the task; raw depth does not require monitoring.')
    check('network.budget','warning' if profile=='minimal' else 'unknown',
          'Minimal intentionally has no network frame service.' if profile=='minimal' else 'No current measured link-budget qualification was collected by this report.',
          'Retain the configured SSH/network connection.' if profile=='minimal' else 'Inspect Streaming connection/admission information; begin with depth plus validity at 1 Hz best effort.')
    versions = {name:value for name,value in (inputs.get('versions') or {}).items()
                if name in PACKAGES and isinstance(value,str) and VERSION.fullmatch(value)}
    check('software.versions','pass' if versions else 'unknown',
          'Installed package versions were read.' if versions else 'Installed package versions are unavailable.',
          'Inspect dpkg-query; use a complete supported signed cohort, not individually mixed packages.')
    approval = inputs.get('approval') if isinstance(inputs.get('approval'),dict) else {}
    tag = approval.get('tag')
    approved = approval.get('approved') is True and approval.get('repository')=='arputhamips/nuvo-depth' and isinstance(tag,str) and re.fullmatch(r'v\d+\.\d+\.\d+(?:-beta\.\d+)?',tag)
    command = None
    if approved:
        script = 'install-nuvo-depth-minimal.sh' if profile=='minimal' else 'install-nuvo-depth.sh'
        command = f'curl -fsSL https://github.com/arputhamips/nuvo-depth/releases/download/{tag}/{script} | sudo bash'
    authority = ready.get('authority') if ready.get('authority') in AUTHORITIES else None
    bundle = ready.get('calibration_sha256')
    bundle = bundle if isinstance(bundle,str) and re.fullmatch('[a-f0-9]{64}',bundle) else None
    return {'schema_version':1,'report_type':'nuvo-depth-support','generated_at':datetime.now(timezone.utc).isoformat(),
            'profile':profile,'checks':checks,'versions':versions,
            'storage':{'mmc_error_counts':mmc_errors,'counter_scope':'since_boot_or_reset'},
            'calibration':{'state':state,'authority':authority,'calibration_sha256':bundle,
                           'loaded':ready.get('loaded') if type(ready.get('loaded')) is bool else None},
            'output':{'camera_count':count,'service':service,'mode':mode,
                      **{key:number(health.get(key)) for key in ('capture_target_hz','captured_pairs','matched_frames','capture_sequence','processing_ms')}},
            'upgrade':{'approved_tag':tag if approved else None,'availability':'bundled_approval_only' if approved else 'unknown',
                       'command':command,'notice':'Release availability was not queried. Review release prerequisites; implicit downgrades and mixed cohorts are refused. An installed candidate can be newer than this public reference. No browser updater or untested package rollback is offered.'},
            'privacy':{'raw_logs_included':False,'automatic_upload':False,'free_form_configuration_included':False}}

def human(report):
    return '\n'.join(f"{item['status'].upper():7} {item['id']}: {item['reason']} Next: {item['next_action']}" for item in report['checks'])

def add_command(commands):
    parser = commands.add_parser('diagnostics',help='Read-only diagnostics and allowlisted support export')
    parser.add_argument('--json',action='store_true')
    parser.add_argument('--output',type=Path,help='Create a new support JSON file; never overwrite an existing file')

def dispatch(args,root,profile):
    report = collect(root,profile)
    if args.output:
        with args.output.open('x',encoding='utf-8') as output:
            json.dump(report,output,indent=2,allow_nan=False); output.write('\n')
    print(json.dumps(report,indent=2,allow_nan=False) if args.json else human(report))
