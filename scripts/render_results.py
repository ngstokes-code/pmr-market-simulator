#!/usr/bin/env python3
"""Compact publication tables from identified retained runs; raw logs stay private.

Every rate is derived from admitted complete activities. This renderer performs
no selection by speed, symbol outcome, price error or visual appearance.
"""
import argparse
import collections
import csv
import json
from pathlib import Path
import re
import statistics
import run as retained


def read_rows(path):
    with Path(path).open(newline='') as f:yield from csv.DictReader(f)
def write_csv(root,name,rows,fields=None):
    rows=list(rows)
    if not rows and fields is None:return
    with (root/name).open('w',newline='') as f:
        writer=csv.DictWriter(f,fieldnames=fields or list(rows[0]));writer.writeheader();writer.writerows(rows)
def write_json(root,name,data):(root/name).write_text(json.dumps(data,indent=2,sort_keys=True)+'\n')
def fraction(a,b):return a/b if b else 'NA'

def admitted(path,allow_failure=False):
    path=Path(path);run=json.loads((path/'run.json').read_text())
    if run['source_id']!=retained.source_manifest()[0] or not run['source_unchanged']:
        raise ValueError('stale-source evidence: '+str(path))
    if not allow_failure and (run['status']!='PASS' or run.get('input_unchanged') is False):
        raise ValueError('activity not admitted: '+str(path))
    return path,run


def study(root,path):
    path,run=admitted(path);report=path/'report';research=path/'research'
    volume=json.loads((report/'summary.json').read_text());check=json.loads((path/'independent.json').read_text())
    primary=json.loads((research/'summary.json').read_text());raw=json.loads((path/'raw-research/summary.json').read_text())
    if check['status']!='PASS' or primary!=raw or volume['input_sha256']!=check['input_sha256']:
        raise ValueError('study agreement gate failed')
    denominator=volume['E']+volume['CY']+volume['P']
    panel=list(read_rows(path/'observations/panel.csv'))
    write_csv(root,'panel.csv',panel)
    birth=collections.defaultdict(collections.Counter)
    for row in read_rows(research/'lifetimes.csv'):
        g=birth[(row['spread'],row['ahead_bin'])];g['births']+=1
        if row['complete_followup']=='1':
            g['complete_horizon_births']+=1;g['endpoint_'+row['endpoint']]+=1
            for field in ('ahead_execution','ahead_cancellation','own_execution','own_cancellation','initial_ahead'):g[field]+=int(row[field])
        else:g['censored_births']+=1
    queue=[]
    for (regime,ahead),counts in sorted(birth.items()):
        row=dict(spread=regime,initial_ahead_bin=ahead)
        row.update({k:counts[k] for k in ('births','complete_horizon_births','censored_births','endpoint_E','endpoint_C','endpoint_X','endpoint_D','endpoint_U','endpoint_HORIZON')})
        den=counts['complete_horizon_births'];row['execution_endpoint_fraction']=fraction(counts['endpoint_E']+counts['endpoint_C'],den)
        for field in ('ahead_execution','ahead_cancellation','own_execution','own_cancellation','initial_ahead'):row['mean_'+field]=fraction(counts[field],den)
        queue.append(row)
    write_csv(root,'queues.csv',queue)
    depletion=collections.defaultdict(collections.Counter);return_times=collections.defaultdict(list);level_ages=collections.defaultdict(list)
    for row in read_rows(research/'depletions.csv'):
        key=row['spread'],row['cause'];g=depletion[key];g['units']+=1
        g['reported_execution_reduction_shares']+=int(row['execution_reduction'])
        g['reported_cancellation_replacement_reduction_shares']+=int(row['cancellation_reduction'])
        reason=row['endpoint'];g['returned' if reason=='RETURN' else 'no_return' if reason=='NO_RETURN' else 'censored']+=1
        if reason=='RETURN':return_times[key].append(int(row['elapsed_ns']));g['restored_best']+=int(row['restored_best'])
        if row['level_age_ns']=='NA':g['level_age_unavailable']+=1
        else:level_ages[key].append(int(row['level_age_ns']))
    dep=[]
    for (regime,cause),counts in sorted(depletion.items()):
        row=dict(spread=regime,cause=cause);row.update({k:counts[k] for k in ('units','returned','no_return','censored','restored_best','reported_execution_reduction_shares','reported_cancellation_replacement_reduction_shares')})
        row.update(observed_return_fraction_lower=fraction(counts['returned'],counts['units']),observed_return_fraction_upper=fraction(counts['returned']+counts['censored'],counts['units']),median_ns_among_returns=statistics.median(return_times[(regime,cause)]) if return_times[(regime,cause)] else 'NA')
        ages=level_ages[(regime,cause)];row.update(level_age_available=len(ages),level_age_unavailable=counts['level_age_unavailable'],median_level_age_ns_among_observed_empties=statistics.median(ages) if ages else 'NA')
        dep.append(row)
    write_csv(root,'depletions.csv',dep)
    opportunities=collections.defaultdict(collections.Counter);adverse=collections.defaultdict(list)
    opportunity_strata=collections.defaultdict(collections.Counter)
    depletion_times=collections.defaultdict(collections.Counter);probe_depletion_times=collections.defaultdict(list)
    for row in read_rows(research/'opportunities.csv'):
        key=row['spread'],row['side'];g=opportunities[key];g['entries']+=1
        g['lost_best_entries']+=int(row['lost_best_within_horizon'])
        if row['time_to_level_depletion_ns']!='NA':
            duration=int(row['time_to_level_depletion_ns']);probe_depletion_times[key].append(duration);g['level_depleted_entries']+=1
            bucket='ZERO' if duration==0 else 'GT_0_LE_1MS' if duration<=1000000 else 'GT_1MS_LE_10MS' if duration<=10000000 else 'GT_10MS_LE_100MS' if duration<=100000000 else 'GT_100MS_LE_1S'
        elif row['depletion_followup']=='COMPLETE_NO_DEPLETION':g['level_no_depletion_entries']+=1;bucket='NO_DEPLETION_1S'
        else:g['level_censored_entries']+=1;bucket='CENSORED_'+row['depletion_followup']
        depletion_times[key][bucket]+=1
        complete=row['optimistic_followup']=='COMPLETE';g['complete_entries' if complete else 'censored_entries']+=1
        ahead=int(row['initial_ahead']);ahead_bin='0' if ahead==0 else '1_100' if ahead<=100 else '101_1000' if ahead<=1000 else 'GT1000'
        stratum=opportunity_strata[(key,ahead_bin)];stratum['entries']+=1;stratum['complete_entries' if complete else 'censored_entries']+=1
        if complete:
            for label in ('pessimistic','optimistic'):
                g[label+'_opportunities']+=int(row[label+'_opportunity']);stratum[label+'_opportunities']+=int(row[label+'_opportunity'])
            g['assumption_sensitive']+=int(row['pessimistic_opportunity']!=row['optimistic_opportunity'])
        for label in ('pessimistic','optimistic'):
            if row[label+'_adverse_half_units']!='NA':adverse[(key,label)].append(int(row[label+'_adverse_half_units']))
            g[label+'_adverse_'+row[label+'_adverse_reason']]+=1
    opp=[]
    for (regime,side),counts in sorted(opportunities.items()):
        row=dict(spread=regime,side=side);row.update({k:counts[k] for k in ('entries','complete_entries','censored_entries','level_depleted_entries','level_no_depletion_entries','level_censored_entries','lost_best_entries','pessimistic_opportunities','optimistic_opportunities','assumption_sensitive')})
        durations=probe_depletion_times[(regime,side)];row['median_time_to_level_depletion_ns_among_observed']=statistics.median(durations) if durations else 'NA'
        for label in ('pessimistic','optimistic'):
            row[label+'_opportunity_fraction']=fraction(counts[label+'_opportunities'],counts['complete_entries'])
            values=adverse[((regime,side),label)];row[label+'_adverse_available']=len(values)
            row[label+'_mean_adverse_half_price_units']=statistics.mean(values) if values else 'NA'
            row[label+'_median_adverse_half_price_units']=statistics.median(values) if values else 'NA'
        opp.append(row)
    write_csv(root,'opportunities.csv',opp)
    strata=[]
    for key in sorted(opportunities):
        for ahead_bin in ('0','1_100','101_1000','GT1000'):
            counts=opportunity_strata[(key,ahead_bin)];row=dict(spread=key[0],side=key[1],initial_ahead_bin=ahead_bin)
            row.update({k:counts[k] for k in ('entries','complete_entries','censored_entries','pessimistic_opportunities','optimistic_opportunities')})
            for label in ('pessimistic','optimistic'):row[label+'_opportunity_fraction']=fraction(counts[label+'_opportunities'],counts['complete_entries'])
            strata.append(row)
    write_csv(root,'opportunity-depth.csv',strata)
    buckets=('ZERO','GT_0_LE_1MS','GT_1MS_LE_10MS','GT_10MS_LE_100MS','GT_100MS_LE_1S','NO_DEPLETION_1S','CENSORED_TRADING_STATE','CENSORED_CIRCUIT_BREAKER','CENSORED_CLOCK_REGRESSION','CENSORED_SESSION_END','CENSORED_OBSERVATION_END')
    if any(set(counts)-set(buckets) for counts in depletion_times.values()):raise ValueError('unknown probe depletion censor category')
    write_csv(root,'depletion-times.csv',[dict(spread=regime,side=side,outcome=bucket,entries=counts[bucket]) for (regime,side),counts in sorted(depletion_times.items()) for bucket in buckets])
    contexts=collections.defaultdict(collections.Counter)
    for row in read_rows(research/'context_volume.csv'):
        key=tuple(row[k] for k in ('spread','depth_bin','active','session','trading','operational'))
        for field in ('E','CY','P','Q','CN','records','broken_records','corrected_E','corrected_CY','corrected_P','corrected_Q','corrected_CN'):
            contexts[key][field]+=int(row[field])
    context=[]
    for key,counts in sorted(contexts.items()):
        row=dict(zip(('spread','depth_bin','active','session','trading','operational'),key));row.update(dict(counts))
        den=counts['E']+counts['CY']+counts['P'];row.update(eligible_shares=den,p_fraction=fraction(counts['P'],den));context.append(row)
    write_csv(root,'context.csv',context)
    # Keep absent and repeated opening/closing episodes visible for all32 symbols.
    auctions=collections.defaultdict(list)
    for row in read_rows(research/'auction_pairs.csv'):auctions[(row['locate'],row['cross'])].append(row)
    auction=[]
    for member in panel:
        for kind in ('O','C'):
            pairs=auctions[(member['locate'],kind)];first=pairs[0] if pairs else None
            row=dict(locate=member['locate'],stock=member['stock'],cross=kind,reported_crosses=len(pairs),status='MISSING_CROSS' if not pairs else 'REPEATED_CROSS' if len(pairs)>1 else 'MISSING_INDICATION' if first['last_ordinal']=='NA' else 'PAIRED')
            for field in ('shares','price','indications','staleness_ns','paired','imbalance','direction','far_difference','near_difference','reference_difference'):row[field]=first[field] if first else 'NA'
            auction.append(row)
    write_csv(root,'auctions.csv',auction)
    # Separate whole-symbol and whole-time-block sensitivity, retaining zeros.
    lifetime=list(read_rows(research/'sensitivity.csv'));other=list(read_rows(research/'study_sensitivity.csv'))
    for label,key in (('symbols','locate'),('blocks','block_ns')):
        groups=collections.defaultdict(collections.Counter)
        for rows in (lifetime,other):
            for row in rows:
                g=groups[row[key]]
                for field,value in row.items():
                    if field not in ('locate','stock','block_ns','execution_endpoint_fraction'):g[field]+=int(value)
        output=[]
        for value,counts in sorted(groups.items(),key=lambda x:int(x[0])):
            row={key:value}
            if key=='locate':row['stock']=next(m['stock'] for m in panel if m['locate']==value)
            row.update(dict(counts));row['execution_endpoint_fraction']=fraction(counts['complete_execution_endpoints'],counts['complete_followup']);output.append(row)
        write_csv(root,label+'-sensitivity.csv',output)
    summary=dict(source_id=run['source_id'],run_record_sha256=retained.sha(path/'run.json'),input=dict(bytes=volume['input_bytes'],sha256=volume['input_sha256'],protocol=volume['protocol'],terminal_event=volume['terminal_event'],messages=volume['messages'],capture_continuity='UNVERIFIED'),
        accounting=dict(totals={k:volume[k] for k in ('E','CY','P','Q','CN')},eligible_shares=denominator,p_fraction=fraction(volume['P'],denominator),breaks=volume['breaks'],corrected_shares=volume['corrected_shares'],corrected_nonprintable_shares=volume['corrected_nonprintable_shares'],companion_groups=volume['companion_groups'],independent_check=check),
        research=primary,independence='independent wire/state and all-market accounting; shared high-level research formulas, separately checked by hand ledgers and bounded token-model enumeration',
        raw_file_sha256={str(p.relative_to(path)):retained.sha(p) for folder in ('report','observations') for p in sorted((path/folder).glob('*')) if p.is_file()})
    # Independent metadata has no private file paths; large raw tables remain private.
    write_json(root,'study.json',summary)


def engine(root,path):
    path,run=admitted(path);results=[]
    if run.get('measurement_activity')!='engine':raise ValueError('not a controlled engine activity')
    workloads=('stable_depth','cancel_churn','many_prices','multi_level_sweep','bursty')
    expected={name+'-'+mode+(str(batch) if mode=='batch' else ''):(name,mode,batch)
              for name in workloads for mode,batch in (('throughput',16),('latency',16),('batch',16),('batch',64))}
    commands={c['label']:c for c in run['commands']}
    inventory={p.stem for p in path.glob('*.json') if p.name not in ('run.json','source-manifest.json','host-observations.json') and json.loads(p.read_text()).get('tool')=='book_bench'}
    if inventory!=set(expected):raise ValueError('engine result inventory differs from planned20 records')
    digests={}
    for filename in sorted(path.glob('*.json')):
        if filename.name in ('run.json','source-manifest.json','host-observations.json'):continue
        data=json.loads(filename.read_text())
        if data.get('tool')!='book_bench':continue
        name,mode,batch=expected[filename.stem];command=commands.get(filename.stem,{})
        if (not command.get('passed') or command.get('contaminated') is not False or command.get('exit_code')!=0
            or command.get('result_file')!=filename.name or command.get('result_sha256')!=retained.sha(filename)
            or Path(command['argv'][-1]).resolve()!=filename.resolve()
            or command.get('executable_sha256')!=run['build_executables']['book_bench']):raise ValueError('engine result/command custody disagreement')
        if any(not commands.get(name+'-'+gate,{}).get('passed') for gate in ('generate','verify')):raise ValueError('missing independent engine gate')
        if (data['mode']!=mode or data['stream']['workload']!=name or data['stream']['seed']!=17 or data['stream']['commands']!=1000000
            or data['book']!='engine' or data['warmup']!=1 or len(data['reps'])!=5
            or data['build']['type']!='Release' or data['build']['sanitize']):raise ValueError('engine record differs from planned workload/profile')
        if data['measured_cpp_new_calls'] or data['outcomes']['internal_allocations_during_replay'] or data['outcomes']['execution_output_growths']:raise ValueError('allocated engine path')
        if mode=='throughput' and any(r['run_ms']<1000 for r in data['reps']):raise ValueError('short throughput group')
        if mode=='batch' and data['batch_size']!=batch:raise ValueError('wrong batch boundary')
        digest=data['stream']['digest']
        if name in digests and digests[name]!=digest:raise ValueError('workload changed between modes')
        digests[name]=digest
        result={key:data[key] for key in ('stream','build','clock','scheduling','book','memory','mode','warmup','reps','summary','outcomes','measured_cpp_new_calls','max_rss_bytes')}
        result['stream']={k:v for k,v in result['stream'].items() if k!='path'}
        if 'latency_ns' in data:result['instrumented_per_command_ns']=data['latency_ns']['per_command']
        if 'batch_mean_ns' in data:result.update(batch_size=data['batch_size'],batch_mean_ns=data['batch_mean_ns'])
        if 'memory_bytes' in data:result['pmr_bytes']={k:v for k,v in data['memory_bytes'].items() if k!='samples'}
        result.update(raw_sha256=retained.sha(filename));results.append(result)
    write_json(root,'performance.json',dict(source_id=run['source_id'],run_record_sha256=retained.sha(path/'run.json'),observer_sha256=retained.sha(path/'host-observations.json'),hardware=run['conditions_before']['hardware'],compiler=run.get('compiler_version'),policy=run['measurement_policy'],records=results,interpretation='throughput is summed timed matching kernels; instrumented samples and batch means are separate quantities; no feed-throughput claim'))


def mechanisms(root,path):
    path,run=admitted(path);records=[]
    if run.get('measurement_activity')!='mechanisms':raise ValueError('not a controlled mechanism activity')
    expected={f'ladder-{n}-{locality}-{arm}':5 for n in (32,512,4096) for locality in ('near','uniform') for arm in ('vector','tree')}
    expected.update({'index-'+pattern+'-'+arm:5 for pattern in ('recent','aged','jump') for arm in ('window','hash')})
    expected.update({f'pipeline-{skew}-t{n}':3 for skew in ('balanced','hot') for n in (1,2,4,8)})
    expected.update({'pipeline-balanced-'+arm:3 for arm in ('chunk1000','chunk1000000','hash','identity-off','stages','cache-pressure')})
    if 'real_prefix' in run.get('inputs',{}):expected.update({f'pipeline-real-prefix-t{n}':3 for n in (1,2,4)})
    if len(run['commands'])!=len(expected) or {c['label'] for c in run['commands']}!=set(expected):raise ValueError('mechanism inventory differs from plan')
    for command in run['commands']:
        stdout=path/command['stdout'];text=stdout.read_text()
        stderr=path/command['stderr']
        if (not command.get('passed') or command.get('contaminated') is not False or command.get('exit_code')!=0
            or command.get('stdout_sha256')!=retained.sha(stdout) or command.get('stderr_sha256')!=retained.sha(stderr)
            or not text.startswith(('kind,','scope,'))):raise ValueError('mechanism command/output disagreement')
        rows=list(csv.DictReader(text.splitlines()));key='repetition' if text.startswith('kind,') else 'rep'
        if [int(r[key]) for r in rows]!=list(range(expected[command['label']])):raise ValueError('mechanism repetition inventory differs from plan')
        diagnostics=[dict(re.findall(r'([a-z_]+)=([0-9.]+)',line)) for line in stderr.read_text().splitlines()]
        records.append(dict(label=command['label'],rows=rows,diagnostics=diagnostics,stdout_sha256=retained.sha(stdout),stderr_sha256=retained.sha(stderr),process_max_rss_bytes=command['max_rss_bytes']))
    write_json(root,'mechanisms.json',dict(source_id=run['source_id'],run_record_sha256=retained.sha(path/'run.json'),observer_sha256=retained.sha(path/'host-observations.json'),records=records,inputs=run.get('inputs'),interpretation='isolated structure kernels and bounded matched pipeline diagnostics; identity bypass removes both live and day-wide global checks; cache pressure does not certify cold caches; no public feed-throughput rate'))


def verification(root,profiles):
    result=[]
    for item in profiles:
        label,path=item.split('=',1);path,run=admitted(path,True)
        tests=next((c for c in run['commands'] if c['label']=='tests'),None)
        text=(path/tests['stdout']).read_text() if tests else ''
        counts=re.search(r'(\d+)% tests passed, (\d+) tests failed out of (\d+)',text)
        result.append(dict(profile=label,status=run['status'],source_unchanged=run['source_unchanged'],run_record_sha256=retained.sha(path/'run.json'),system=run['system'],architecture=run['architecture'],compiler=run.get('compiler_version'),cache=run.get('cmake_cache'),inventory=run.get('test_count'),selected_tests=int(counts[3]) if counts else None,failed_tests=int(counts[2]) if counts else None,commands=[dict(label=c['label'],passed=c['passed'],exit_code=c['exit_code'],stdout_sha256=retained.sha(path/c['stdout']),stderr_sha256=retained.sha(path/c['stderr'])) for c in run['commands']],executables=run.get('build_executables')))
    write_json(root,'verification.json',dict(source_id=retained.source_manifest()[0],profiles=result,limits='PASS applies to named executed profiles only; failed sanitizer activities remain failed, not silently waived'))


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',required=True);p.add_argument('--study');p.add_argument('--engine');p.add_argument('--mechanisms');p.add_argument('--verify',action='append',default=[]);a=p.parse_args()
    root=Path(a.out);root.mkdir(parents=True,exist_ok=True)
    for name in ('study','engine','mechanisms'):
        if getattr(a,name):globals()[name](root,getattr(a,name))
    if a.verify:verification(root,a.verify)
    identity,manifest=retained.source_manifest();write_json(root,'source-manifest.json',dict(source_id=identity,files=manifest))
if __name__=='__main__':main()
