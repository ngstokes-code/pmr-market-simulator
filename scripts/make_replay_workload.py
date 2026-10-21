#!/usr/bin/env python3
"""Deterministic matched balanced/hot-locate feeds for bounded diagnostics.

Same order sizes/prices/lifecycles and total event count, locate mapping differs.
This is a synthetic ownership experiment, not an arrival-process model.
"""
import argparse
from pathlib import Path
import struct
from make_demo import header,system,OPEN


def write(path,commands=1000000,skew='balanced'):
    if not 100<=commands<=10000000 or skew not in ('balanced','hot'):
        raise ValueError('bounded workload size and supported skew required')
    with Path(path).open('wb') as out:
        def emit(row):out.write(struct.pack('>H',len(row)));out.write(row)
        emit(system('O',0))
        names={loc:f'SYM{loc:05}'.encode() for loc in range(1,17)}
        for loc,name in names.items():
            emit(header('R',loc,0)+name+b'Q'+bytes(9)+b'P'+bytes(9))
            emit(header('H',loc,0)+name+b'T'+bytes(5))
        emit(system('S',1));emit(system('Q',OPEN))
        # Keep one persistent quote on each side, then independent transient births.
        ref=0
        for loc,name in names.items():
            for side,price in ((b'B',100000),(b'S',100100)):
                ref+=1;emit(header('A',loc,OPEN)+struct.pack('>QcI8sI',ref,side,100,name,price))
        for i in range(commands//2):
            loc=1 if skew=='hot' and i%10<8 else i%16+1
            ref+=1;stamp=OPEN+i+1
            emit(header('A',loc,stamp)+struct.pack('>QcI8sI',ref,b'B',100,names[loc],99900))
            emit(header('D',loc,stamp)+struct.pack('>Q',ref))
        emit(system('M',57600*10**9));emit(system('E',57600*10**9+1));emit(system('C',57600*10**9+2))


def prefix(source,path,regular_messages=200000):
    """All starting state plus a fixed regular-session prefix; synthetic close.

    Never presents the appended terminal markers as observed source events.
    The enclosing retained run hashes the original input before and after.
    """
    from itch_reference import Reader, number
    import os
    initial=os.stat(source)
    in_regular=False;regular=0;messages=0;maximum=0
    with Path(path).open('wb') as out:
        for _,frame in Reader(source):
            kind=chr(frame[0]);maximum=max(maximum,number(frame,5,6))
            if kind=='S' and frame[11]==ord('M'):break
            payload=bytes(frame);out.write(struct.pack('>H',len(payload)));out.write(payload);messages+=1
            if kind=='S' and frame[11]==ord('Q'):in_regular=True;continue
            if in_regular:
                regular+=1
                if regular==regular_messages:break
        if regular!=regular_messages:raise ValueError('source has insufficient specified regular-session prefix')
        if maximum+3>=86400*10**9:raise ValueError('no timestamp space for synthetic terminal markers')
        for i,code in enumerate('MEC',1):
            payload=system(code,maximum+i);out.write(struct.pack('>H',len(payload)));out.write(payload)
    final=os.stat(source)
    fields=('st_dev','st_ino','st_size','st_mtime_ns','st_ctime_ns')
    if any(getattr(initial,k)!=getattr(final,k) for k in fields):raise ValueError('source changed during prefix extraction')
    return dict(scope='original start through fixed regular-session message count; synthetic M/E/C appended',
                observed_messages=messages,regular_messages=regular,synthetic_terminal_messages=3)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('output');p.add_argument('--commands',type=int,default=1000000);p.add_argument('--skew',default='balanced');a=p.parse_args();write(a.output,a.commands,a.skew)
