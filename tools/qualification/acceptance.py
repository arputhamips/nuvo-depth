#!/usr/bin/env python3
"""Validate onboarding evidence; a structurally valid record is not acceptance."""
import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re

ROOT=Path(__file__).resolve().parents[2]
REFERENCE=ROOT/'qualification/onboarding/acceptance-template.json'
STATUSES={'not_run','pass','fail','blocked','not_applicable'}

def load(path):return json.loads(Path(path).read_text(encoding='utf-8'))
def digest(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def validate(record,base):
    reference=load(REFERENCE)
    expected={test['test_id']:test for test in reference['tests']}
    tests=record.get('tests',[])
    ids=[test.get('test_id') for test in tests]
    if len(ids)!=len(set(ids)) or set(ids)!=set(expected):raise ValueError('All 69 test IDs must occur exactly once')
    if record.get('specification_sha256')!=reference['specification_sha256']:raise ValueError('Specification digest changed')
    frozen=record.get('candidate',{}).get('source_commit')
    for test in tests:
        original=expected[test['test_id']]
        for key in ('execution_class','required_for_final_acceptance','procedure','expected_result'):
            if test.get(key)!=original[key]:raise ValueError(f"{test['test_id']}: requirement/scope was changed")
        status=test.get('status')
        if status not in STATUSES:raise ValueError('Unknown status: '+str(status))
        runs=test.get('runs',[])
        if status!='not_run' and not runs:raise ValueError(f"{test['test_id']}: missing attempt")
        run_ids=set()
        for run in runs:
            if not run.get('run_id') or run['run_id'] in run_ids:raise ValueError('Missing/duplicate attempt ID')
            run_ids.add(run['run_id'])
            if run.get('status') not in STATUSES-{'not_run'}:raise ValueError('Invalid attempt status')
            if run['status']=='blocked' and not run.get('notes'):raise ValueError('Blocked attempt must name missing input')
            if run['status'] in ('pass','not_applicable'):
                for key in ('source_commit','candidate_manifest','environment_manifest','procedure_revision','started_at','finished_at','observed_values','criteria','evidence','reviewer'):
                    if not run.get(key):raise ValueError(f"{test['test_id']}: missing {key}")
                if not frozen or not re.fullmatch('[a-f0-9]{40}',frozen) or run['source_commit']!=frozen:raise ValueError('Evidence is not from the frozen candidate')
                start,end=(datetime.fromisoformat(run[key]) for key in ('started_at','finished_at'))
                if start.tzinfo is None or end.tzinfo is None or end<start:raise ValueError('Invalid timezone/attempt chronology')
                if original['execution_class']=='physical' and not run.get('hardware_manifest'):raise ValueError('Physical evidence needs hardware identity')
                if run['status']=='not_applicable' and not run.get('support_scope_rationale'):raise ValueError('Not applicable needs reviewed support-scope rationale')
                for artifact in run['evidence']:
                    if not isinstance(artifact,dict) or not re.fullmatch('[a-f0-9]{64}',artifact.get('sha256','')):raise ValueError('Evidence needs path/URL and SHA-256')
                    if 'path' in artifact:
                        path=(base/artifact['path']).resolve()
                        if not path.is_relative_to(base.resolve()) or not path.is_file() or digest(path)!=artifact['sha256']:raise ValueError('Missing/changed evidence: '+artifact['path'])
                    elif not str(artifact.get('url','')).startswith('https://'):raise ValueError('Evidence requires a local file or HTTPS artifact URL')
        if runs and status!=runs[-1]['status']:raise ValueError('Aggregate status must match latest retained attempt')
    return tests

def gate(record,tests,name):
    classes={'software','clean_environment','client_environment','published_documentation'} if name=='software' else None
    pending=[test['test_id'] for test in tests if test['required_for_final_acceptance'] and (classes is None or test['execution_class'] in classes) and test['status']!='pass']
    for stream in record['workstreams']:
        for item in stream['deliverables']:
            if item.get('status')!='complete' or not all(item.get(k) for k in ('artifact_links','owner','reviewer')):pending.append(item['deliverable_id'])
    if pending:raise ValueError('Gate remains incomplete: '+', '.join(pending))

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('record',type=Path);parser.add_argument('--gate',choices=('software','self-service'))
    args=parser.parse_args()
    try:
        record=load(args.record);tests=validate(record,args.record.parent)
        if args.gate:gate(record,tests,args.gate)
    except (OSError,ValueError,KeyError,TypeError) as error:parser.exit(1,f'Acceptance rejected: {error}\n')
    print('Evidence structure valid.'+(' Requested gate passes.' if args.gate else ' No acceptance gate was requested.'))

if __name__=='__main__':main()
