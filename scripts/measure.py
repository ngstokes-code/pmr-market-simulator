#!/usr/bin/env python3
"""Controlled serial engine measurements and bounded mechanism diagnostics.

Admission policy is sampled host observation, not physical isolation. Preserve
all repetitions and reject the entire activity on contamination. No day replay.
"""
import argparse
import csv
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import time
import run as retained
from make_replay_workload import write as workload, prefix as real_prefix

COMPUTE=re.compile(r'^(python[0-9.]*|Python|node|clang[+0-9.-]*|cc1.*|g[+0-9.-]+|c[+0-9.-]+|cmake|make|ninja|rustc|cargo|book_bench|structure_bench|replay_bench|itch_.*|.*_test)$')


def process_sample(owned):
    result=subprocess.run(['ps','-axo','pid=,ppid=,%cpu=,comm='],capture_output=True,text=True,check=True)
    rows=[]
    for line in result.stdout.splitlines():
        fields=line.split(None,3)
        if len(fields)!=4:continue
        pid,parent,cpu,command=fields
        rows.append(dict(pid=int(pid),parent=int(parent),cpu=float(cpu),command=command))
    descendants=set(owned)
    while True:
        before=len(descendants)
        descendants.update(row['pid'] for row in rows if row['parent'] in descendants)
        if len(descendants)==before:break
    violations=[]
    for row in rows:
        name=Path(row['command']).name
        if row['pid'] not in descendants and (row['cpu']>=100 or (COMPUTE.fullmatch(name) and row['cpu']>=5)):
            violations.append(row)
    return dict(monotonic=time.monotonic(),processes=rows,violations=violations)


def power():
    if sys.platform!='darwin':return dict(policy='AC and pageouts enforced on the macOS measurement host only')
    battery=subprocess.check_output(['pmset','-g','batt'],text=True)
    thermal=subprocess.run(['pmset','-g','therm'],capture_output=True,text=True)
    vm=subprocess.check_output(['vm_stat'],text=True)
    match=re.search(r'Pageouts:\s+(\d+)',vm)
    # Missing thermal telemetry is recorded, never promoted to a verified limit.
    return dict(ac_power='AC Power' in battery,battery=battery,thermal=thermal.stdout+thermal.stderr,pageouts=int(match[1]) if match else None)


class Controlled:
    def __init__(self,run):
        self.run=run;self.samples=[];self.initial=power()
        run.meta['measurement_policy']=dict(sample_seconds=.25,start_quiet_samples=3,start_sample_seconds=1,
            competing_compute_percent=5,any_competing_process_percent=100,minimum_engine_group_kernel_ms=1000,
            physical_isolation=False,frequency_affinity_cache_residency='unverified',power_before=self.initial)
        run.save();self.save()
        if self.initial.get('ac_power') is False:raise RuntimeError('controlled host must use AC power')
        for _ in range(3):
            sample=process_sample({os.getpid()});self.samples.append(sample);self.save()
            if sample['violations']:raise RuntimeError('host admission failed; competing process')
            time.sleep(1)
        self.save()

    def save(self):
        (self.run.output/'host-observations.json').write_text(json.dumps(self.samples,indent=2)+'\n')

    def command(self,label,args):
        args=list(map(str,args));stdout=self.run.output/(label+'.stdout');stderr=self.run.output/(label+'.stderr')
        command=dict(label=label,argv=args,cwd=str(retained.ROOT),executable_sha256=retained.sha(args[0]),conditions=retained.conditions())
        started=time.monotonic();contaminated=False;child=None
        try:
            with stdout.open('wb') as out,stderr.open('wb') as err:
                child=subprocess.Popen(args,stdout=out,stderr=err,cwd=retained.ROOT)
                while True:
                    sample=process_sample({os.getpid(),child.pid});self.samples.append(sample)
                    contaminated|=bool(sample['violations'])
                    child_pid,status,usage=os.wait4(child.pid,os.WNOHANG)
                    if child_pid:
                        child.returncode=os.waitstatus_to_exitcode(status);break
                    time.sleep(.25)
            final_power=power()
            if final_power.get('ac_power') is False:contaminated=True
            if self.initial.get('pageouts') is not None and final_power.get('pageouts')!=self.initial['pageouts']:contaminated=True
            command.update(exit_code=child.returncode,wall_seconds=time.monotonic()-started,user_seconds=usage.ru_utime,
                           system_seconds=usage.ru_stime,max_rss_bytes=usage.ru_maxrss*(1 if sys.platform=='darwin' else 1024),
                           contaminated=contaminated,power_after=final_power,stdout=stdout.name,stderr=stderr.name)
            command['passed']=child.returncode==0 and not contaminated
            if not command['passed']:raise RuntimeError('measurement rejected: '+label)
        except BaseException as error:
            if child is not None and child.returncode is None:
                child.terminate()
                try:child.wait(timeout=5)
                except subprocess.TimeoutExpired:child.kill();child.wait()
            command.update(passed=False,observer_or_child_error=str(error),exit_code=child.returncode if child else None)
            raise
        finally:
            command['stdout_sha256']=retained.sha(stdout) if stdout.exists() else None
            command['stderr_sha256']=retained.sha(stderr) if stderr.exists() else None
            self.run.meta['commands'].append(command);self.run.save();self.save()
            print(label,'PASS' if command.get('passed') else 'FAIL',flush=True)
        return stdout


def engine(run,controlled,build):
    binary=build/'book_bench'
    for name in ('stable_depth','cancel_churn','many_prices','multi_level_sweep','bursty'):
        stream=run.output/(name+'.cmds')
        run.command(name+'-generate',[binary,'generate','--workload',name,'--seed',17,'--commands',1000000,'--out',stream])
        run.command(name+'-verify',[binary,'verify','--stream',stream])
        run.meta.setdefault('inputs',{})[name]=dict(bytes=stream.stat().st_size,sha256=retained.sha(stream));run.save()
        for mode,batch in (('throughput',16),('latency',16),('batch',16),('batch',64)):
            label=name+'-'+mode+(str(batch) if mode=='batch' else '')
            destination=run.output/(label+'.json')
            controlled.command(label,[binary,'run','--stream',stream,'--mode',mode,'--batch',batch,'--reps',5,'--warmup',1,'--minimum-ms',1000,'--json',destination])
            result=json.loads(destination.read_text())
            if result['measured_cpp_new_calls'] or result['outcomes']['internal_allocations_during_replay'] or result['outcomes']['execution_output_growths']:
                raise RuntimeError('measured path allocated')
            if mode=='throughput' and any(r['run_ms']<1000 for r in result['reps']):raise RuntimeError('insufficient aggregate kernel duration')
            run.meta['commands'][-1].update(result_file=destination.name,result_sha256=retained.sha(destination));run.save()


def mechanisms(run,controlled,build,real_input=None):
    # Balanced arm ordering across fixed sizes/localities. Every result is kept.
    for levels in (32,512,4096):
        for locality in ('near','uniform'):
            arms=('vector','tree') if (levels==512)==(locality=='near') else ('tree','vector')
            for arm in arms:
                controlled.command(f'ladder-{levels}-{locality}-{arm}',[build/'structure_bench','ladder',arm,levels,2000000,locality])
    for pattern in ('recent','aged','jump'):
        for arm in (('window','hash') if pattern!='aged' else ('hash','window')):
            controlled.command('index-'+pattern+'-'+arm,[build/'structure_bench','index',arm,18,2000000,pattern])
    for skew in ('balanced','hot'):
        source=run.output/(skew+'.itch');workload(source,1000000,skew)
        run.meta.setdefault('inputs',{})[skew]=dict(bytes=source.stat().st_size,sha256=retained.sha(source));run.save()
        for workers in (1,2,4,8):
            controlled.command(f'pipeline-{skew}-t{workers}',[build/'replay_bench',source,workers,64000,'window','identity','plain','warm',3])
        if skew=='balanced':
            for name,chunk,index,identity,profile,cache in (
                ('chunk1000',1000,'window','identity','plain','warm'),
                ('chunk1000000',1000000,'window','identity','plain','warm'),
                ('hash',64000,'hash','identity','plain','warm'),
                ('identity-off',64000,'window','unsafe','plain','warm'),
                ('stages',64000,'window','identity','profile','warm'),
                ('cache-pressure',64000,'window','identity','plain','perturbed')):
                controlled.command('pipeline-balanced-'+name,[build/'replay_bench',source,4,chunk,index,identity,profile,cache,3])

    if real_input:
        source=run.output/'real-prefix.itch'
        provenance=real_prefix(real_input,source,200000)
        provenance.update(bytes=source.stat().st_size,sha256=retained.sha(source))
        run.meta.setdefault('inputs',{})['real_prefix']=provenance;run.save()
        for workers in (1,2,4):
            controlled.command(f'pipeline-real-prefix-t{workers}',[build/'replay_bench',source,workers,64000,'window','identity','plain','warm',3,2000000])


def profile(run,build):
    # Diagnostic stack sample is deliberately outside the admitted timing set.
    stream=run.output/'profile.cmds'
    run.command('profile-generate',[build/'book_bench','generate','--workload','many_prices','--seed',17,'--commands',1000000,'--out',stream])
    args=[str(build/'book_bench'),'run','--stream',str(stream),'--mode','throughput','--reps','1','--warmup','0','--minimum-ms','20000','--json',str(run.output/'profile-kernel.json')]
    if sys.platform!='darwin':raise RuntimeError('maintained stack sample currently requires macOS sample')
    with (run.output/'profile-child.stdout').open('wb') as out,(run.output/'profile-child.stderr').open('wb') as err:
        child=subprocess.Popen(args,stdout=out,stderr=err)
        try:
            run.command('stack-sample',['sample',str(child.pid),'10','1','-file',run.output/'stacks.txt'])
        finally:
            status=child.wait()
        if status:raise RuntimeError('profiled kernel failed')
    run.meta['profile_scope']='statistical stacks of setup plus repeated matching kernel, not hardware counters; sampled rates excluded'
    run.meta['profile_child_argv']=args


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('activity',choices=('engine','mechanisms','profile'));p.add_argument('--build',required=True);p.add_argument('--out',required=True);p.add_argument('--real-input');a=p.parse_args()
    build=Path(a.build).resolve();run=retained.Run(a.out,build,a.real_input);passed=False
    run.meta['measurement_activity']=a.activity;run.save()
    try:
        retained.require_build_identity(build,run.identity)
        cache=(build/'CMakeCache.txt').read_text()
        if 'CMAKE_BUILD_TYPE:STRING=Release' not in cache or re.search(r'^MSIM_SANITIZE:STRING=.+',cache,re.M):
            raise RuntimeError('measurements require a Release build without sanitizers')
        import shlex
        compile_commands=json.loads((build/'compile_commands.json').read_text())
        for entry in compile_commands:
            flags=entry.get('arguments') or shlex.split(entry['command'])
            optimization=[flag for flag in flags if re.fullmatch(r'-O[0-3szg]',flag)]
            if not optimization or optimization[-1]!='-O3' or any(flag.startswith('-fsanitize') for flag in flags):
                raise RuntimeError('controlled profile requires final -O3 and no sanitizer instrumentation')
        stamp=retained.executables(build)
        if a.activity=='profile':profile(run,build)
        else:
            controlled=Controlled(run)
            if a.activity=='engine':engine(run,controlled,build)
            else:mechanisms(run,controlled,build,a.real_input)
        if stamp!=retained.executables(build):raise RuntimeError('executables changed during measurement')
        passed=True
    finally:run.finish(passed)
if __name__=='__main__':main()
