#!/usr/bin/env python3
"""Export a verified, local-only historical excerpt; raw data stays outside Git."""
import argparse
from collections import OrderedDict, Counter
import csv
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

from itch_reference import State, LENGTHS, number
import run

MASK = (1 << 64) - 1


def mix(h, value):
    x = h ^ value
    x = ((x ^ (x >> 30)) * 0xbf58476d1ce4e5b9) & MASK
    x = ((x ^ (x >> 27)) * 0x94d049bb133111eb) & MASK
    return x ^ (x >> 31)


def read_csv(path):
    with Path(path).open(newline='') as stream:
        return list(csv.DictReader(stream))


class Book:
    """Independent ordered dictionaries, reconstructed directly from wire bytes."""
    def __init__(self):
        self.orders = {}
        self.levels = {'B': {}, 'S': {}}

    def add(self, ref, side, price, qty):
        if ref in self.orders or not qty:
            raise ValueError('invalid independent birth')
        self.orders[ref] = [side, price, qty]
        self.levels[side].setdefault(price, OrderedDict())[ref] = qty

    def reduce(self, ref, qty):
        side, price, own = self.orders[ref]
        if not 0 < qty <= own:
            raise ValueError('invalid independent reduction')
        queue = self.levels[side][price]
        if qty == own:
            del queue[ref], self.orders[ref]
            if not queue:
                del self.levels[side][price]
        else:
            queue[ref] -= qty
            self.orders[ref][2] -= qty

    def snapshot(self):
        return [[str(ref), side, price, qty] for side in 'BS'
                for price in sorted(self.levels[side], reverse=side == 'B')
                for ref, qty in self.levels[side][price].items()]

    def digest(self, locate):
        h = 0x9e3779b97f4a7c15
        for side in 'BS':
            if self.levels[side]:
                h = mix(h, locate * 2 + (side == 'S'))
            for price in sorted(self.levels[side], reverse=side == 'B'):
                for ref, qty in self.levels[side][price].items():
                    h = mix(mix(mix(h, price), ref), qty)
        return str(h)


def selected_frames(source, locate):
    # Scan framing from byte zero, not from an arbitrary timestamp/offset. Only
    # selected payloads enter Python models; all lengths are checked on the way.
    offset = ordinal = 0
    with Path(source).open('rb') as stream:
        carry = b''
        while block := stream.read(8 * 1024 * 1024):
            data, at = carry + block, 0
            while at + 2 <= len(data):
                length = struct.unpack_from('>H', data, at)[0]
                if length < 11:
                    raise ValueError('short raw frame')
                if at + 2 + length > len(data):
                    break
                kind = chr(data[at + 2])
                if LENGTHS.get(kind) != length:
                    raise ValueError('unsupported raw frame')
                loc = struct.unpack_from('>H', data, at + 3)[0]
                ordinal += 1
                if loc == locate or loc == 0:
                    yield ordinal, offset, data[at + 2:at + 2 + length]
                at += length + 2
                offset += length + 2
            carry = data[at:]
        if carry:
            raise ValueError('truncated raw frame')


def verify(source, report):
    report, source = Path(report), Path(source)
    summary = json.loads((report / 'summary.json').read_text())
    if summary.get('status') != 'SUCCESS' or summary.get('schema') != 1:
        raise ValueError('unsuccessful tape report')
    original = source.stat()
    identity = (original.st_dev, original.st_ino, original.st_size, original.st_mtime_ns, original.st_ctime_ns)
    if original.st_size != summary['input_bytes'] or run.sha(source) != summary['input_sha256']:
        raise ValueError('tape input identity differs')
    start, end, locate = (summary[k] for k in ('start_ns', 'end_ns', 'locate'))
    if not 0 <= start < end <= 86400 * 10**9 or end - start > 60 * 10**9:
        raise ValueError('invalid tape bounds')
    book, protocol = Book(), State()
    events, initial, high, counts = [], None, start, Counter()
    supplied = read_csv(report / 'events.csv')
    if len(supplied) > 200000:
        raise ValueError('tape event bound exceeded')
    bracketed = False
    regressions = 0
    for ordinal, offset, f in selected_frames(source, locate):
        k, loc, ts = chr(f[0]), number(f, 1, 2), number(f, 5, 6)
        if loc == locate and ts >= start and initial is None:
            initial = book.snapshot()
            supplied_initial = [[r['ref'], r['side'], int(r['price']), int(r['quantity'])]
                                for r in read_csv(report / 'initial.csv')]
            if supplied_initial != initial:
                raise ValueError('initial complete FIFO state disagrees')
        if loc == locate and ts >= end:
            bracketed = True
            break
        protocol.accept(ordinal, f)
        if not loc:
            continue
        row = dict(ordinal=str(ordinal), offset=str(offset), timestamp=str(ts), kind=k,
                   ref='0', new_ref='0', side='-', price='0', new_price='0', quantity='0',
                   match='0', flag='-', execution_price='0', code='-')
        if k in 'AF':
            ref, side, qty, price = number(f, 11, 8), chr(f[19]), number(f, 20, 4), number(f, 32, 4)
            row.update(ref=str(ref), side=side, price=str(price), quantity=str(qty))
            book.add(ref, side, price, qty)
        elif k in 'ECXDU':
            ref = number(f, 11, 8)
            side, price, own = book.orders[ref]
            qty = own if k == 'D' else number(f, 27 if k == 'U' else 19, 4)
            row.update(ref=str(ref), side=side, price=str(price), quantity=str(qty))
            book.reduce(ref, own if k == 'U' else qty)
            if k == 'U':
                new, new_price = number(f, 19, 8), number(f, 31, 4)
                row.update(new_ref=str(new), new_price=str(new_price))
                book.add(new, side, new_price, qty)
            if k in 'EC':
                row.update(match=str(number(f, 23, 8)), flag='Y' if k == 'E' else chr(f[31]))
                if k == 'C':
                    row['execution_price'] = str(number(f, 32, 4))
        elif k in 'PQ':
            row.update(quantity=str(number(f, 20 if k == 'P' else 11, 4 if k == 'P' else 8)),
                       price=str(number(f, 32 if k == 'P' else 27, 4)),
                       match=str(number(f, 36 if k == 'P' else 31, 8)))
            if k == 'Q':
                row['code'] = chr(f[39])
        elif k == 'B':
            row['match'] = str(number(f, 11, 8))
        elif k == 'H':
            row['code'] = chr(f[19])
        elif k == 'h':
            row.update(flag=chr(f[19]), code=chr(f[20]))
        elif k == 'I':
            # The observer's shared Row also carries indication fields; only
            # these retained columns are checked/exported, not an auction book.
            row.update(side=chr(f[27]), execution_price=str(number(f, 44, 4)),
                       code=chr(f[48]), flag=chr(f[49]).replace(' ', '_'))
        if initial is None:
            continue
        row['digest'] = book.digest(locate)
        index = len(events)
        if index >= len(supplied) or row != supplied[index]:
            raise ValueError(f'tape event/state disagreement at {ordinal}: {row} != {supplied[index:index+1]}')
        regressions += ts < high
        high = max(high, ts)
        updates, removes = [], []
        if k in 'AF':
            updates = [[row['ref'], side, price, qty]]
        elif k in 'ECXD':
            removes = [row['ref']] if ref not in book.orders else []
            updates = [[row['ref'], *book.orders[ref]]] if not removes else []
        elif k == 'U':
            removes, updates = [row['ref']], [[row['new_ref'], side, new_price, qty]]
        # Nanoseconds since midnight fit JS's integer domain; identities do not.
        event = dict(t=high-start, raw_ns=ts, n=ordinal, k=k, updates=updates, removes=removes,
                     side=row['side'], price=int(row['price']), qty=int(row['quantity']),
                     match=row['match'], printable=row['flag'], execution_price=int(row['execution_price']), code=row['code'])
        events.append(event)
        counts[k] += 1
    if not bracketed or initial is None or not events or len(events) != len(supplied) or len(events) != summary['events'] or len(initial) != summary['initial_orders']:
        raise ValueError('incomplete or extra tape data')
    if protocol.names.get(locate, b'').decode().rstrip() != summary['stock']:
        raise ValueError('tape symbol identity differs')
    now = source.stat()
    if identity != (now.st_dev, now.st_ino, now.st_size, now.st_mtime_ns, now.st_ctime_ns):
        raise ValueError('tape input changed during independent verification')
    return dict(schema=1, verified=True, stock=summary['stock'], start_ns=start, end_ns=end,
                input_sha256=summary['input_sha256'], input_bytes=summary['input_bytes'], input_messages=summary['messages'],
                initial=initial, events=events, counts=dict(counts), clock_regressions=regressions,
                capture_continuity='UNVERIFIED', verification='Raw-source prefix, exact fields and every ordered-state digest; full-input strict C++ replay')


def write_view(root, payload):
    (root/'tape.json').write_text(json.dumps(payload, separators=(',', ':'))+'\n')
    assets = run.ROOT/'docs/replay'
    for name in ('tape.css', 'tape.js'):
        shutil.copy2(assets/name, root/name)
    html = (assets/'tape.html').read_text().replace('<!-- LOCAL_DATA -->', '<script id="tape-data" type="application/json">'+json.dumps(payload, separators=(',', ':')).replace('<', '\\u003c')+'</script>')
    (root/'index.html').write_text(html)


def export(args):
    if not 1 <= len(args.date_label) <= 64 or not args.date_label.isprintable():
        raise ValueError('date label must be 1..64 printable characters')
    source, out = Path(args.input).resolve(), Path(args.out).resolve()
    if out == run.ROOT or run.ROOT in out.parents:
        raise ValueError('historical exports must remain outside the repository')
    implementation = run.source_manifest()[0]
    run.require_build_identity(Path(args.build).resolve(), implementation)
    if out.exists():
        raise ValueError('tape output must be new')
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=out.name+'.tmp-', dir=out.parent) as temporary:
        root = Path(temporary)
        command = [str(Path(args.build).resolve()/'itch_observe'), '--tape', str(source), str(root/'report'), args.symbol, str(args.start_ns), str(args.end_ns)]
        try:
            result = subprocess.run(command, capture_output=True, text=True)
            (root/'producer.stdout').write_text(result.stdout)
            (root/'producer.stderr').write_text(result.stderr)
            if result.returncode or not result.stdout.endswith('result: SUCCESS\n'):
                raise ValueError('tape producer failed')
            payload = verify(source, root/'report')
            if run.source_manifest()[0] != implementation:
                raise ValueError('implementation changed during export')
            payload.update(date_label=args.date_label, source_id=implementation,
                           executable_sha256=run.sha(Path(args.build)/'itch_observe'), playback_speed=args.speed)
            write_view(root, payload)
            (root/'receipt.json').write_text(json.dumps(dict(status='PASS', command=command, source_id=payload['source_id'], executable_sha256=payload['executable_sha256'], input_sha256=payload['input_sha256'], events=len(payload['events']), initial_orders=len(payload['initial']), semantic_agreement=True, capture_continuity='UNVERIFIED', public_display_rights='NOT_ESTABLISHED', files={p.name:run.sha(p) for p in root.iterdir() if p.is_file()}), indent=2)+'\n')
        except BaseException as error:
            # Retain producer logs and disagreement reports, but never admit a
            # failed projection as a viewer. Existing attempts are not replaced.
            failed = out.with_name(out.name+'.failed')
            if failed.exists():
                failed = Path(tempfile.mkdtemp(prefix=out.name+'.failed-', dir=out.parent))/'attempt'
            (root/'failure.txt').write_text(repr(error)+'\n')
            shutil.copytree(root, failed)
            raise ValueError('tape export failed; evidence retained at '+str(failed)) from error
        root.rename(out)
    print(json.dumps(dict(status='PASS', viewer=str(out/'index.html'), events=len(payload['events']), initial_orders=len(payload['initial']), counts=payload['counts'])))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', required=True)
    parser.add_argument('--input', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--symbol', required=True)
    parser.add_argument('--date-label', required=True, help='1..64 printable characters; external label, not inferred from ITCH timestamps')
    parser.add_argument('--speed', type=int, choices=(1, 3, 5, 10), default=5,
                        help='initial playback multiple (default: 5)')
    parser.add_argument('--start-ns', required=True, type=int)
    parser.add_argument('--end-ns', required=True, type=int)
    export(parser.parse_args())
