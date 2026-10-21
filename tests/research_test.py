#!/usr/bin/env python3
"""Exact wire-state agreement plus hand ledgers for the connected experiment."""
import csv
import importlib
import json
import collections
import itertools
import shutil
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

binary,scripts,volume_binary=sys.argv[1:]
sys.path.insert(0,scripts)
from make_demo import rows, header, add, OPEN
from check_observations import check, Observations
from research import analyze, typed, Prefix, SECOND, Study
from render_tape import verify as verify_tape, write_view, export as export_tape
from types import SimpleNamespace
from cli_failure import rejected

def records(root,name):
    with (root/(name+'.csv')).open(newline='') as f:return list(csv.DictReader(f))
def write(path,items):path.write_bytes(b''.join(struct.pack('>H',len(r))+r for r in items))

# A dynamic Fenwick append must preserve old accumulated departures.
p=Prefix(); values=[]
for bad_label in ('', 'x'*65, 'date\nlabel'):
    try:export_tape(SimpleNamespace(date_label=bad_label))
    except ValueError:pass
    else:raise AssertionError('unrenderable date label admitted')
for n in range(1,100):
    assert p.append()==n;values.append(0)
    for k in range(n):
        p.add(0,k+1,k+2);values[k]+=k+2
    for k in range(n+1):assert p.prefix(0,k)==sum(values[:k])

# Independent unit-token service model, exhaustively compared with the scalar
# shadow calculation. This checks analytical mechanics, not Nasdaq allocation.
for initial in range(4):
    for sequence in itertools.product(('E1','E2','X1','X2','N1','N2','B1','B2'),repeat=3):
        events=[];broken=set();expected=[]
        for n,code in enumerate(sequence):
            kind='C' if code[0]=='N' else 'E' if code[0]=='B' else code[0]
            if code[0]=='B':broken.add(n)
            events.append(dict(t=(n+1)*100000000,n=n,k=kind,side='B',price=100,execution_price=100,qty=int(code[1]),match=n,flag='N' if code[0]=='N' else 'Y',ahead=True,mid=201,level_empty=0,at_best=True))
        for optimistic in (False,True):
            queue=['A']*initial+['SHADOW'];filled=False
            for event in events:
                eligible=event['k'] in ('E','C') and event['flag']=='Y' and event['match'] not in broken
                if eligible:
                    for _ in range(event['qty']):
                        if queue:
                            filled|=queue.pop(0)=='SHADOW'
                elif event['k'] in ('E','C') or optimistic:
                    for _ in range(event['qty']):
                        if 'A' in queue:queue.remove('A')
            expected.append(int(filled))
        study=Study.__new__(Study);study.broken=broken;study.counts=collections.Counter()
        study.secondary=collections.defaultdict(collections.Counter);study.probe_groups=collections.defaultdict(collections.Counter)
        captured=[];study.emit=lambda _,row:captured.append(row)
        probe=dict(locate=1,bin=0,entry_ordinal=0,entry_ns=0,side='B',price=100,initial_ahead=initial,entry_mid=201,spread='SUBCENT',stop_ns=2*SECOND,stop_reason='HORIZON',events=events,tree=None,last_rank=0)
        study.evaluate_probe(probe)
        assert [captured[0][label+'_opportunity'] for label in ('pessimistic','optimistic')]==expected

# Exact stop boundaries have reason-sensitive semantics. A natural horizon
# supplies its endpoint quote; an administrative/clock stop supplies no quote
# at that boundary. S:M equality still completes primary one-second exposure.
for reason,stop,complete,adverse in (
    ('HORIZON',2*SECOND,'COMPLETE','OBSERVED'),
    ('TRADING_STATE',SECOND,'TRADING_STATE','TRADING_STATE'),
    ('CIRCUIT_BREAKER',SECOND,'CIRCUIT_BREAKER','CIRCUIT_BREAKER'),
    ('SESSION_END',SECOND,'COMPLETE','SESSION_END'),
    ('TRADING_STATE',1_200_000_000,'COMPLETE','TRADING_STATE'),
    ('SESSION_END',1_200_000_000,'COMPLETE','SESSION_END'),
    ('CLOCK_REGRESSION',1_200_000_000,'COMPLETE','CLOCK_REGRESSION')):
    study=Study.__new__(Study);study.broken=set();study.counts=collections.Counter()
    study.secondary=collections.defaultdict(collections.Counter);study.probe_groups=collections.defaultdict(collections.Counter)
    captured=[];study.emit=lambda _,row:captured.append(row)
    event=dict(t=200_000_000,n=1,k='E',side='B',price=100,execution_price=100,qty=1,match=1,flag='Y',ahead=False,mid=201,level_empty=0,at_best=True)
    probe=dict(locate=1,bin=0,entry_ordinal=0,entry_ns=0,side='B',price=100,initial_ahead=0,entry_mid=201,spread='SUBCENT',stop_ns=stop,stop_reason=reason,events=[event],tree=None,last_rank=0)
    study.evaluate_probe(probe)
    for label in ('pessimistic','optimistic'):
        assert captured[0][label+'_opportunity']==1
        assert captured[0][label+'_followup']==complete
        assert captured[0][label+'_adverse_reason']==adverse
        assert captured[0][label+'_adverse_half_units']==(0 if adverse=='OBSERVED' else 'NA')

with tempfile.TemporaryDirectory(prefix='msim-research-test-') as tmp:
    root=Path(tmp)
    for variant in ('base','broken-service','clock','halt','late','breaker','auction-clock','ahead-after-terminal','clock-bins','global-clock','ineligible-depletion','ipo-update','worse-execution-price','improved-execution-price','halt-adverse-boundary','same-price-replacement-level','best-displaced-nonempty'):
        source=root/(variant+'.itch');items=rows()
        if variant=='broken-service':items.insert(-5,header('B',t=OPEN+2_200_000_000)+struct.pack('>Q',101))
        if variant=='same-price-replacement-level':
            items.insert(9,header('U',t=OPEN+25_000_000)+struct.pack('>QQII',1,5,100,100000))
            for j,item in enumerate(items):
                if chr(item[0]) in ('E','X') and int.from_bytes(item[11:19],'big')==1:items[j]=item[:11]+struct.pack('>Q',5)+item[19:]
        if variant in ('worse-execution-price','improved-execution-price'):
            # Field-valid counterexamples to confusing resting and execution
            # price; this does not assert that worse-price C is venue-valid.
            price=100001 if variant=='worse-execution-price' else 99999
            items[12]=header('C',t=OPEN+300_000_000)+struct.pack('>QIQcI',3,20,101,b'Y',price)
        if variant=='clock':
            item=bytearray(items[11]);item[5:11]=(OPEN+80_000_000).to_bytes(6,'big');items[11]=bytes(item)
        if variant=='halt':items.insert(11,header('H',t=OPEN+150_000_000)+b'ONE     H'+bytes(5))
        if variant=='best-displaced-nonempty':items.insert(11,add(6,'B',10,100050,OPEN+150_000_000))
        if variant=='halt-adverse-boundary':
            items.insert(6,add(5,'B',10,99900,OPEN-1))
            items.insert(16,header('H',t=OPEN+1_300_000_000)+b'ONE     H'+bytes(5))
        if variant=='late':
            items=items[:9]+[items[9][:5]+(OPEN+500_000_000).to_bytes(6,'big')+items[9][11:],
                 header('A',t=OPEN+500_000_000)+struct.pack('>QcI8sI',5,b'B',20,b'ONE     ',100000),
                 header('E',t=OPEN+600_000_000)+struct.pack('>QIQ',3,20,101),
                 header('S',0,OPEN+SECOND)+b'M',header('S',0,OPEN+SECOND+1)+b'E',header('S',0,OPEN+SECOND+2)+b'C']
        if variant=='breaker':items.insert(15,header('W',0,OPEN+1_200_000_000)+b'1')
        if variant=='auction-clock':
            item=bytearray(items[6]);item[5:11]=(OPEN+SECOND).to_bytes(6,'big');items[6]=bytes(item)
        if variant=='clock-bins':
            items[7]=header('S',0,OPEN+36157)+b'Q'
            items[-5:-5]=[header('H',t=OPEN+299*SECOND)+b'ONE     T'+bytes(5),header('P',t=OPEN+300*SECOND+10)+struct.pack('>QcI8sIQ',0,b'B',1,b'ONE     ',100000,999)]
        if variant=='global-clock':items.insert(15,header('W',0,OPEN+60_000_000)+b'1')
        if variant=='ineligible-depletion':
            item=bytearray(items[5]);item[32:36]=struct.pack('>I',100000);items[5]=bytes(item)
        if variant=='ipo-update':items.insert(3,header('K',0,1)+b'ONE     '+bytes(9))
        if variant=='ahead-after-terminal':
            items[10]=header('X',t=OPEN+100_000_000)+struct.pack('>QI',3,20)
            items[11]=header('X',t=OPEN+200_000_000)+struct.pack('>QI',1,90)
            items[12]=header('E',t=OPEN+300_000_000)+struct.pack('>QIQ',1,10,101)
        write(source,items);report=root/(variant+'-observations')
        run=subprocess.run([binary,str(source),str(report)],capture_output=True,text=True)
        assert run.returncode==0,run.stdout+run.stderr
        check(source,report,root/(variant+'-check.json'))
        raw=Observations(source)
        independent=analyze(raw,raw.panel,root/(variant+'-raw-study'))
        with (report/'events.csv').open(newline='') as stream:
            candidate=analyze(map(typed,csv.DictReader(stream)),raw.panel,root/(variant+'-cpp-study'))
        assert independent==candidate
        life=records(root/(variant+'-cpp-study'),'lifetimes')
        probe=records(root/(variant+'-cpp-study'),'opportunities')
        if variant=='base':
            assert len(life)==1 and life[0]['ref']=='3' and life[0]['endpoint']=='E'
            assert life[0]['initial_ahead']=='100' and life[0]['ahead_execution']=='10' and life[0]['ahead_cancellation']=='90'
            assert candidate['counts']['optimistic_opportunities']==1 and candidate['counts']['pessimistic_opportunities']==0
            depletion=records(root/(variant+'-cpp-study'),'depletions')
            assert len(depletion)==1 and depletion[0]['endpoint']=='RETURN' and depletion[0]['elapsed_ns']=='100000000'
            assert depletion[0]['level_birth_ns']==str(OPEN-2) and depletion[0]['level_age_ns']=='300000002'
            assert probe[0]['time_to_level_depletion_ns']=='300000000' and probe[0]['depletion_followup']=='DEPLETED'
            assert probe[1]['depletion_followup']=='COMPLETE_NO_DEPLETION'
            assert probe[0]['time_to_lost_best_ns']=='300000000'
            context=records(root/(variant+'-cpp-study'),'context_volume')
            assert sum(int(r['P']) for r in context)==0 and sum(int(r['corrected_P']) for r in context)==7
            assert sum(int(r['E']) for r in context)==31
            auction=records(root/(variant+'-cpp-study'),'auction_pairs')
            assert len(auction)==2 and auction[0]['near_difference']=='5' and auction[0]['staleness_ns']=='1'
            assert candidate['exclusions']['PROBE_NO_EVENT_FIRST_SECOND']==154
            assert probe[0]['optimistic_adverse_reason']=='QUOTE_UNAVAILABLE'
            from render_replay import build_payload, encode
            replay=build_payload(source,report,root/(variant+'-cpp-study'),'test-source')
            frames=replay['frames']
            assert len(frames)==24 and replay['initial_frame']==9
            assert [(o['ref'],o['shares']) for o in frames[10]['orders'] if o['side']=='B']==[('1',100),('3',20)]
            assert [(o['ref'],o['shares']) for o in frames[11]['orders'] if o['side']=='B']==[('1',10),('3',20)]
            assert frames[11]['shadow']['optimistic']['ahead']==10 and frames[11]['shadow']['pessimistic']['ahead']==100
            assert frames[12]['shadow']['optimistic']['ahead']==0 and not frames[12]['shadow']['optimistic']['opportunity']
            assert not any(o['side']=='B' for o in frames[13]['orders'])
            assert frames[13]['shadow']['optimistic']['opportunity'] and frames[13]['shadow']['pessimistic']['ahead']==70
            assert not frames[13]['shadow']['pessimistic']['opportunity']
            assert frames[14]['orders']==frames[15]['orders']
            assert frames[16]['orders']==frames[18]['orders']
            assert frames[15]['eligible_shares']==37 and frames[18]['eligible_shares']==31
            assert not next(p for p in frames[15]['tape'] if p['match']=='102')['broken']
            assert next(p for p in frames[18]['tape'] if p['match']=='102')['broken']
            assert frames[16]['shadow']['complete'] and frames[-1]['cross_shares']==160
            assert encode(replay)==encode(build_payload(source,report,root/(variant+'-cpp-study'),'test-source'))
            wrong=root/'wrong-demo.itch';wrong.write_bytes(source.read_bytes()[:-1])
            try:build_payload(wrong,report,root/(variant+'-cpp-study'),'test-source')
            except ValueError:pass
            else:raise AssertionError('visualizer admitted a non-original input')
            table=root/(variant+'-cpp-study')/'opportunities.csv';saved=table.read_bytes();table.write_bytes(saved+b'\n')
            try:build_payload(source,report,root/(variant+'-cpp-study'),'test-source')
            except ValueError:pass
            else:raise AssertionError('visualizer admitted a changed research table')
            finally:table.write_bytes(saved)
        elif variant=='broken-service':
            assert candidate['counts']['optimistic_opportunities']==0
            assert any(r['endpoint']=='E' for r in life) # correction never undoes displayed path
        elif variant in ('worse-execution-price','improved-execution-price'):
            assert candidate['counts']['optimistic_opportunities']==int(variant=='improved-execution-price')
            assert candidate['counts']['C_limit_incompatible_or_unavailable' if variant=='worse-execution-price' else 'C_price_improved']==1
        elif variant=='clock':
            assert life[0]['endpoint']=='CLOCK_REGRESSION' and life[0]['elapsed_ns']=='NA'
            assert records(root/(variant+'-cpp-study'),'depletions')[0]['level_age_ns']=='NA'
            assert probe[0]['depletion_followup']=='CLOCK_REGRESSION' and probe[0]['depletion_observation_ns']=='NA'
        elif variant=='late':
            assert len(life)==2 and all(r['complete_followup']=='0' and r['followup']=='LATE_ENTRY' for r in life)
            sensitivity=records(root/(variant+'-cpp-study'),'sensitivity')
            assert all(r['execution_endpoint_fraction']=='NA' for r in sensitivity)
        elif variant=='breaker':
            assert life[0]['endpoint']=='E' and life[0]['followup']=='HORIZON'
        elif variant=='auction-clock':
            auction=records(root/(variant+'-cpp-study'),'auction_pairs')
            assert auction[0]['staleness_ns']=='NA' and auction[0]['last_ordinal']=='NA'
        elif variant=='clock-bins':
            assert any(r['bin']=='1' and r['entry_ns']==str(OPEN+300*SECOND+10) for r in probe)
        elif variant=='global-clock':
            assert life[0]['followup']=='CLOCK_REGRESSION' and life[0]['elapsed_ns']=='NA'
            assert candidate['exclusions']['GLOBAL_STOP_CLOCK_INVALID']==1
        elif variant=='ineligible-depletion':assert candidate['exclusions']['DEPLETION_LOCKED_CROSSED']==1
        elif variant=='ipo-update':assert candidate['counts']['optimistic_opportunities']==1
        elif variant=='ahead-after-terminal':
            assert life[0]['endpoint']=='X' and life[0]['ahead_execution']=='10' and life[0]['ahead_cancellation']=='90'
        elif variant=='halt':
            assert life[0]['endpoint']=='TRADING_STATE'
            assert candidate['counts']['optimistic_opportunities']==0 and probe[0]['optimistic_followup']=='TRADING_STATE'
        elif variant=='halt-adverse-boundary':
            assert probe[0]['optimistic_opportunity']=='1'
            assert probe[0]['optimistic_adverse_reason']=='TRADING_STATE' and probe[0]['optimistic_adverse_half_units']=='NA'
        elif variant=='same-price-replacement-level':
            level=records(root/(variant+'-cpp-study'),'depletions')[0]
            assert level['level_age_ns']=='300000002' and level['cancellation_reduction']=='190' and level['execution_reduction']=='30'
        elif variant=='best-displaced-nonempty':
            assert probe[0]['time_to_lost_best_ns']=='150000000' and probe[0]['time_to_level_depletion_ns']=='300000000'
            assert records(root/(variant+'-cpp-study'),'depletions')==[]
    # Exercise analytical CLI gates and publication-table arithmetic on the
    # same hand ledger. Renderer formulas are checked against known counts.
    import run as retained
    import render_results
    import check_volume
    # Historical presentation must retain prefix state, exact boundary/order
    # semantics and report classes; it must not turn book reductions into fills.
    for variant in ('base','clock','same-price-replacement-level','improved-execution-price'):
        source=root/(variant+'.itch');report=root/(variant+'-tape')
        result=subprocess.run([binary,'--tape',str(source),str(report),'ONE',str(OPEN),str(OPEN+1_500_000_000)],capture_output=True,text=True)
        assert result.returncode==0 and result.stdout.endswith('result: SUCCESS\n'),result.stdout+result.stderr
        payload=verify_tape(source,report)
        assert len(payload['initial'])==2 and payload['initial'][0][0]=='1'
        assert payload['counts']['Q']==1 and payload['counts']['P']==1
        if variant=='clock':assert payload['clock_regressions']==1
        if variant=='improved-execution-price':assert next(e for e in payload['events'] if e['k']=='C')['execution_price']==99999
        if variant=='base':
            assert next(e for e in payload['events'] if e['k']=='X')['updates']==[['1','B',100000,10]]
            assert next(e for e in payload['events'] if e['k']=='E')['removes']==['1']
            view=root/'tape-view';view.mkdir();payload.update(date_label='fixture <script>',source_id='fixture',executable_sha256='fixture',input_bytes=source.stat().st_size)
            write_view(view,payload)
            html=(view/'index.html').read_text();assert 'fixture <script>' not in html and 'fixture \\u003cscript>' in html
            original=(report/'events.csv').read_text();lines=original.splitlines();bad=lines[1].rsplit(',',1)[0]+',0';lines[1]=bad
            (report/'events.csv').write_text('\n'.join(lines)+'\n')
            try:verify_tape(source,report)
            except ValueError:pass
            else:raise AssertionError('planted state disagreement admitted')
            (report/'events.csv').write_text(original)
    # A replacement at the inclusive start is part of the clip, and its ID is
    # never rounded through a browser number. End is exclusive.
    source=root/'wide-id.itch';items=rows();wide=9007199254740993
    items.insert(9,header('U',t=OPEN)+struct.pack('>QQII',1,wide,100,100000))
    for i,item in enumerate(items):
        if chr(item[0]) in ('X','E') and int.from_bytes(item[11:19],'big')==1:items[i]=item[:11]+struct.pack('>Q',wide)+item[19:]
    write(source,items);report=root/'wide-tape'
    result=subprocess.run([binary,'--tape',str(source),str(report),'ONE',str(OPEN),str(OPEN+1_400_000_000)],capture_output=True,text=True)
    assert result.returncode==0,result.stderr
    payload=verify_tape(source,report)
    assert next(e for e in payload['events'] if e['k']=='U')['updates']==[[str(wide),'B',100000,100]]
    assert not any(e['raw_ns']==OPEN+1_400_000_000 for e in payload['events'])
    for name,items,stock,first,last in (
        ('truncated-tape',rows()[:-1],'ONE',OPEN,OPEN+SECOND),
        ('unknown-symbol',rows(),'MISSING',OPEN,OPEN+SECOND),
        ('unbracketed',rows(),'ONE',OPEN+3*SECOND,OPEN+4*SECOND),
        ('bad-interval',rows(),'ONE',OPEN,OPEN+61*SECOND)):
        source=root/(name+'.itch');write(source,items);destination=root/(name+'-out')
        result=subprocess.run([binary,'--tape',str(source),str(destination),stock,str(first),str(last)],capture_output=True,text=True)
        rejected(result);assert not destination.exists()
    public_input=root/'base.itch';accounting=root/'accounting'
    result=subprocess.run([volume_binary,str(public_input),str(accounting)],capture_output=True,text=True)
    assert result.returncode==0,result.stdout+result.stderr
    check_volume.check(public_input,accounting,root/'accounting-check.json',1)
    cli=[sys.executable,str(Path(scripts)/'research.py'),'--raw',str(public_input),'--observations-check',str(root/'base-observations'),'--out',str(root/'cli-study'),'--accounting',str(accounting),'--accounting-check',str(root/'accounting-check.json')]
    result=subprocess.run(cli,capture_output=True,text=True);assert result.returncode==0,result.stdout+result.stderr
    held=json.loads((root/'cli-study/summary.json').read_text())
    assert held==json.loads((root/'base-cpp-study/summary.json').read_text())
    packet=root/'packet';packet.mkdir()
    for source,destination in ((accounting,'report'),(root/'base-observations','observations'),(root/'base-cpp-study','research'),(root/'base-raw-study','raw-research')):shutil.copytree(source,packet/destination)
    shutil.copy2(root/'accounting-check.json',packet/'independent.json')
    (packet/'run.json').write_text(json.dumps(dict(status='PASS',source_id=retained.source_manifest()[0],source_unchanged=True,input_unchanged=True)))
    public=root/'summary';public.mkdir();render_results.study(public,packet)
    published=json.loads((public/'study.json').read_text())
    assert published['accounting']['eligible_shares']==31 and published['accounting']['p_fraction']==0
    opportunities=records(public,'opportunities')
    assert sum(int(row['optimistic_opportunities']) for row in opportunities)==1
    assert sum(int(row['pessimistic_opportunities']) for row in opportunities)==0
    assert len(records(public,'auctions'))==2
    level=records(public,'depletions')[0]
    assert level['reported_execution_reduction_shares']=='30' and level['reported_cancellation_replacement_reduction_shares']=='90'
    timing=records(public,'depletion-times')
    assert sum(int(r['entries']) for r in timing if r['outcome']=='GT_100MS_LE_1S')==1
    assert sum(int(r['entries']) for r in timing if r['outcome']=='NO_DEPLETION_1S')==1
    depth=records(public,'opportunity-depth')
    buy=next(r for r in depth if r['side']=='B' and r['initial_ahead_bin']=='1_100')
    assert buy['complete_entries']=='1' and buy['optimistic_opportunities']=='1' and buy['pessimistic_opportunities']=='0'
print('independent raw state, hand-derived lifetimes, corrections, auctions, scenario sensitivity: PASS')
