#!/usr/bin/env python3
"""Original small ITCH session with cancellation-sensitive queue opportunity.

No downloaded data. Events and expected economic mechanisms are hand specified.
"""
import argparse
from pathlib import Path
import struct

OPEN=34200*10**9

def header(k,loc=1,t=OPEN):
    return k.encode()+struct.pack('>HH',loc,0)+t.to_bytes(6,'big')
def system(k,t): return header('S',0,t)+k.encode()
def directory(): return header('R',t=0)+b'ONE     Q'+bytes(9)+b'P'+bytes(9)
def add(ref,side,qty,price,t): return header('A',t=t)+struct.pack('>QcI8sI',ref,side.encode(),qty,b'ONE     ',price)
def execution(ref,qty,match,t): return header('E',t=t)+struct.pack('>QIQ',ref,qty,match)
def indication(code,t): return header('I',t=t)+struct.pack('>QQc8sIIIcc',80,20,b'B',b'ONE     ',100000,100005,100000,code.encode(),b' ')
def cross(code,match,t): return header('Q',t=t)+struct.pack('>Q8sIQc',80,b'ONE     ',100010,match,code.encode())
def trade(match,qty,t): return header('P',t=t)+struct.pack('>QcI8sIQ',0,b'B',qty,b'ONE     ',100000,match)
def rows():
    return [system('O',0),directory(),system('S',1),header('H',t=2)+b'ONE     T'+bytes(5),
            add(1,'B',100,100000,OPEN-2),add(2,'S',100,100100,OPEN-1),indication('O',OPEN-1),system('Q',OPEN),cross('O',500,OPEN),
            add(3,'B',20,100000,OPEN+50_000_000),header('X',t=OPEN+100_000_000)+struct.pack('>QI',1,90),
            execution(1,10,100,OPEN+200_000_000),execution(3,20,101,OPEN+300_000_000),
            add(4,'B',5,100000,OPEN+400_000_000),trade(102,7,OPEN+500_000_000),
            execution(4,1,103,OPEN+1_400_000_000),header('H',t=OPEN+2_000_000_000)+b'ONE     H'+bytes(5),
            header('B',t=OPEN+2_100_000_000)+struct.pack('>Q',102),
            system('M',57600*10**9),indication('C',57600*10**9),cross('C',501,57600*10**9),system('E',57600*10**9+1),system('C',57600*10**9+2)]
def write(path):
    data=b''.join(struct.pack('>H',len(r))+r for r in rows())
    Path(path).write_bytes(data)
    return data
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('output');args=p.parse_args();write(args.output)
