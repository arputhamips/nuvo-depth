"""Fail-safe CI routing: only documented content can omit runtime checks."""
import argparse
from pathlib import PurePosixPath
import re
import subprocess

def classify(paths):
    def documentation(path):
        suffix=PurePosixPath(path).suffix.lower()
        return ((path.startswith('docs/') and suffix in ('.json','.png','.jpg','.svg','.csv')) or suffix in ('.md','.rst')
                or path=='deployment/public-release.json')
    runtime = not paths or not all(documentation(path) for path in paths)
    return {'native':runtime,'browser':runtime,'docs_only':not runtime}

if __name__=='__main__':
    parser=argparse.ArgumentParser(); parser.add_argument('--base',default=''); args=parser.parse_args()
    paths=[]
    if re.fullmatch('[a-f0-9]{40}',args.base) and args.base!='0'*40:
        result=subprocess.run(['git','diff','--name-only',args.base,'HEAD'],capture_output=True,text=True,check=False)
        if result.returncode==0: paths=result.stdout.splitlines()
    for key,value in classify(paths).items(): print(f'{key}={str(value).lower()}')
