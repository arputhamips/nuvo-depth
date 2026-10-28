"""Export the canonical SDK dependency closure, with exact provenance and checksums."""
from pathlib import Path
import argparse, hashlib, json, subprocess
ROOT=Path(__file__).resolve().parents[1]
def source_bytes(path):
    # The declared closure is text. Pin Git-equivalent LF bytes so a Windows
    # export and a clean Ubuntu checkout have identical source/checksum data.
    return path.read_bytes().replace(b'\r\n',b'\n')
def inventory():
    sdk=ROOT/'sdk/cpp'
    names=['CMakeLists.txt','README.md','cmake/nuvo_depth_clientConfig.cmake.in',
           'include/nuvo/client.hpp','src/frame.cpp','src/local.cpp','src/calibration.cpp','src/replay.cpp',
           'src/https.cpp','src/https_disabled.cpp','tests/client_test.cpp',
           'tests/transports.py','tests/installed.py','tests/measurements.py','tests/calibration.py']
    files={name:sdk/name for name in names}
    files.update({'src/wire_protocol.cpp':ROOT/'src/wire_protocol.cpp','private/depthd/wire_protocol.hpp':ROOT/'include/depthd/wire_protocol.hpp','LICENSE':ROOT/'LICENSE','NOTICE':ROOT/'NOTICE'})
    return files
def main():
    parser=argparse.ArgumentParser();parser.add_argument('destination',type=Path);parser.add_argument('--check',action='store_true');args=parser.parse_args()
    files=inventory();manifest={'schema_version':1,'upstream_repository':'https://github.com/arputhamips/nuvo-depth','upstream_base_commit':subprocess.check_output(['git','-C',str(ROOT),'rev-parse','HEAD'],text=True).strip(),'sdk_version':'0.1.0','files':{name:{'upstream_path':p.relative_to(ROOT).as_posix(),'sha256':hashlib.sha256(source_bytes(p)).hexdigest()} for name,p in sorted(files.items())}}
    manifest['source_normalization']='Declared text closure exported with LF line endings'
    manifest['source_state']='content-addressed working tree; base commit is not an assertion that exported changes are committed'
    manifest['content_sha256']=hashlib.sha256(json.dumps(manifest['files'],sort_keys=True,separators=(',',':')).encode()).hexdigest()
    if args.check:
        actual=json.loads((args.destination/'SOURCE-MANIFEST.json').read_text())
        if actual!=manifest:
            raise SystemExit('SDK snapshot provenance differs; regenerate export')
        for name,record in manifest['files'].items():
            if hashlib.sha256((args.destination/name).read_bytes()).hexdigest()!=record['sha256']:
                raise SystemExit('SDK snapshot checksum mismatch: '+name)
        if {p.relative_to(args.destination).as_posix() for p in args.destination.rglob('*') if p.is_file() and '__pycache__' not in p.relative_to(args.destination).parts}!=set(files)|{'SOURCE-MANIFEST.json'}:
            raise SystemExit('unexpected snapshot files')
    else:
        if args.destination.exists():raise SystemExit('Destination must not exist; export to a fresh directory for review')
        for name,p in files.items():target=args.destination/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(source_bytes(p))
        (args.destination/'SOURCE-MANIFEST.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8',newline='\n')
if __name__=='__main__':main()
