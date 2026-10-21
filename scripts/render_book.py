#!/usr/bin/env python3
"""Build bounded, reference-checked matching scenes for the offline visualizer."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


class Scene:
    def __init__(self, key, name, description, limits=(192,16,1000000), initial=0, pace=8):
        self.key,self.name,self.description=key,name,description
        self.limits,self.initial,self.pace=limits,initial,pace
        self.commands=[];self.labels=[];self.next_id=1

    def add(self,side,price,quantity,label='Orders join the queue',order_id=None):
        ref=self.next_id if order_id is None else order_id
        self.next_id=max(self.next_id,ref+1)
        self.commands.append(f'{len(self.commands)+1} add {ref} {side} {price} {quantity}')
        self.labels.append(label)
        return ref

    def cancel(self,ref,label='An order leaves without a trade'):
        self.commands.append(f'{len(self.commands)+1} cancel {ref}')
        self.labels.append(label)

    def text(self):
        return '# msim-trace 1\n# limits '+','.join(map(str,self.limits))+'\n'+'\n'.join(self.commands)+'\n'


def scenes():
    basic=Scene('basics','Start here','Each block is one order. Buyers wait on the left; sellers wait on the right.',pace=4)
    for side,p,q in [('B',9999,40),('S',10001,25),('B',9999,30),('S',10001,20),('S',10002,35),('B',9998,25)]:basic.add(side,p,q)
    basic.add('B',10001,30,'Oldest at this price goes first')
    basic.cancel(3,'Find an order by ID, then unlink it')
    basic.add('S',9998,60,'One order reaches two price levels')
    basic.add('B',10002,100,'Trade what can match; park the remainder')
    basic.add('S',10000,35,'The best available price wins')
    basic.add('S',10003,20,'The queues begin to build again')

    burst=Scene('burst','Burst','160 waiting orders. Rapid cancellations reuse space; large orders then move the best price.',initial=944,pace=32)
    live=[]
    for round_no in range(8):
        for level in range(10):
            for side,price in [('B',9999-level),('S',10001+level)]:
                live.append((burst.add(side,price,20+(round_no*11+level*7)%60,'Queues build across twenty prices'),side))
    for n in range(420):
        index=n*37%len(live);old,side=live[index]
        burst.cancel(old,'Cancel → free capacity → reuse')
        price=9999-(n//16)%12 if side=='B' else 10001+(n//16)%12
        live[index]=(burst.add(side,price,25+(n*17)%76,'New orders enter the back of their price queue'),side)
    burst.add('B',10020,1500,'A large buy consumes the cheapest sellers first')
    burst.add('S',9980,5000,'A large sell moves back through the buyers')
    burst.add('B',10020,5000,'Another buy walks back up through the sellers')

    sweep=Scene('sweep','Walk the book','One incoming order can consume several prices. Watch whole rows disappear.',initial=20,pace=3)
    for level in range(10):
        sweep.add('B',9999-level,40)
        sweep.add('S',10001+level,50)
    sweep.add('B',10005,250,'One buy clears five prices, best price first')
    sweep.add('S',9997,100,'A sell clears the best buyers and partially reduces the next')
    sweep.add('B',10008,150,'Three more selling prices disappear')
    sweep.add('S',10001,35,'A new seller improves the best selling price')
    sweep.add('B',10001,20,'A partial match leaves the same order at the front')
    sweep.add('B',10001,15,'The last shares remove that price level')

    limits=Scene('limits','Edge cases','When the remainder cannot fit, the whole order is rejected. No partial damage.',limits=(6,2,1000000),initial=6,pace=2)
    for side,p,q in [('B',9999,20),('B',9999,30),('B',9998,40),('S',10001,15),('S',10001,25),('S',10002,30)]:limits.add(side,p,q)
    limits.add('B',10003,80,'Could trade 70, but the remainder needs a third buying price: reject everything')
    limits.add('B',10002,70,'Exact fill succeeds even though the book started full')
    limits.add('S',10001,15,'Freed capacity is available to the next order')
    limits.add('B',9999,10,'A duplicate live ID cannot steal another order',order_id=1)
    limits.cancel(999999,'A missing ID changes nothing')
    limits.add('B',9997,0,'Zero quantity is refused before mutation')
    limits.cancel(1,'Cancel unlinks the oldest order without disturbing its neighbour')
    limits.add('B',9999,20,'A retired ID may be reused in the synthetic matching engine',order_id=1)
    return [basic,burst,sweep,limits]


def flat(frame):
    return {ref:[ref,side,price,qty,rank] for side,name in [('B','bids'),('S','asks')]
            for price,orders in frame[name] for rank,(ref,qty) in enumerate(orders)}


def build_payload(binary,source_id,evidence=None):
    cases=[]
    for scene in scenes():
        with tempfile.TemporaryDirectory(prefix='msim-book-scene-') as temporary:
            trace=Path(temporary)/'scene.trace';trace.write_text(scene.text())
            process=subprocess.run([str(binary),str(trace),'--json'],capture_output=True,text=True)
        if evidence:
            evidence.mkdir(parents=True,exist_ok=True)
            (evidence/(scene.key+'.trace')).write_text(scene.text())
            (evidence/(scene.key+'.stdout')).write_text(process.stdout)
            (evidence/(scene.key+'.stderr')).write_text(process.stderr)
        if process.returncode:
            raise ValueError('checked scene failed: '+scene.key+' '+process.stderr)
        raw=json.loads(process.stdout)
        if raw.get('verified') is not True or len(raw['frames'])!=len(scene.commands):
            raise ValueError('incomplete semantic gate: '+scene.key)
        previous={};frames=[];trades=[];executed=0
        for n,frame in enumerate(raw['frames']):
            if frame['seq']!=n+1:raise ValueError('nonsequential scene')
            state=flat(frame)
            changed=[order for ref,order in state.items() if previous.get(ref)!=order]
            removed=sorted(set(previous)-set(state))
            reconstructed=previous.copy()
            for ref in removed:del reconstructed[ref]
            for order in changed:reconstructed[order[0]]=order
            if reconstructed!=state:raise ValueError('snapshot patch disagrees')
            for resting,incoming,price,qty in frame['executions']:
                trades.append([frame['seq'],resting,incoming,price,qty]);executed+=qty
            accepted=frame['status']=='accepted'
            if frame['kind']=='add' and not accepted and (state!=previous or frame['executions']):
                raise ValueError('rejected order changed the book')
            frame.update(updates=changed,remove=removed,label=scene.labels[n],orders=len(state),
                         levels=len(frame['bids'])+len(frame['asks']),trades=len(trades),executed=executed,
                         shares=sum(o[3] for o in state.values()))
            del frame['bids'];del frame['asks']
            frames.append(frame);previous=state
        cases.append(dict(key=scene.key,name=scene.name,description=scene.description,initial=scene.initial,pace=scene.pace,
                          capacity=scene.limits[0],level_capacity=scene.limits[1],trace_sha256=hashlib.sha256(scene.text().encode()).hexdigest(),
                          peak_orders=max(f['orders'] for f in frames),peak_levels=max(f['levels'] for f in frames),frames=frames))
    return dict(schema=1,source_id=source_id,executable_sha256=hashlib.sha256(Path(binary).read_bytes()).hexdigest(),cases=cases)


def encode(payload):
    return '// Generated by scripts/render_book.py; every command and full FIFO state checked against ReferenceBook.\nwindow.MSIM_BOOK = '+json.dumps(payload,sort_keys=True,separators=(',',':'))+';\n'


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build',required=True,type=Path)
    parser.add_argument('--out',required=True,type=Path)
    parser.add_argument('--evidence',type=Path)
    parser.add_argument('--check',action='store_true')
    args=parser.parse_args()
    import run
    source=run.source_manifest()[0];run.require_build_identity(args.build,source)
    text=encode(build_payload(args.build/'diff_replay',source,args.evidence))
    if args.check:
        if args.out.read_text()!=text:raise ValueError('published matching scenes differ from checked generation')
    else:args.out.write_text(text)
    print('Matching scenes: complete outcome, execution, FIFO and invariant agreement')


if __name__=='__main__':main()
