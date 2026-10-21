#!/usr/bin/env python3
"""Finite-input displayed-queue study and explicit one-share service scenarios.

Consumes either checked C++ observations or independently decoded raw bytes.
No exchange allocation, market impact, routing, P&L or causal estimand.
"""
import argparse
import collections
import csv
import hashlib
import heapq
import json
from pathlib import Path
import tempfile
import os
from check_observations import Observations, typed

SECOND=10**9
BIN=300*SECOND
BLOCK=1800*SECOND


SCHEMAS={
 'lifetimes':'locate ref birth_ordinal arrival_ns side price quantity initial_ahead initial_orders_ahead ahead_bin spread best_depth block_ns own_execution own_cancellation partial_execution partial_cancellation end_ns elapsed_ns endpoint followup followup_end_ns ahead_execution ahead_cancellation complete_followup',
 'depletions':'locate side price ordinal start_ns level_birth_ordinal level_birth_ns level_age_ns cause spread spread_units execution_reduction cancellation_reduction end_ns elapsed_ns endpoint event_distance restored_best spread_change',
 'opportunities':'locate bin entry_ordinal entry_ns side price initial_ahead entry_mid spread stop_ns stop_reason depleted_within_horizon time_to_level_depletion_ns depletion_followup depletion_observation_ns lost_best_within_horizon time_to_lost_best_ns pessimistic_opportunity pessimistic_followup pessimistic_time_to_front_ns pessimistic_time_to_opportunity_ns pessimistic_adverse_half_units pessimistic_adverse_reason optimistic_opportunity optimistic_followup optimistic_time_to_front_ns optimistic_time_to_opportunity_ns optimistic_adverse_half_units optimistic_adverse_reason',
 'auction_pairs':'locate cross ordinal timestamp shares price indications episode last_ordinal staleness_ns paired imbalance direction far far_difference near near_difference reference reference_difference',
 'indications':'locate ordinal cross paired imbalance direction far near reference lead_ns cross_ordinal',
 'context_volume':'locate block_ns spread depth_bin active session trading operational distinct_matches E CY P Q CN records broken_records corrected_E corrected_CY corrected_P corrected_Q corrected_CN unbroken_reported_shares bid_depth_share_sum ask_depth_share_sum eligible_shares p_fraction',
 'sensitivity':'locate stock block_ns births complete_followup complete_execution_endpoints followup_LATE_ENTRY followup_CIRCUIT_BREAKER followup_TRADING_STATE followup_CLOCK_REGRESSION E C X D U HORIZON TRADING_STATE SESSION_END CLOCK_REGRESSION OBSERVATION_END own_execution own_cancellation ahead_execution ahead_cancellation partial_execution partial_cancellation execution_endpoint_fraction',
 'lifetime_groups':'locate block_ns spread ahead_bin births complete_followup complete_execution_endpoints followup_LATE_ENTRY followup_CIRCUIT_BREAKER followup_TRADING_STATE followup_CLOCK_REGRESSION E C X D U HORIZON own_execution own_cancellation ahead_execution ahead_cancellation',
 'study_sensitivity':'locate stock block_ns depletions returned no_return depletion_censored probes complete_probes censored_probes probe_level_depleted probe_level_no_depletion probe_level_censored pessimistic_opportunities optimistic_opportunities assumption_sensitive_probes probe_attempts probe_excluded probe_no_event',
 'depletion_groups':'locate block_ns spread cause depletions returned no_return censored restored_best',
 'probe_groups':'locate block_ns spread side probes complete_probes censored_probes probe_level_depleted probe_level_no_depletion probe_level_censored pessimistic_opportunities optimistic_opportunities assumption_sensitive_probes',
 'exclusions':'locate block_ns reason count',
}


def spread(bid,ask):
    if not bid or not ask: return 'EMPTY'
    if ask<=bid: return 'LOCKED_CROSSED'
    return 'SUBCENT' if ask-bid<100 else 'ONE_CENT' if ask-bid==100 else 'WIDER'


def midpoint(row,post=True):
    prefix='post_' if post else ''
    b,a=row[prefix+'bid'],row[prefix+'ask']
    return b+a if b and a>b else None  # integer half-Price(4) units


class Prefix:
    """Two cumulative departure trees, independently of the live C++ queues."""
    def __init__(self): self.trees=[[0],[0]]
    def prefix(self,k,n):
        tree=self.trees[k]; value=0
        while n: value+=tree[n];n-=n&-n
        return value
    def append(self):
        n=len(self.trees[0])
        for k,tree in enumerate(self.trees):
            tree.append(self.prefix(k,n-1)-self.prefix(k,n-(n&-n)))
        return n
    def add(self,k,n,value):
        tree=self.trees[k]
        while n<len(tree): tree[n]+=value;n+=n&-n


class Study:
    def __init__(self,panel,root):
        self.root=Path(root);self.panel=panel;self.files={};self.writers={}
        for name,schema in SCHEMAS.items():
            f=(self.root/(name+'.csv')).open('w',newline='');self.files[name]=f
            self.writers[name]=csv.DictWriter(f,fieldnames=schema.split());self.writers[name].writeheader()
        self.secondary=collections.defaultdict(collections.Counter)
        self.depletion_groups=collections.defaultdict(collections.Counter)
        self.probe_groups=collections.defaultdict(collections.Counter)
        self.exclusion_cells=collections.Counter()
        self.life=collections.defaultdict(dict);self.expiry=collections.defaultdict(list)
        self.trees={};self.ranks={};self.depletions=collections.defaultdict(list)
        self.contributions=collections.defaultdict(lambda:[0,0])
        self.level_birth=collections.defaultdict(dict)
        self.probes=collections.defaultdict(list);self.closed_probes=[];self.attempted=set()
        self.volume=[];self.broken=set();self.indications=collections.defaultdict(list);self.crosses=collections.Counter()
        self.high=collections.defaultdict(int);self.clock_bad=collections.defaultdict(bool)
        self.exclusions=collections.Counter();self.groups=collections.defaultdict(collections.Counter)
        self.q_start=self.market_end=self.bin_anchor=None;self.last_time={};self.rows=0;self.nonfront=0
        self.counts=collections.Counter()

    def emit(self,name,row):
        self.writers[name].writerow(row)

    def exclude(self,loc,t,reason,count=1):
        self.exclusions[reason]+=count;self.exclusion_cells[(loc,t//BLOCK*BLOCK,reason)]+=count

    def admission(self,r,post=False):
        loc=r['locate']
        if self.clock_bad[loc]:return 'CLOCK'
        active=r['post_active'] if post else r['active']
        if not active:
            trading=r['code'] if post and r['kind']=='H' else r['trading']
            oper=r['code'] if post and r['kind']=='h' and r['flag']=='Q' else r['operational']
            if r['session']!='Q':return 'OUTSIDE_MARKET_HOURS'
            if trading=='?':return 'UNKNOWN_TRADING_STATE'
            if trading!='T':return 'NON_TRADING'
            if oper!='T':return 'OPERATIONAL_HALT'
            return 'CIRCUIT_RECOVERY'
        prefix='post_' if post else ''
        return spread(r[prefix+'bid'],r[prefix+'ask'])

    def life_end(self,loc,ref,time,reason):
        e=self.life[loc].get(ref)
        if e is None:return
        if reason in ('E','C','X','D','U'):
            e['own_endpoint'],e['own_end_ns']=reason,time
            return
        self.life[loc].pop(ref)
        if reason=='SESSION_END' and self.market_end is not None and e['arrival_ns']+SECOND>self.market_end:
            reason='LATE_ENTRY'
        endpoint=e.pop('own_endpoint',reason)
        own_end=e.pop('own_end_ns',time)
        tree,rank=e.pop('tree'),e.pop('rank')
        baseline=e.pop('baseline')
        e.update(end_ns=own_end,elapsed_ns='NA' if reason=='CLOCK_REGRESSION' else max(0,own_end-e['arrival_ns']),endpoint=endpoint,
                 followup=reason,followup_end_ns=time,
                 ahead_execution=tree.prefix(0,rank-1)-baseline[0],
                 ahead_cancellation=tree.prefix(1,rank-1)-baseline[1])
        e['complete_followup']=int(reason=='HORIZON')
        self.emit('lifetimes',e);self.counts['lifetimes']+=1
        g=self.groups[(loc,e['block_ns'],e['spread'],e['ahead_bin'])]
        g['births']+=1;g[endpoint]+=1;g['followup_'+reason]+=1;g['complete_followup']+=e['complete_followup']
        for key in ('own_execution','own_cancellation','ahead_execution','ahead_cancellation'):g[key]+=e[key]
        g['complete_execution_endpoints']+=int(e['complete_followup'] and endpoint in ('E','C'))
        g['partial_execution']+=int(e['partial_execution']);g['partial_cancellation']+=int(e['partial_cancellation'])

    def depletion_end(self,key,e,time,reason,row=None):
        result={k:v for k,v in e.items() if k!='deadline'}
        result.update(end_ns=time,elapsed_ns='NA' if reason=='CLOCK_REGRESSION' else max(0,time-e['start_ns']),endpoint=reason,
                      event_distance=(row['ordinal']-e['ordinal']) if row else 'NA',
                      restored_best=int(row['post_bid' if key[1]=='B' else 'post_ask']==key[2]) if row else 'NA',
                      spread_change=(row['post_ask']-row['post_bid']-e['spread_units']) if row and midpoint(row) is not None and e['spread_units']>0 else 'NA')
        self.emit('depletions',result);self.counts['depletions']+=1
        loc=key[0];block=e['start_ns']//BLOCK*BLOCK
        cell=self.secondary[(loc,block)];group=self.depletion_groups[(loc,block,e['spread'],e['cause'])]
        cell['depletions']+=1;group['depletions']+=1
        category='returned' if reason=='RETURN' else 'no_return' if reason=='NO_RETURN' else 'depletion_censored'
        cell[category]+=1;group['censored' if category=='depletion_censored' else category]+=1
        group['restored_best']+=int(reason=='RETURN' and result['restored_best']==1)

    def close_probe(self,loc,p,time,reason):
        p['stop_ns'],p['stop_reason']=time,reason
        self.closed_probes.append(p);self.probes[loc].remove(p)

    def advance(self,loc,t):
        heap=self.expiry[loc]
        while heap and heap[0][0]<t:
            end,ref=heapq.heappop(heap)
            if ref in self.life[loc]:self.life_end(loc,ref,end,'HORIZON')
        for key,pending in list(self.depletions.items()):
            if key[0]!=loc:continue
            kept=[]
            for e in pending:
                if e['deadline']<t:self.depletion_end(key,e,e['deadline'],'NO_RETURN')
                else:kept.append(e)
            if kept:self.depletions[key]=kept
            else:del self.depletions[key]
        for p in list(self.probes[loc]):
            if p['entry_ns']+2*SECOND<t:self.close_probe(loc,p,p['entry_ns']+2*SECOND,'HORIZON')

    def censor(self,loc,t,reason):
        for ref in list(self.life[loc]):self.life_end(loc,ref,t,reason)
        self.expiry[loc].clear()
        for key in list(self.depletions):
            if key[0]==loc:
                for e in self.depletions.pop(key):self.depletion_end(key,e,t,reason)
        for p in list(self.probes[loc]):self.close_probe(loc,p,t,reason)

    def global_stop(self,loc,t,reason,session_end=False):
        if t<self.high[loc]:
            self.invalidate_level_clock(loc)
            self.exclude(loc,t,'GLOBAL_STOP_CLOCK_INVALID');self.censor(loc,t,'CLOCK_REGRESSION')
        else:
            self.advance(loc,t+int(session_end));self.censor(loc,t,reason)

    def invalidate_level_clock(self,loc):
        for key,(ordinal,t,_) in self.level_birth[loc].items():
            self.level_birth[loc][key]=(ordinal,t,False)

    def on(self,r):
        self.rows+=1
        k,loc,t,n=r['kind'],r['locate'],r['timestamp'],r['ordinal']
        if k=='S':
            if r['code']=='Q':self.q_start=t;self.bin_anchor=t//BIN*BIN
            if r['code']=='M':self.market_end=t
            if r['code'] in 'MEC':
                for _,_,owner in self.panel:
                    self.global_stop(owner,t,'SESSION_END',True)
            return
        if k=='W':
            for _,_,owner in self.panel:
                self.global_stop(owner,t,'CIRCUIT_BREAKER')
            return
        if loc not in {item[2] for item in self.panel}:return
        regressed=t<self.high[loc]
        self.last_time[loc]=t
        if regressed:
            self.invalidate_level_clock(loc)
            self.censor(loc,t,'CLOCK_REGRESSION');self.clock_bad[loc]=True;self.exclude(loc,t,'CLOCK_REGRESSION_EVENTS')
            for cls in list(self.indications):
                if cls[0]==loc:
                    for i in self.indications.pop(cls):
                        self.emit('indications',dict(locate=loc,ordinal=i['ordinal'],cross=i['code'],paired=i['paired'],imbalance=i['imbalance'],direction=i['side'],far=i['far'] or 'NA',near=i['near'] or 'NA',reference=i['reference'] or 'NA',lead_ns='NA',cross_ordinal='NA'))
                        self.counts['clock_invalid_indications']+=1
        else:
            self.high[loc]=t;self.clock_bad[loc]=False;self.advance(loc,t)
        if r['active'] and not r['post_active']:
            self.censor(loc,t,'TRADING_STATE')
        price,side=r['price'],r['side'];key=(loc,side,price)
        rank_info=self.ranks.get(r['ref']) if k in ('E','C','X','D','U') else None
        if k in ('E','C') and r['ahead_orders']:
            self.nonfront+=1
        if k=='C':
            compatible=r['reference']>0 and (r['reference']<=price if side=='B' else r['reference']>=price)
            self.counts['C_limit_compatible' if compatible else 'C_limit_incompatible_or_unavailable']+=1
            if compatible and r['reference']!=price:self.counts['C_price_improved']+=1
        if k in ('E','C','X','D','U'):
            qty=r['order_qty'] if k=='U' else r['quantity']
            if rank_info:
                tree,rank=rank_info;tree.add(0 if k in ('E','C') else 1,rank,qty)
            self.contributions[key][0 if k in ('E','C') else 1]+=qty
            e=self.life[loc].get(r['ref'])
            if e:
                if k in ('E','C'):
                    e['own_execution']+=qty;e['partial_execution']|=qty<r['order_qty']
                else:
                    e['own_cancellation']+=qty;e['partial_cancellation']|=k=='X' and qty<r['order_qty']
                if k in ('D','U') or qty==r['order_qty']:self.life_end(loc,r['ref'],t,k)
            if k in ('D','U') or qty==r['order_qty']:self.ranks.pop(r['ref'],None)
        # Existing shadow entries see the transition before a new entry joins.
        for p in self.probes[loc]:
            event=dict(t=t,n=n,k=k,side=side,price=price,execution_price=r['reference'] if k=='C' else price,qty=r['order_qty'] if k=='U' else r['quantity'],match=r['match'],flag=r['flag'],
                       ahead=bool(rank_info and rank_info[0] is p['tree'] and rank_info[1]<=p['last_rank']),mid=midpoint(r),
                       at_best=r['post_bid' if p['side']=='B' else 'post_ask']==p['price'],
                       level_empty=int(side==p['side'] and price==p['price'] and not r['post_level_qty']))
            p['events'].append(event)
        if k in ('E','C','P','Q'):
            category='E' if k=='E' else ('CY' if r['flag']=='Y' else 'CN') if k=='C' else k
            self.volume.append((r['match'],loc,t//BLOCK*BLOCK,spread(r['bid'],r['ask']),r['active'],r['session'],r['trading'],r['operational'],category,r['quantity'],r['bid_qty'],r['ask_qty']))
        if k=='B':self.broken.add(r['match'])
        if k=='I' and not self.clock_bad[loc]:self.indications[(loc,r['code'])].append(dict(r))
        if k=='Q':
            cls=(loc,r['code']);indications=self.indications.pop(cls,[])
            last=indications[-1] if indications and not self.clock_bad[loc] else None;self.crosses[cls]+=1
            pair=dict(locate=loc,cross=r['code'],ordinal=n,timestamp=t,shares=r['quantity'],price=price if price else 'NA',
                      indications=len(indications),episode=self.crosses[cls],last_ordinal=last['ordinal'] if last else 'NA',
                      staleness_ns=t-last['timestamp'] if last else 'NA',paired=last['paired'] if last else 'NA',
                      imbalance=last['imbalance'] if last else 'NA',direction=last['side'] if last else 'NA')
            for field in ('far','near','reference'):
                pair[field]=last[field] if last and last[field] else 'NA'
                pair[field+'_difference']=price-last[field] if price and last and last[field] else 'NA'
            self.emit('auction_pairs',pair)
            for i in indications:self.emit('indications',dict(locate=loc,ordinal=i['ordinal'],cross=i['code'],paired=i['paired'],imbalance=i['imbalance'],direction=i['side'],far=i['far'] or 'NA',near=i['near'] or 'NA',reference=i['reference'] or 'NA',lead_ns=t-i['timestamp'],cross_ordinal=n))
        emptied=k in ('E','C','X','D','U') and r['level_qty'] and not r['post_level_qty']
        origin=self.level_birth[loc].pop((side,price),None) if emptied else None
        # Depletion requires a previously best named price to become empty.
        if emptied and price==r['bid' if side=='B' else 'ask']:
            execution,cancel=self.contributions.pop(key,[0,0])
            if r['active'] and not self.clock_bad[loc] and spread(r['bid'],r['ask']) not in ('EMPTY','LOCKED_CROSSED'):
                self.depletions[key].append(dict(locate=loc,side=side,price=price,ordinal=n,start_ns=t,deadline=t+SECOND,
                    level_birth_ordinal=origin[0] if origin else 'NA',level_birth_ns=origin[1] if origin else 'NA',
                    level_age_ns=t-origin[1] if origin and origin[2] and t>=origin[1] else 'NA',cause=k,
                    spread=spread(r['bid'],r['ask']),spread_units=r['ask']-r['bid'],execution_reduction=execution,cancellation_reduction=cancel))
            else:
                reason=self.admission(r)
                self.exclude(loc,t,'DEPLETION_'+reason)
        if k in ('A','F','U'):
            birth_ref=r['new_ref'] if k=='U' else r['ref'];birth_price=r['new_price'] if k=='U' else price
            birth_key=(loc,side,birth_price)
            # Same-price replacement of the last order is one atomic report:
            # a positive pre/post level remains the same displayed spell.
            new_level=(side,birth_price) not in self.level_birth[loc]
            self.level_birth[loc].setdefault((side,birth_price),(n,t,not self.clock_bad[loc]))
            tree=self.trees.setdefault(birth_key,Prefix());rank=tree.append();self.ranks[birth_ref]=(tree,rank)
            for e in self.depletions.pop(birth_key,[]):self.depletion_end(birth_key,e,t,'RETURN',r)
            if new_level:self.contributions[birth_key]=[0,0]
            reason=self.admission(r)
            if reason in ('SUBCENT','ONE_CENT','WIDER'):
                qa=r['birth_ahead'];ahead_bin='0' if qa==0 else '1_100' if qa<=100 else '101_1000' if qa<=1000 else 'GT1000'
                e=dict(locate=loc,ref=birth_ref,birth_ordinal=n,arrival_ns=t,side=side,price=birth_price,quantity=r['quantity'],
                       initial_ahead=qa,initial_orders_ahead=r['birth_orders'],ahead_bin=ahead_bin,spread=reason,
                       best_depth=r['bid_qty'] if side=='B' else r['ask_qty'],block_ns=t//BLOCK*BLOCK,
                       own_execution=0,own_cancellation=0,partial_execution=False,partial_cancellation=False,
                       tree=tree,rank=rank,baseline=(tree.prefix(0,rank-1),tree.prefix(1,rank-1)))
                self.life[loc][birth_ref]=e;heapq.heappush(self.expiry[loc],(t+SECOND,birth_ref))
            else:self.exclude(loc,t,'BIRTH_'+reason)
        if self.q_start is not None and r['session']=='Q' and t>=self.bin_anchor:
            bin_no=(t-self.bin_anchor)//BIN
            attempt=(loc,bin_no)
            if attempt not in self.attempted and (t-self.bin_anchor)%BIN<SECOND:
                self.attempted.add(attempt);self.secondary[(loc,t//BLOCK*BLOCK)]['probe_attempts']+=2
                reason=self.admission(r,True)
                if reason not in ('ONE_CENT','SUBCENT','WIDER'):
                    self.exclude(loc,t,'PROBE_'+reason,2);self.secondary[(loc,t//BLOCK*BLOCK)]['probe_excluded']+=2
                else:
                    for s in ('B','S'):
                        pprice=r['post_bid' if s=='B' else 'post_ask'];pkey=(loc,s,pprice)
                        tree=self.trees.setdefault(pkey,Prefix())
                        self.probes[loc].append(dict(locate=loc,bin=bin_no,entry_ordinal=n,entry_ns=t,side=s,price=pprice,
                            initial_ahead=r['post_bid_qty' if s=='B' else 'post_ask_qty'],entry_mid=midpoint(r),spread=reason,
                            tree=tree,last_rank=len(tree.trees[0])-1,events=[]))

    def evaluate_probe(self,p,trace=None):
        result={k:v for k,v in p.items() if k not in ('tree','last_rank','events')}
        deadline=p['entry_ns']+SECOND
        complete=p['stop_ns']>deadline or (p['stop_ns']==deadline and p['stop_reason'] in ('HORIZON','SESSION_END'))
        depletion=next((e for e in p['events'] if e['level_empty'] and e['t']<=deadline),None)
        result['depleted_within_horizon']=int(depletion is not None)
        result['time_to_level_depletion_ns']=depletion['t']-p['entry_ns'] if depletion else 'NA'
        result['depletion_followup']='DEPLETED' if depletion else 'COMPLETE_NO_DEPLETION' if complete else p['stop_reason']
        result['depletion_observation_ns']=depletion['t']-p['entry_ns'] if depletion else ('NA' if p['stop_reason']=='CLOCK_REGRESSION' else min(p['stop_ns'],deadline)-p['entry_ns'])
        lost_best=next((e for e in p['events'] if not e['at_best'] and e['t']<=deadline),None)
        result['lost_best_within_horizon']=int(lost_best is not None)
        result['time_to_lost_best_ns']=lost_best['t']-p['entry_ns'] if lost_best else 'NA'
        for label,optimistic in (('pessimistic',False),('optimistic',True)):
            ahead=p['initial_ahead'];opportunity=None;front=None;used=set()
            for e in p['events']:
                if e['t']>p['entry_ns']+SECOND:break
                if e['side']!=p['side'] or e['price']!=p['price']:continue
                execution=e['k'] in ('E','C')
                # C reports execution price separately from the resting level.
                # Service at a worse price cannot execute the shadow limit.
                compatible=e['execution_price']>0 and (e['execution_price']<=p['price'] if p['side']=='B' else e['execution_price']>=p['price'])
                eligible=execution and compatible and e['flag']=='Y' and e['match'] not in self.broken and e['match'] not in used
                if eligible:
                    used.add(e['match']);qty=e['qty'];left=max(0,qty-ahead);ahead=max(0,ahead-qty)
                    if left and opportunity is None:opportunity=e
                elif e['ahead'] and (execution or (optimistic and e['k'] in ('X','D','U'))):
                    ahead=max(0,ahead-e['qty'])
                if not ahead and front is None:front=e['t']
                if trace is not None:
                    trace.append(dict(mode=label,ordinal=e['n'],ahead=ahead,opportunity=opportunity is not None))
            result[label+'_opportunity']=int(opportunity is not None)
            result[label+'_followup']='COMPLETE' if complete else p['stop_reason']
            result[label+'_time_to_front_ns']=front-p['entry_ns'] if front is not None else 'NA'
            result[label+'_time_to_opportunity_ns']=opportunity['t']-p['entry_ns'] if opportunity else 'NA'
            adverse='NA';adverse_reason='NO_OPPORTUNITY'
            if opportunity:
                target=opportunity['t']+SECOND
                if p['stop_ns']<target or (p['stop_ns']==target and p['stop_reason']!='HORIZON'):adverse_reason=p['stop_reason']
                else:
                    mid=p['entry_mid']
                    for e in p['events']:
                        if e['t']>target:break
                        mid=e['mid']
                    if mid is None or opportunity['mid'] is None:adverse_reason='QUOTE_UNAVAILABLE'
                    else:
                        adverse=(opportunity['mid']-mid)*(1 if p['side']=='B' else -1);adverse_reason='OBSERVED'
            result[label+'_adverse_half_units']=adverse;result[label+'_adverse_reason']=adverse_reason
            self.counts[label+'_opportunities']+=int(opportunity is not None)
        self.emit('opportunities',result);self.counts['probes']+=1
        loc,block=p['locate'],p['entry_ns']//BLOCK*BLOCK
        for group in (self.secondary[(loc,block)],self.probe_groups[(loc,block,p['spread'],p['side'])]):
            group['probes']+=1
            group['complete_probes' if result['optimistic_followup']=='COMPLETE' else 'censored_probes']+=1
            group['probe_level_depleted' if depletion else 'probe_level_no_depletion' if complete else 'probe_level_censored']+=1
            for label in ('pessimistic','optimistic'):group[label+'_opportunities']+=result[label+'_opportunity']
            group['assumption_sensitive_probes']+=int(result['optimistic_opportunity']!=result['pessimistic_opportunity'])
        self.counts['assumption_sensitive_probes']+=int(result['optimistic_opportunity']!=result['pessimistic_opportunity'])

    def finish(self):
        if self.q_start is None or self.market_end is None or self.market_end<=self.q_start:
            raise ValueError('research requires a positive observed market-hours clock interval')
        for _,_,loc in self.panel:
            t=self.market_end if self.market_end is not None else self.last_time.get(loc,0)
            self.advance(loc,t);self.censor(loc,t,'OBSERVATION_END')
        for p in self.closed_probes:self.evaluate_probe(p)
        volume=collections.defaultdict(collections.Counter)
        match_sets=collections.defaultdict(set)
        for match,loc,block,regime,active,session,trading,oper,kind,qty,bd,ad in self.volume:
            depth=bd+ad;depth_bin='0' if depth==0 else '1_200' if depth<=200 else '201_2000' if depth<=2000 else 'GT2000'
            key=(loc,block,regime,depth_bin,active,session,trading,oper)
            g=volume[key];g['records']+=1;match_sets[key].add(match)
            if match in self.broken:g['corrected_'+kind]+=qty;g['broken_records']+=1
            else:g[kind]+=qty
            if match not in self.broken:
                g['unbroken_reported_shares']+=qty
                g['bid_depth_share_sum']+=bd*qty;g['ask_depth_share_sum']+=ad*qty
        for (loc,block,regime,depth_bin,active,session,trading,oper),values in sorted(volume.items()):
            e,cy,p=values['E'],values['CY'],values['P'];den=e+cy+p
            row=dict(locate=loc,block_ns=block,spread=regime,depth_bin=depth_bin,active=active,session=session,trading=trading,operational=oper,distinct_matches=len(match_sets[(loc,block,regime,depth_bin,active,session,trading,oper)]))
            row.update({k:values[k] for k in ('E','CY','P','Q','CN','records','broken_records','corrected_E','corrected_CY','corrected_P','corrected_Q','corrected_CN','unbroken_reported_shares','bid_depth_share_sum','ask_depth_share_sum')})
            row.update(eligible_shares=den,p_fraction=p/den if den else 'NA');self.emit('context_volume',row)
        for (loc,cls),indications in sorted(self.indications.items()):
            self.counts['unmatched_indications']+=len(indications)
            for i in indications:self.emit('indications',dict(locate=loc,ordinal=i['ordinal'],cross=cls,paired=i['paired'],imbalance=i['imbalance'],direction=i['side'],far=i['far'] or 'NA',near=i['near'] or 'NA',reference=i['reference'] or 'NA',lead_ns='NA',cross_ordinal='NA'))
        bin_count=max(0,(self.market_end-self.bin_anchor+BIN-1)//BIN) if self.market_end is not None and self.q_start is not None else 0
        for _,_,loc in self.panel:
            for bin_no in range(bin_count):
                if (loc,bin_no) not in self.attempted:
                    t=self.bin_anchor+bin_no*BIN
                    self.exclude(loc,t,'PROBE_NO_EVENT_FIRST_SECOND',2);self.secondary[(loc,t//BLOCK*BLOCK)]['probe_no_event']+=2
        blocks=range(self.q_start//BLOCK*BLOCK,(self.market_end//BLOCK+1)*BLOCK,BLOCK) if self.q_start is not None and self.market_end is not None else []
        # Include every symbol and time block, including empty cells.
        for _,name,loc in self.panel:
            for block in blocks:
                aggregate=collections.Counter()
                for (owner,b,_,_),values in self.groups.items():
                    if owner==loc and b==block:aggregate.update(values)
                row=dict(locate=loc,stock=name.decode().rstrip(' '),block_ns=block)
                row.update({k:aggregate[k] for k in ('births','complete_followup','complete_execution_endpoints','followup_LATE_ENTRY','followup_CIRCUIT_BREAKER','followup_TRADING_STATE','followup_CLOCK_REGRESSION','E','C','X','D','U','HORIZON','TRADING_STATE','SESSION_END','CLOCK_REGRESSION','OBSERVATION_END','own_execution','own_cancellation','ahead_execution','ahead_cancellation','partial_execution','partial_cancellation')})
                den=row['complete_followup'];row['execution_endpoint_fraction']=row['complete_execution_endpoints']/den if den else 'NA';self.emit('sensitivity',row)
                other=dict(locate=loc,stock=name.decode().rstrip(' '),block_ns=block)
                other.update({k:self.secondary[(loc,block)][k] for k in SCHEMAS['study_sensitivity'].split()[3:]})
                self.emit('study_sensitivity',other)
        for (loc,block,regime,ahead),values in sorted(self.groups.items()):
            row=dict(locate=loc,block_ns=block,spread=regime,ahead_bin=ahead)
            row.update({k:values[k] for k in ('births','complete_followup','complete_execution_endpoints','followup_LATE_ENTRY','followup_CIRCUIT_BREAKER','followup_TRADING_STATE','followup_CLOCK_REGRESSION','E','C','X','D','U','HORIZON','own_execution','own_cancellation','ahead_execution','ahead_cancellation')});self.emit('lifetime_groups',row)
        for (loc,block,regime,cause),group in sorted(self.depletion_groups.items()):
            row=dict(locate=loc,block_ns=block,spread=regime,cause=cause)
            row.update({k:group[k] for k in SCHEMAS['depletion_groups'].split()[4:]});self.emit('depletion_groups',row)
        for (loc,block,regime,side),group in sorted(self.probe_groups.items()):
            row=dict(locate=loc,block_ns=block,spread=regime,side=side)
            row.update({k:group[k] for k in SCHEMAS['probe_groups'].split()[4:]});self.emit('probe_groups',row)
        for (loc,block,reason),count in sorted(self.exclusion_cells.items()):self.emit('exclusions',dict(locate=loc,block_ns=block,reason=reason,count=count))
        for f in self.files.values():f.close()
        identities={name+'.csv':hashlib.sha256((self.root/(name+'.csv')).read_bytes()).hexdigest() for name in sorted(self.files)}
        result=dict(status='SUCCESS',horizon_ns=SECOND,unit='finite-input displayed reference and fixed-entry service scenario',rows=self.rows,counts=dict(self.counts),exclusions=dict(self.exclusions),nonfront_execution_events=self.nonfront,market_start_ns=self.q_start,bin_anchor_ns=self.bin_anchor,market_end_ns=self.market_end,table_sha256=identities,
                    interpretation='descriptive census; service-scenario outcomes are not actual exchange-fill bounds; capture continuity unverified')
        (self.root/'summary.json').write_text(json.dumps(result,indent=2,sort_keys=True)+'\n');return result


def analyze(rows,panel,destination,validate=None):
    destination=Path(destination)
    if destination.exists():raise ValueError('research output already exists')
    destination.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=destination.name+'.tmp.',dir=destination.parent) as tmp:
        study=Study(panel,tmp)
        for row in rows:study.on(row)
        result=study.finish()
        if validate:validate()
        os.rename(tmp,destination)
    return result


def main():
    p=argparse.ArgumentParser(description=__doc__)
    group=p.add_mutually_exclusive_group(required=True);group.add_argument('--observations');group.add_argument('--raw')
    p.add_argument('--out',required=True);p.add_argument('--accounting',required=True);p.add_argument('--accounting-check',required=True);p.add_argument('--observations-check');args=p.parse_args()
    accounting=json.loads((Path(args.accounting)/'summary.json').read_text())
    independent=json.loads(Path(args.accounting_check).read_text())
    if accounting['status']!='SUCCESS' or independent['status']!='PASS' or accounting['input_sha256']!=independent['input_sha256']:
        raise ValueError('same-input all-market accounting gate is missing or invalid')
    if args.raw:
        observations=Observations(args.raw);rows=observations;panel=observations.panel
        if args.observations_check:
            def checked_rows():
                with (Path(args.observations_check)/'events.csv').open(newline='') as source:
                    supplied=iter(csv.DictReader(source))
                    for row in observations:
                        candidate=next(supplied,None)
                        if candidate is None or typed(candidate)!=row:raise ValueError('raw before/after state disagreement at '+str(row['ordinal']))
                        yield row
                    if next(supplied,None) is not None:raise ValueError('extra supplied observation rows')
            rows=checked_rows()
        def validate():
            if observations.reader.sha.hexdigest()!=accounting['input_sha256'] or observations.reader.bytes!=accounting['input_bytes']:
                raise ValueError('raw research input differs from verified accounting')
    else:
        with (Path(args.observations)/'panel.csv').open(newline='') as f:
            panel=[(r['selection_sha256'],r['stock'].ljust(8).encode(),int(r['locate'])) for r in csv.DictReader(f)]
        observed=json.loads((Path(args.observations)/'summary.json').read_text())
        if observed['input_sha256']!=accounting['input_sha256'] or observed['input_bytes']!=accounting['input_bytes']:
            raise ValueError('observations differ from verified accounting input')
        source=(Path(args.observations)/'events.csv').open(newline='');rows=map(typed,csv.DictReader(source))
        validate=None
    print(json.dumps(analyze(rows,panel,args.out,validate),sort_keys=True))
if __name__=='__main__':main()
