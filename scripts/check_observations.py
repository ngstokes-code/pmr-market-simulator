#!/usr/bin/env python3
"""Recompute each selected before/after state directly from raw wire bytes.

Independent Python price maps and ordered dictionaries; no production module.
The research analysis can consume this iterator instead of C++ observations.
"""
import argparse
import collections
import csv
import hashlib
import json
from pathlib import Path
from itch_reference import Reader, State, number

COLUMNS = 'ordinal timestamp kind locate ref new_ref side price new_price quantity match flag code paired imbalance far near reference bid ask bid_qty ask_qty level_qty level_orders order_qty ahead_orders ahead_qty post_bid post_ask post_bid_qty post_ask_qty post_level_qty birth_ahead birth_orders session trading operational active post_active high_timestamp'.split()
TEXT = set('kind side flag code session trading operational'.split())


def select_panel(path):
    eligible = set()
    for _, f in Reader(path):
        if f[0] == ord('R') and f[29] == ord('P') and chr(f[19]) in 'QGS':
            name = bytes(f[11:19])
            eligible.add((hashlib.sha256(name).hexdigest(), name, number(f, 1, 2)))
        if f[0] == ord('S') and f[11] == ord('Q'):
            return sorted(eligible)[:32]
    raise ValueError('panel selection requires Start of Market Hours')


class Observations:
    def __init__(self, path):
        self.path, self.panel = path, select_panel(path)
        self.reader, self.protocol = Reader(path), State()

    def __iter__(self):
        selected = {loc for _, _, loc in self.panel}
        books = collections.defaultdict(lambda: {'B': {}, 'S': {}})
        orders, regimes, high = {}, {}, collections.defaultdict(int)
        session, breach = '?', 0

        def regime(loc):
            return regimes.setdefault(loc, ['?', 'T', 0])
        def active(loc):
            trading, oper, update = regime(loc)
            return int(session == 'Q' and trading == 'T' and oper == 'T' and update > breach)
        def quote(loc):
            b = books[loc]
            bid, ask = max(b['B'], default=0), min(b['S'], default=0)
            return (bid, ask, sum(b['B'].get(bid, {}).values()), sum(b['S'].get(ask, {}).values()))
        def level(loc, side, price):
            return books[loc].get(side, {}).get(price, {})
        def add(loc, ref, side, price, qty):
            orders[ref] = (loc, side, price, qty)
            books[loc][side].setdefault(price, collections.OrderedDict())[ref] = qty
        def reduce(ref, qty):
            loc, side, price, own = orders[ref]
            q = books[loc][side][price]
            if qty == own:
                del orders[ref]; del q[ref]
                if not q: del books[loc][side][price]
            else:
                q[ref] -= qty; orders[ref] = loc, side, price, own - qty

        for n, f in self.reader:
            k, loc, t = chr(f[0]), number(f, 1, 2), number(f, 5, 6)
            self.protocol.accept(n, f)
            if loc not in selected and k not in ('S', 'W'):
                continue
            r = dict.fromkeys(COLUMNS, 0)
            r.update(ordinal=n, timestamp=t, kind=k, locate=loc, side='-', flag='-', code='-',
                     session=session, trading=regime(loc)[0], operational=regime(loc)[1], active=active(loc))
            for name, value in zip(('bid','ask','bid_qty','ask_qty'), quote(loc)):
                r[name] = value
            if k in ('A','F'):
                r.update(ref=number(f,11,8), side=chr(f[19]), quantity=number(f,20,4), price=number(f,32,4))
            elif k in ('E','C','X','D','U'):
                ref=number(f,11,8); _,side,price,own=orders[ref]
                r.update(ref=ref,side=side,price=price,order_qty=own)
                queue=level(loc,side,price)
                before=[]
                for key,qty in queue.items():
                    if key==ref: break
                    before.append(qty)
                r.update(ahead_orders=len(before),ahead_qty=sum(before))
                if k=='U': r.update(new_ref=number(f,19,8),quantity=number(f,27,4),new_price=number(f,31,4))
                else: r['quantity']=own if k=='D' else number(f,19,4)
                if k in ('E','C'):
                    r.update(match=number(f,23,8),flag='Y' if k=='E' else chr(f[31]))
                    if k=='C': r['reference']=number(f,32,4)
            elif k=='P': r.update(quantity=number(f,20,4),price=number(f,32,4),match=number(f,36,8))
            elif k=='Q': r.update(quantity=number(f,11,8),price=number(f,27,4),match=number(f,31,8),code=chr(f[39]))
            elif k=='B': r['match']=number(f,11,8)
            elif k in ('S','W'): r['code']=chr(f[11])
            elif k=='H': r['code']=chr(f[19])
            elif k=='h': r.update(code=chr(f[20]),flag=chr(f[19]))
            elif k=='I':
                r.update(paired=number(f,11,8),imbalance=number(f,19,8),side=chr(f[27]),far=number(f,36,4),near=number(f,40,4),reference=number(f,44,4),code=chr(f[48]),flag=chr(f[49]).replace(' ','_'))
            q=level(loc,r['side'],r['price'])
            r.update(level_qty=sum(q.values()),level_orders=len(q))
            if k in ('A','F'): add(loc,r['ref'],r['side'],r['price'],r['quantity'])
            elif k=='U':
                reduce(r['ref'],r['order_qty']); add(loc,r['new_ref'],r['side'],r['new_price'],r['quantity'])
            elif k in ('E','C','X','D'): reduce(r['ref'],r['quantity'])
            elif k=='S': session=r['code']
            elif k=='W': breach=n
            elif k=='H': regimes[loc][0],regimes[loc][2]=r['code'],n
            elif k=='h' and r['flag']=='Q': regimes[loc][1]=r['code']
            high[loc]=max(high[loc],t) if loc else high[loc]
            r.update(post_active=active(loc),high_timestamp=high[loc])
            for name,value in zip(('post_bid','post_ask','post_bid_qty','post_ask_qty'),quote(loc)): r[name]=value
            r['post_level_qty']=sum(level(loc,r['side'],r['price']).values())
            if k in ('A','F','U'):
                born=level(loc,r['side'],r['new_price'] if k=='U' else r['price'])
                r.update(birth_ahead=sum(born.values())-r['quantity'],birth_orders=len(born)-1)
            yield r
        self.protocol.finish()


def typed(row):
    return {k: v if k in TEXT else int(v) for k,v in row.items()}


def check(path, report, output):
    observations = Observations(path)
    with (Path(report)/'panel.csv').open(newline='') as stream:
        supplied=list(csv.DictReader(stream))
    expected=[dict(locate=str(loc),stock=name.decode().rstrip(' '),selection_sha256=digest) for digest,name,loc in observations.panel]
    if supplied!=expected: raise ValueError('panel disagreement')
    count=0
    with (Path(report)/'events.csv').open(newline='') as stream:
        supplied=iter(csv.DictReader(stream))
        for row in observations:
            candidate=next(supplied,None)
            if candidate is None or typed(candidate)!=row:
                raise ValueError(f'observation disagreement at {row["ordinal"]}: {candidate} != {row}')
            count+=1
        if next(supplied,None) is not None: raise ValueError('extra observation rows')
    summary=json.loads((Path(report)/'summary.json').read_text())
    result=dict(status='PASS',input_bytes=observations.reader.bytes,input_sha256=observations.reader.sha.hexdigest(),messages=observations.reader.messages,rows=count,panel_size=len(expected))
    for key in ('input_bytes','input_sha256','messages','rows','panel_size'):
        if summary[key]!=result[key]: raise ValueError('summary disagreement: '+key)
    Path(output).write_text(json.dumps(result,indent=2)+'\n')
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input');parser.add_argument('report');parser.add_argument('output')
    args=parser.parse_args();print(json.dumps(check(args.input,args.report,args.output)))
