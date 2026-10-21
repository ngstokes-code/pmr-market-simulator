#!/usr/bin/env python3
"""Complete-session CLI boundaries, deterministic files, and late-write rollback."""
import json
import os
from pathlib import Path
import resource
import struct
import subprocess
import sys
import tempfile

binary=sys.argv[1]
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from make_demo import rows,header,add,execution,OPEN
from cli_failure import rejected,check_predicate

def wire(items):return b''.join(struct.pack('>H',len(r))+r for r in items)
def run(source,*args,**kwargs):return subprocess.run([binary,str(source),*map(str,args)],stderr=subprocess.PIPE,**kwargs)
check_predicate()

with tempfile.TemporaryDirectory(prefix='msim-replay-boundary-') as tmp:
    root=Path(tmp);source=root/'input.itch';data=wire(rows());source.write_bytes(data)
    reference=None
    for workers,chunk in ((1,1),(2,3),(3,7),(4,31),(4,1)):
        out=root/f'complete-{workers}-{chunk}'
        result=run(source,'--threads',workers,'--chunk',chunk,'--out',out,stdout=subprocess.PIPE)
        assert result.returncode==0,result.stdout+result.stderr
        files={p.name:p.read_bytes() for p in out.iterdir() if p.name!='result.json'}
        if reference is None:reference=files
        else:assert files==reference
        summary=json.loads((out/'result.json').read_text());assert summary['terminal_event']=='C' and summary['rejected_events']==0
    for suffix in ('truncated','short','extra','unknown'):
        bad={'truncated':data[:-1],'short':b'\x00\x01S','extra':data+b'\x00','unknown':wire([header('?')])}[suffix]
        source.write_bytes(bad);out=root/suffix
        result=run(source,'--out',out,stdout=subprocess.PIPE)
        rejected(result)
        assert not out.exists() and b'result: SUCCESS' not in result.stdout
    base=rows()
    invalid=[]
    # These retain complete framing/session markers: rejection is semantic.
    x=base.copy();x.insert(10,add(3,'B',1,100000,OPEN));invalid.append(x) # live duplicate
    x=base.copy();x.insert(10,execution(1,101,777,OPEN));invalid.append(x)
    x=base.copy();x.insert(10,header('U')+struct.pack('>QQII',1,2,1,100000));invalid.append(x)
    x=base.copy();bad=bytearray(add(99,'B',1,100000,OPEN));bad[24:32]=b'TWO     ';x.insert(10,bytes(bad));invalid.append(x)
    x=base.copy();x.insert(-5,add(1,'B',1,100000,OPEN));invalid.append(x) # retired reference
    x=base.copy();x.insert(-3,header('S',0)+b'Q');invalid.append(x)
    x=base.copy();x.insert(1,add(99,'B',1,100000,OPEN));invalid.append(x) # before directory
    x=base.copy();x.insert(2,add(99,'B',1,100000,OPEN));invalid.append(x) # before system open
    x=base.copy();x.insert(-1,add(99,'B',1,100000,OPEN));invalid.append(x) # after system close
    x=base.copy();x.insert(-1,header('U')+struct.pack('>QQII',4,99,1,100000));invalid.append(x)
    x=base.copy();bad=bytearray(header('D')+struct.pack('>Q',1));bad[1:3]=b'\x00\x02';x.insert(10,bytes(bad));invalid.append(x)
    for i,items in enumerate(invalid):
        source.write_bytes(wire(items));out=root/f'semantic{i}'
        result=run(source,'--threads',3,'--chunk',2,'--out',out,stdout=subprocess.PIPE)
        rejected(result)
        assert not out.exists(),result.stdout+result.stderr
    source.write_bytes(data)
    for flag in ('--max-orders','--worker-orders','--max-levels','--output-records','--cross-records'):
        out=root/flag[2:];result=run(source,flag,1,'--out',out,stdout=subprocess.PIPE)
        rejected(result)
        assert not out.exists(),result.stdout+result.stderr
    # Exact inode aliasing must be rejected before touching the source.
    alias=root/'alias.csv';os.link(source,alias)
    result=run(source,'--crosses',alias,stdout=subprocess.PIPE)
    rejected(result,'aliases input')
    assert source.read_bytes()==data and alias.read_bytes()==data
    # Existing output survives early AND final-console failures.
    crosses=root/'crosses.csv';crosses.write_bytes(b'PREVIOUS\n')
    with open(os.devnull,'rb') as sink:
        result=run(source,'--crosses',crosses,stdout=sink)
    rejected(result,'write failed')
    assert crosses.read_bytes()==b'PREVIOUS\n'
    success=run(source,stdout=subprocess.PIPE)
    assert success.returncode==0,success.stdout+success.stderr
    first=len(success.stdout.splitlines(keepends=True)[0])
    reached_late=False
    for extra in (5,20,35,50,65):
        limit=first+extra
        def limited():resource.setrlimit(resource.RLIMIT_FSIZE,(limit,limit))
        console=root/f'limited-{extra}.txt'
        with console.open('wb') as sink:result=run(source,'--crosses',crosses,stdout=sink,preexec_fn=limited)
        output=console.read_bytes()
        if result.returncode==0:crosses.write_bytes(b'PREVIOUS\n');continue
        rejected(result,'write failed')
        assert crosses.read_bytes()==b'PREVIOUS\n',result.stderr
        if b'\nresult: SUCC' in output:reached_late=True;break
    assert reached_late,'late console failure was not exercised'
    # File-size write failure during a fresh report must not leave a report.
    def tiny():resource.setrlimit(resource.RLIMIT_FSIZE,(2048,2048))
    many=rows();many[9:9]=[header('Q')+struct.pack('>Q8sIQc',1,b'ONE     ',100000,1000+i,b'O') for i in range(100)]
    source.write_bytes(wire(many))
    result=run(source,'--out',root/'small-file',stdout=subprocess.PIPE,preexec_fn=tiny)
    rejected(result,'output write failed') # a silent Linux TSan SIGBUS must fail
    assert not (root/'small-file').exists()
    # Rename over a directory fails without replacing its prior contents.
    destination=root/'destination';destination.mkdir();(destination/'owned').write_text('PRESERVE')
    result=run(source,'--crosses',destination,stdout=subprocess.PIPE)
    rejected(result,'cannot publish output')
    assert (destination/'owned').read_text()=='PRESERVE'
    assert not list(root.glob('*.tmp.*')) and not list(root.glob('*.rollback.*'))
print('complete-session rejection, exact worker files, resource failure, publication rollback: PASS')
