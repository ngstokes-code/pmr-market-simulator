#!/usr/bin/env python3
"""Export the original demo as checked snapshots; the browser only renders them."""
import argparse
import copy
import csv
import hashlib
import json
from pathlib import Path
import struct
import tempfile

from check_observations import Observations, check, typed
from make_demo import rows as demo_rows
from research import Study

ROOT = Path(__file__).resolve().parents[1]
NOTES = [
    ('Before the session', 'Step forward to build the displayed book.', 'No messages yet'),
    ('Session opens', 'The start-of-messages marker does not create orders.', 'Session control'),
    ('ONE is registered', 'Directory identity precedes its order messages.', 'Directory → symbol identity'),
    ('System hours begin', 'The book is still empty.', 'Session control'),
    ('Trading state: active', 'Trading state and order state are separate.', 'Trading-state update'),
    ('100 shares join the bid', 'Reference #1 rests at $10.00.', 'Add → displayed book'),
    ('100 shares join the ask', 'Reference #2 rests at $10.01.', 'Add → displayed book'),
    ('Opening auction indication', 'NOII describes the auction, not a new continuous-book order.', 'Auction indication'),
    ('Market hours begin', 'The continuous displayed book remains unchanged.', 'Session control'),
    ('The one-share scenario starts', 'An opening cross prints separately. A hypothetical bid joins behind 100 displayed shares at $10.00.', 'Auction print · scenario entry'),
    ('20 shares join behind', 'Reference #3 joins the bid tail. It does not increase the shares ahead of the hypothetical entry.', 'Add → displayed book'),
    ('90 shares ahead cancel', 'Reference #1 falls from 100 to 10 shares. Only the optimistic scenario removes the withdrawal from shares ahead.', 'Displayed order → cancellation'),
    ('10 shares execute', 'Reference #1 leaves. Optimistic shares ahead reach zero, but no excess execution service remains yet.', 'Displayed order → execution print'),
    ('The bid price empties', 'Reference #3 executes 20. The optimistic scenario has a one-share service opportunity; the pessimistic scenario still has 70 shares ahead.', 'Displayed order → execution print'),
    ('The bid replenishes', 'Reference #4 restores five displayed shares at $10.00, 100 ms after the price emptied.', 'Add → displayed book'),
    ('A non-displayed trade prints', 'Seven P-message shares appear on the tape. No displayed order is created or reduced.', 'Trade report → tape only'),
    ('One displayed share executes', 'Reference #4 now has four shares. The one-second scenario horizon has ended; this later report does not extend it.', 'Displayed order → execution print'),
    ('Trading state: halted', 'The reported halt does not cancel the remaining displayed orders.', 'Trading-state update'),
    ('The seven-share print is broken', 'Match #102 is corrected out of eligible reported volume. The displayed book stays unchanged.', 'Correction → accounting only'),
    ('Market hours end', 'Session state changes; remaining displayed orders are not silently removed.', 'Session control'),
    ('Closing auction indication', 'Auction state stays separate from the continuous book.', 'Auction indication'),
    ('Closing cross prints', 'The 80-share cross is kept separate from eligible E / printable C / P volume.', 'Auction report → tape'),
    ('System hours end', 'The replay retains its last reconstructed displayed state.', 'Session control'),
    ('Session complete', 'The complete session has passed its protocol and independent-state checks.', 'End of messages'),
]


class TracedStudy(Study):
    def __init__(self, panel, root):
        super().__init__(panel, root)
        self.entry = None
        self.trace = []

    def evaluate_probe(self, probe, trace=None):
        if probe['side'] == 'B' and probe['bin'] == 0:
            self.entry = {k: probe[k] for k in ('entry_ordinal', 'entry_ns', 'initial_ahead', 'price')}
            super().evaluate_probe(probe, self.trace)
        else:
            super().evaluate_probe(probe, trace)


def load_csv(path):
    with Path(path).open(newline='') as stream:
        return list(csv.DictReader(stream))


def quote(orders):
    bid = max((o['price'] for o in orders.values() if o['side'] == 'B'), default=0)
    ask = min((o['price'] for o in orders.values() if o['side'] == 'S'), default=0)
    return [bid, ask, sum(o['shares'] for o in orders.values() if o['side'] == 'B' and o['price'] == bid),
            sum(o['shares'] for o in orders.values() if o['side'] == 'S' and o['price'] == ask)]


def build_payload(source, observations, research, source_id):
    source, observations, research = map(Path, (source, observations, research))
    expected = b''.join(struct.pack('>H', len(row)) + row for row in demo_rows())
    if source.read_bytes() != expected:
        raise ValueError('visualizer supports only the original, exact demo input')
    with tempfile.TemporaryDirectory(prefix='msim-replay-export-') as temporary:
        check(source, observations, Path(temporary) / 'independent.json')
        supplied = [typed(row) for row in load_csv(observations / 'events.csv')]
        raw = Observations(source)
        study = TracedStudy(raw.panel, temporary)
        for row in supplied:
            study.on(row)
        summary = study.finish()
        # A presentation trace must not change any retained analytical output.
        if summary != json.loads((research / 'summary.json').read_text()):
            raise ValueError('traced research differs from the checked demo')
        for name, digest in summary['table_sha256'].items():
            if hashlib.sha256((research / name).read_bytes()).hexdigest() != digest:
                raise ValueError('research table identity differs: ' + name)
    if [r['ordinal'] for r in supplied] != list(range(1, 24)) or study.entry is None:
        raise ValueError('incomplete demo observation sequence')
    orders, tape, frames = {}, [], []

    def frame(row):
        ordinal = row['ordinal'] if row else 0
        shadow = None
        if row and ordinal >= study.entry['entry_ordinal']:
            shadow = {'complete': row['timestamp'] > study.entry['entry_ns'] + 10**9}
            for mode in ('optimistic', 'pessimistic'):
                values = [e for e in study.trace if e['mode'] == mode and e['ordinal'] <= ordinal]
                shadow[mode] = values[-1] if values else dict(ahead=study.entry['initial_ahead'], opportunity=False)
        eligible = sum(p['shares'] for p in tape if p['kind'] in ('E', 'P') and not p['broken'])
        crosses = sum(p['shares'] for p in tape if p['kind'] == 'Q' and not p['broken'])
        title, note, flow = NOTES[ordinal]
        return dict(ordinal=ordinal, timestamp=row['timestamp'] if row else 0, kind=row['kind'] if row else '-',
                    code=row['code'] if row else '-', ref=str(row['ref']) if row and row['ref'] else None,
                    quantity=row['quantity'] if row else 0, price=row['price'] if row else 0,
                    active=bool(row and row['post_active']), title=title, note=note, flow=flow,
                    orders=copy.deepcopy(list(orders.values())), tape=copy.deepcopy(tape), shadow=copy.deepcopy(shadow),
                    eligible_shares=eligible, cross_shares=crosses)

    frames.append(frame(None))
    for row in supplied:
        if row['locate'] and quote(orders) != [row[k] for k in ('bid', 'ask', 'bid_qty', 'ask_qty')]:
            raise ValueError('display projection disagrees before event ' + str(row['ordinal']))
        kind, ref = row['kind'], str(row['ref'])
        if kind == 'A':
            orders[ref] = dict(ref=ref, side=row['side'], price=row['price'], shares=row['quantity'])
        elif kind in ('E', 'X'):
            if ref not in orders or orders[ref]['shares'] != row['order_qty']:
                raise ValueError('order projection disagrees')
            # Derive the changed order from C++ before/after level projections,
            # not a second implementation of reduction or matching policy.
            remaining = row['post_level_qty'] - (row['level_qty'] - row['order_qty'])
            if not 0 <= remaining < row['order_qty']:
                raise ValueError('invalid projected remainder')
            if remaining:
                orders[ref]['shares'] = remaining
            else:
                del orders[ref]
        if kind in ('E', 'P', 'Q'):
            tape.append(dict(match=str(row['match']), ordinal=row['ordinal'], timestamp=row['timestamp'],
                             price=row['price'], shares=row['quantity'], kind=kind, broken=False,
                             detail='Resting bid' if kind == 'E' else 'Non-displayed' if kind == 'P' else 'Opening cross' if row['code'] == 'O' else 'Closing cross'))
        elif kind == 'B':
            target = next((p for p in tape if p['match'] == str(row['match'])), None)
            if target is None:
                raise ValueError('broken match is missing from tape')
            target['broken'] = True
        if row['locate'] and quote(orders) != [row[k] for k in ('post_bid', 'post_ask', 'post_bid_qty', 'post_ask_qty')]:
            raise ValueError('display projection disagrees after event ' + str(row['ordinal']))
        frames.append(frame(row))
    if (frames[-1]['eligible_shares'], frames[-1]['cross_shares']) != (31, 160):
        raise ValueError('demo tape accounting disagrees with its fixed hand ledger')
    return dict(schema=1, symbol='ONE', input_bytes=len(expected), input_sha256=hashlib.sha256(expected).hexdigest(),
                source_id=source_id, initial_frame=study.entry['entry_ordinal'], scenario_price=study.entry['price'],
                frames=frames)


def encode(payload):
    return '// Generated by scripts/render_replay.py from the checked original demo.\nwindow.MSIM_REPLAY = ' + json.dumps(payload, sort_keys=True, separators=(',', ':')) + ';\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--demo', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    import run
    receipt = json.loads((args.demo / 'run.json').read_text())
    if receipt['status'] != 'PASS' or not receipt['source_unchanged'] or receipt['source_id'] != run.source_manifest()[0]:
        raise ValueError('a successful same-source demo receipt is required')
    payload = build_payload(args.demo / 'demo.itch', args.demo / 'observations', args.demo / 'research', receipt['source_id'])
    text = encode(payload)
    if args.check:
        if args.out.read_text() != text:
            raise ValueError('published replay data differs from the checked demo')
    else:
        args.out.write_text(text)
    print('Original demo: 23 checked transitions, research trace and tape ledger agree')


if __name__ == '__main__':
    main()
