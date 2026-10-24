#!/usr/bin/env python3
"""Check/generate current setup references without rewriting historical evidence."""
from pathlib import Path
import argparse,hashlib,inspect,json,re,subprocess,sys,urllib.request
ROOT=Path(__file__).resolve().parents[1]

def approved_sources():
    reference=json.loads((ROOT/'deployment/public-release.json').read_text(encoding='utf-8'))
    if reference.get('approved') is not True or not re.fullmatch(r'v\d+\.\d+\.\d+(?:-beta\.[1-9]\d*)?',reference.get('tag','')):
        raise ValueError('an explicit approved public reference is required')
    result={'approval':reference}
    for key,name in [('platform','deployment/release-platform.json'),('profiles','web/depth-profiles.json')]:
        expected=reference[key+'_sha256'];data=(ROOT/name).read_bytes().replace(b'\r\n',b'\n')
        if hashlib.sha256(data).hexdigest()!=expected:
            try:data=subprocess.check_output(['git','show',reference['tag']+':'+name],cwd=ROOT)
            except subprocess.CalledProcessError:
                with urllib.request.urlopen(f"https://raw.githubusercontent.com/{reference['repository']}/{reference['tag']}/{name}",timeout=20) as response:data=response.read(65537)
        if len(data)>65536 or hashlib.sha256(data).hexdigest()!=expected:raise ValueError('approved source digest mismatch: '+name)
        result[key]=json.loads(data)
    return result

def block(value):
    return '''<!-- BEGIN APPROVED SETUP -->
Approved public release: **`TAG`**. Start with **PROFILE**.
Use Raspberry Pi OS Lite 64-bit Trixie (`ARCH`), kernel `KERNEL`.
The rolling command follows this approved reference; development builds and
draft candidates are never selected automatically. Quality is an optional
profile with greater processing cost.
<!-- END APPROVED SETUP -->'''.replace('TAG',value['approval']['tag']).replace('PROFILE',value['profiles']['default_profile'].title()).replace('ARCH',value['platform']['architecture']).replace('KERNEL',', '.join(value['platform']['kernels']))

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--write',action='store_true');parser.add_argument('--wiki',type=Path);args=parser.parse_args()
    source=approved_sources();expected=block(source)
    for name in ['README.md','docs/install.md','docs/installer-reference.md']:
        path=ROOT/name;text=path.read_text(encoding='utf-8')
        if args.write:
            text=re.sub(r'<!-- BEGIN APPROVED SETUP -->.*?<!-- END APPROVED SETUP -->',lambda _:expected,text,flags=re.S);path.write_text(text,encoding='utf-8')
        if expected not in text:raise ValueError('setup reference drift: '+name)
        if re.search(r'releases/download/v0\.1\.1-beta\.5/install-nuvo-depth',text):raise ValueError('obsolete current installation command: '+name)
    import nuvo_release
    for name in ['install-nuvo-depth.sh','install-nuvo-depth-minimal.sh']:
        path=ROOT/name;text=path.read_text(encoding='utf-8');start='# BEGIN PUBLIC RELEASE RESOLVER\n';end='# END PUBLIC RELEASE RESOLVER'
        if args.write:
            text=text.split(start)[0]+start+inspect.getsource(nuvo_release.latest_release)+end+text.split(end,1)[1];path.write_text(text,encoding='utf-8',newline='\n')
        if text.split(start,1)[1].split(end,1)[0]!=inspect.getsource(nuvo_release.latest_release):raise ValueError('bootstrap resolver drift: '+name)
    html=(ROOT/'web/index.html').read_text(encoding='utf-8')
    if 'Claim the device' in html or 'Start with Quality' in html:raise ValueError('obsolete browser setup guidance')
    if f"Start with the default {source['profiles']['default_profile'].title()} profile" not in html:raise ValueError('console starting profile drift')
    actual=json.loads((ROOT/'config/stereo.json').read_text(encoding='utf-8'));catalog=source['profiles'];controls=catalog['base_controls']|catalog['profiles'][catalog['default_profile']]['controls']
    for key in ['sgbm_mode','opencv_threads','search_range','median_filter_size']:
        if actual[key]!=controls[key]:raise ValueError('runtime/profile catalog drift: '+key)
    if args.wiki:
        path=args.wiki/'setup-reference.json'
        if args.write:path.write_text(json.dumps(source,indent=2)+'\n',encoding='utf-8')
        if json.loads(path.read_text(encoding='utf-8'))!=source:raise ValueError('wiki setup snapshot is stale; synchronize from the approved authority')
    print('Approved release, platform, starting profile and browser instructions agree.')

if __name__=='__main__':main()
