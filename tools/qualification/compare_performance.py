#!/usr/bin/env python3
"""Conservative frozen-budget comparison; no physical/accuracy claim implied."""
import argparse
import json
import math
from pathlib import Path

def compare(baseline,candidate,thresholds):
    if baseline.get('conditions_sha256')!=candidate.get('conditions_sha256') or len(str(baseline.get('conditions_sha256','')))!=64:raise ValueError('Comparison conditions are not identical/frozen')
    for cohort in (baseline,candidate):
        if not cohort.get('source_commit') or not cohort.get('artifact_sha256') or len(cohort.get('runs',[]))<3:raise ValueError('Need exact cohorts and at least three measured runs')
        for run in cohort['runs']:
            if run.get('warmup_seconds',0)<300 or run.get('seconds',0)<600:raise ValueError('Need five-minute warmup and ten-minute measurement per run')
            for key in ('depth_hz','latency_p95_ms','rss_steady_bytes'):
                if type(run.get(key)) not in (int,float) or not math.isfinite(run[key]) or run[key]<=0:raise ValueError('Missing/invalid measurement: '+key)
    checks={}
    for key,minimum,budget in (('depth_hz',True,thresholds['depth_rate_min_ratio']),('latency_p95_ms',False,thresholds['latency_p95_max_ratio']),('rss_steady_bytes',False,thresholds['steady_rss_max_ratio'])):
        before=[run[key] for run in baseline['runs']];after=[run[key] for run in candidate['runs']]
        ratio=min(after)/max(before) if minimum else max(after)/min(before)
        checks[key]={'conservative_ratio':ratio,'budget':budget,'pass':ratio>=budget if minimum else ratio<=budget}
    checks['errors']={'pass':all(run.get('new_core_errors')==0 and run.get('throttled')==0 and run.get('swap_bytes')==0 for run in candidate['runs'])}
    return {'scope':'Declared matched cohorts and measured workload only; quality, physical SKU, absolute product thresholds and soak remain separate gates.',
            'method':'Worst observed run ratio; noisy threshold crossings require extended measurement, never favorable sample selection.',
            'checks':checks,'pass':all(check['pass'] for check in checks.values())}

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('baseline',type=Path);parser.add_argument('candidate',type=Path)
    parser.add_argument('--thresholds',type=Path,default=Path(__file__).resolve().parents[2]/'qualification/onboarding/performance-thresholds.json')
    args=parser.parse_args()
    try:result=compare(*(json.loads(path.read_text()) for path in (args.baseline,args.candidate,args.thresholds)))
    except (OSError,ValueError,KeyError,TypeError) as error:parser.exit(1,f'Comparison rejected: {error}\n')
    print(json.dumps(result,indent=2));raise SystemExit(0 if result['pass'] else 1)

if __name__=='__main__':main()
