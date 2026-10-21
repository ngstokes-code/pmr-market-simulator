#!/usr/bin/env python3
"""Correction attribution, raw-byte agreement, and report rollback."""
import csv
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from cli_failure import rejected

binary, checker = sys.argv[1:]
sys.path.insert(0, str(Path(checker).parent))
spec = importlib.util.spec_from_file_location('independent', checker)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

def h(k, loc=1, t=0):
    return k.encode() + struct.pack('>HH', loc, 0) + t.to_bytes(6, 'big')
def r(loc, name):
    return h('R', loc) + name.ljust(8).encode() + b'Q' + bytes(9) + b'P' + bytes(9)
def a(ref=10, name='ONE', kind='A'):
    return h(kind) + struct.pack('>QcI8sI', ref, b'B', 1000, name.ljust(8).encode(), 100) + (b'MPID' if kind == 'F' else b'')
def e(match, qty, t=0, printable=None, ref=10):
    row = h('E' if printable is None else 'C', t=t) + struct.pack('>QIQ', ref, qty, match)
    return row if printable is None else row + printable.encode() + struct.pack('>I', 100)
def p(match, qty, loc=1, t=0):
    return h('P', loc, t) + struct.pack('>QcI8sIQ', 0, b'B', qty, ('ONE' if loc == 1 else 'TWO').ljust(8).encode(), 100, match)
def b(match, loc=1):
    return h('B', loc) + struct.pack('>Q', match)
def complete(rows,before_system=(),after_system=()):
    rows = [h('S', 0) + b'O', r(1, 'ONE'), r(2, 'TWO')] + list(before_system) + [h('S', 0)+b'S', h('S', 0)+b'Q'] + rows + [h('S',0)+c for c in (b'M', b'E')] + list(after_system) + [h('S',0)+b'C']
    return b''.join(struct.pack('>H', len(row)) + row for row in rows)

with tempfile.TemporaryDirectory(prefix='msim-ledger-test-') as tmp:
    root, source = Path(tmp), Path(tmp) / 'input.itch'
    rows = [a(), e(1,100), p(2,40,t=300*10**9), e(3,60,300*10**9,'Y'),
            e(4,50,printable='N'), h('Q')+struct.pack('>Q8sIQc',100,b'ONE     ',100,5,b'O'),
            b(1), b(2), p(6,20,2,300*10**9), a(11), e(7,100,printable='N'), e(7,100,ref=11), b(7)]
    source.write_bytes(complete(rows))
    for partitions in (1,4):
        report = root / f'report{partitions}'
        run = subprocess.run([binary,str(source),str(report),'--partitions',str(partitions)],capture_output=True,text=True)
        assert run.returncode == 0 and 'result: SUCCESS' in run.stdout, run.stderr
        summary=json.loads((report/'summary.json').read_text())
        assert [summary[k] for k in ('E','CY','P','Q','CN','breaks','corrected_shares','companion_groups','corrected_nonprintable_shares')] == [0,60,20,100,50,3,240,1,100]
        module.check(source,report,root/f'check{partitions}.json',partitions)
    for field,replacement in (('stock','WRONG'),('non_displayed_fraction','0.5')):
        surface=root/'report1'/'symbols.csv'; retained=surface.read_bytes()
        with surface.open(newline='') as stream:
            reader=csv.DictReader(stream); fields=reader.fieldnames; data=list(reader)
        data[0][field]=replacement
        with surface.open('w',newline='') as stream:
            writer=csv.DictWriter(stream,fields); writer.writeheader(); writer.writerows(data)
        try: module.check(source,root/'report1',root/'forged.json',1)
        except ValueError: pass
        else: raise AssertionError('checker accepted forged '+field)
        surface.write_bytes(retained)
    bad_cases=(([a(), e(80,10), b(80,2)],'break crosses symbols'),([a(name='TWO')],'order symbol'),
               ([a(kind='F',name='TWO')],'order symbol'),([a(),e(99,1001)],'excessive reduction'),
               ([a(),b(999)],'unknown correction'),([a(),a()],'reused day reference'),
               ([a(),e(1,100,printable='?')],'invalid printable flag'))
    for i,(bad,reason) in enumerate(bad_cases):
        source.write_bytes(complete(bad)); report=root/f'bad{i}'
        run=subprocess.run([binary,str(source),str(report)],capture_output=True,text=True)
        rejected(run)
        assert not report.exists(),run.stdout+run.stderr
        try: module.check(source,root/'report1',root/'bad-check.json',1)
        except ValueError as error:assert reason in str(error),str(error)
        else: raise AssertionError('independent checker accepted malformed input')
    for closed in (False,True):
        for kind in ('A','F','U'):
            birth=h('U')+struct.pack('>QQII',10,11,10,100) if kind=='U' else a(11,kind=kind)
            source.write_bytes(complete([a()] if closed else [],after_system=[birth]) if closed else complete([],before_system=[birth]))
            report=root/f'outside-system-{closed}-{kind}'
            run=subprocess.run([binary,str(source),str(report)],capture_output=True,text=True)
            rejected(run,'order birth outside system hours');assert not report.exists()
            # Check the semantic cause directly, without a mismatching report
            # identity being allowed to masquerade as the intended rejection.
            protocol=module.State()
            try:
                for ordinal,frame in module.Reader(source):protocol.accept(ordinal,frame)
            except ValueError as error:assert str(error)=='order birth outside system hours'
            else:raise AssertionError('raw protocol accepted an out-of-system birth')
    source.write_bytes(complete(rows))
    with open(os.devnull,'rb') as sink:
        run=subprocess.run([binary,str(source),str(root/'stdout-failure')],stdout=sink,stderr=subprocess.PIPE,text=True)
    rejected(run,'stdout write failed')
    assert not (root/'stdout-failure').exists(),run.stderr
    assert not list(root.glob('*.tmp.*'))
print('correction attribution, raw-byte agreement, report rollback: PASS')
