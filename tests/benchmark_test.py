#!/usr/bin/env python3
"""Measurement boundaries and semantic gates; no speed thresholds."""
import csv
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from unittest.mock import patch
from types import SimpleNamespace

book,structure,replay,scripts=sys.argv[1:]
sys.path.insert(0,scripts)
from make_replay_workload import write
import measure
import render_results
import run as retained

def run(args):
    r=subprocess.run(list(map(str,args)),capture_output=True,text=True)
    assert r.returncode==0,r.stdout+r.stderr
    return r.stdout
with tempfile.TemporaryDirectory(prefix='msim-measurement-test-') as tmp:
    assert 'detected=3' in run([book,'allocation-control'])
    root=Path(tmp);stream=root/'commands'
    from render_book import build_payload
    visualizer=build_payload(Path(book).parent/'diff_replay','test-source')
    cases={case['key']:case for case in visualizer['cases']}
    assert len(cases['burst']['frames'])==1003 and cases['burst']['peak_orders']==160
    assert [x[0] for x in cases['basics']['frames'][6]['executions']]==['2','4']
    assert len({x[2] for x in cases['sweep']['frames'][20]['executions']})==5
    edge=cases['limits']['frames']
    assert edge[6]['status']=='capacity_full' and edge[6]['updates']==edge[6]['remove']==edge[6]['executions']==[]
    assert edge[7]['status']=='accepted' and edge[7]['filled']==70 and edge[7]['rested']==0
    assert edge[9]['status']=='duplicate_id' and edge[10]['status']=='not_found' and edge[11]['status']=='invalid_qty'
    assert next(o for o in edge[-1]['updates'] if o[0]=='1')[4]==1
    for case in visualizer['cases']:
        for frame in case['frames']:
            if frame['kind']=='add' and frame['status']=='accepted':assert frame['filled']+frame['rested']==frame['quantity']
    for result in (SimpleNamespace(returncode=1,stdout='{"verified":true}',stderr='planted failure'),SimpleNamespace(returncode=0,stdout='{"verified":false,"frames":[]}',stderr='')):
        with patch('render_book.subprocess.run',return_value=result):
            try:build_payload(Path(book).parent/'diff_replay','test-source')
            except ValueError:pass
            else:raise AssertionError('unverified matching data was published')
    boundary=root/'json-boundary.trace';boundary.write_text('# limits 513,2,1000000\n1 add 1 B 100 1\n')
    refused=subprocess.run([str(Path(book).parent/'diff_replay'),str(boundary),'--json'],capture_output=True,text=True)
    assert refused.returncode!=0 and '"verified":true' not in refused.stdout
    boundary.write_text('# limits 2,2,1000000\n1 add 9007199254740993 B 100 1\n')
    exact=json.loads(run([Path(book).parent/'diff_replay',boundary,'--json']))
    assert exact['frames'][0]['id']==exact['frames'][0]['bids'][0][1][0][0]=='9007199254740993'
    run([book,'generate','--workload','cancel_churn','--seed',17,'--commands',10000,'--out',stream])
    run([book,'verify','--stream',stream]);digests=set()
    for mode in ('throughput','latency','batch'):
        out=root/(mode+'.json')
        run([book,'run','--stream',stream,'--mode',mode,'--reps',1,'--warmup',0,'--minimum-ms',20,'--batch',16,'--json',out])
        data=json.loads(out.read_text());digests.add(data['outcomes']['execution_digest'])
        assert data['measured_cpp_new_calls']==0 and data['outcomes']['internal_allocations_during_replay']==0
        assert data['outcomes']['execution_output_growths']==0
        if mode=='throughput':assert data['reps'][0]['run_ms']>=20
        if mode=='batch':assert data['batch_size']==16 and data['batch_mean_ns']['n']==625
    assert len(digests)==1
    # A failed observer must retain its sample and reap its own child.
    class FakeRun:
        def __init__(self):self.output=root;self.meta={'commands':[]}
        def save(self):pass
    fake=FakeRun();original_sample,original_power=measure.process_sample,measure.power
    try:
        measure.power=lambda:dict(ac_power=True,pageouts=0)
        measure.process_sample=lambda _:dict(violations=[{'pid':123,'cpu':100}],processes=[])
        try:measure.Controlled(fake)
        except RuntimeError:pass
        else:raise AssertionError('competing process admitted')
        assert json.loads((root/'host-observations.json').read_text())[0]['violations']
        observer=measure.Controlled.__new__(measure.Controlled);observer.run=fake;observer.samples=[];observer.initial={'pageouts':0}
        def broken(_):raise RuntimeError('planted observer failure')
        measure.process_sample=broken
        try:observer.command('observer-failure',[sys.executable,'-c','import time;time.sleep(30)'])
        except RuntimeError:pass
        else:raise AssertionError('observer failure was ignored')
        assert fake.meta['commands'][-1]['passed'] is False and fake.meta['commands'][-1]['exit_code'] is not None
    finally:measure.process_sample,measure.power=original_sample,original_power
    # A same-source PASS receipt from the wrong activity, or an empty/partial
    # activity, must not manufacture an admitted publication summary.
    packet=root/'packet';packet.mkdir();published=root/'published';published.mkdir()
    meta=dict(status='PASS',source_id=retained.source_manifest()[0],source_unchanged=True,commands=[])
    for activity in ('engine','mechanisms'):
        for declared in ('unrelated',activity):
            meta['measurement_activity']=declared;(packet/'run.json').write_text(json.dumps(meta))
            try:getattr(render_results,activity)(published,packet)
            except ValueError:pass
            else:raise AssertionError('wrong/empty measurement activity admitted')
    (packet/'cancel_churn-batch16.json').write_text(json.dumps(data))
    meta['measurement_activity']='engine';(packet/'run.json').write_text(json.dumps(meta))
    try:render_results.engine(published,packet)
    except ValueError:pass
    else:raise AssertionError('partial engine matrix admitted')
    for kind,arms,patterns,size in (('ladder',('vector','tree'),('near','uniform'),32),('index',('window','hash'),('recent','aged','jump'),12)):
        for pattern in patterns:
            digests=set()
            for arm in arms:
                rows=list(csv.DictReader(run([structure,kind,arm,size,10000,pattern]).splitlines()))
                assert len(rows)==5;digests.update(row['digest'] for row in rows)
            assert len(digests)==1
    source=root/'feed.itch';write(source,1000)
    for identity in ('identity','unsafe'):
        rows=list(csv.DictReader(run([replay,source,2,31,'window',identity,'plain','warm',1]).splitlines()))
        assert len(rows)==1 and rows[0]['scope']==('VALIDATED_DIAGNOSTIC' if identity=='identity' else 'EXPERIMENTAL_NO_GLOBAL_IDENTITY')
print('batch boundary, allocation interception, isolated-structure and pipeline gates: PASS')
