"""Independent ITCH 5.0 reader and live-reference semantics for verification.

Uses Python integers, dictionaries and sparse bit sets, not the C++ decoder,
reference window, node pool or worker pipeline. Full-session file contract only.
Unused message payloads are length checked, not business-rule certified.
"""
import hashlib
import os
import struct

LENGTHS = dict(S=12, R=39, H=25, h=21, I=50, W=12, A=36, F=40,
               E=31, C=36, X=23, D=19, U=35, P=44, Q=40, B=19,
               Y=20, L=26, V=35, K=28, J=35, N=20)
DAY = 86400 * 10**9


def number(data, at, length):
    return int.from_bytes(data[at:at + length], 'big')


def stock(data):
    data = bytes(data)
    if any(c < 32 or c > 126 for c in data) or not data.rstrip(b' '):
        raise ValueError('invalid report stock label')
    return bytes(data)


class Reader:
    def __init__(self, path):
        self.path = path
        self.sha = hashlib.sha256()
        self.messages = self.bytes = 0
        self.identity = None

    def __iter__(self):
        with open(self.path, 'rb') as source:
            initial = os.fstat(source.fileno())
            self.identity = (initial.st_dev, initial.st_ino, initial.st_size,
                             initial.st_mtime_ns, initial.st_ctime_ns)
            remaining, carry = initial.st_size, b''
            while remaining:
                block = source.read(min(8 * 1024 * 1024, remaining))
                if not block:
                    raise ValueError('input shortened')
                self.sha.update(block)
                self.bytes += len(block)
                remaining -= len(block)
                data, at = carry + block, 0
                while at + 2 <= len(data):
                    length = struct.unpack_from('>H', data, at)[0]
                    if length < 11:
                        raise ValueError('short frame')
                    if at + 2 + length > len(data):
                        break
                    frame = memoryview(data)[at + 2:at + 2 + length]
                    kind = chr(frame[0])
                    if LENGTHS.get(kind) != length:
                        raise ValueError('unsupported type or wrong message length')
                    self.messages += 1
                    yield self.messages, frame
                    at += 2 + length
                carry = data[at:]
            if carry:
                raise ValueError('truncated input')
            for info in (os.fstat(source.fileno()), os.stat(self.path)):
                if self.identity != (info.st_dev, info.st_ino, info.st_size,
                                     info.st_mtime_ns, info.st_ctime_ns):
                    raise ValueError('input changed')


class State:
    def __init__(self):
        self.session = ''
        self.names, self.locates, self.live, self.used = {}, {}, {}, {}
        self.events = []
        self.directory_metadata = {}
        self.ordinal = 0

    def birth(self, ref):
        block, bit = divmod(ref, 65536)
        bits = self.used.get(block)
        if bits is None:
            bits = self.used[block] = bytearray(8192)
        byte, shift = divmod(bit, 8)
        mask = 1 << shift
        if bits[byte] & mask:
            raise ValueError('reused day reference')
        bits[byte] |= mask

    def accept(self, ordinal, f):
        k, loc, ts = chr(f[0]), number(f, 1, 2), number(f, 5, 6)
        if ts >= DAY:
            raise ValueError('timestamp outside day')
        if self.session == 'C' or (not self.ordinal and k != 'S'):
            raise ValueError('invalid session boundary')
        if k in ('S', 'V', 'W', 'K'):
            if loc:
                raise ValueError('global message has nonzero locate')
        elif not loc:
            raise ValueError('symbol message has zero locate')
        if loc and k != 'R' and loc not in self.names:
            raise ValueError('symbol event precedes directory')
        if k == 'S':
            code = chr(f[11])
            expected = 'OSQMEC'[len(self.events)] if len(self.events) < 6 else None
            if code != expected:
                raise ValueError('system events out of order')
            self.events.append((code, ts))
            self.session = code
        offset = {'R': 11, 'H': 11, 'h': 11, 'I': 28,
                  'A': 24, 'F': 24, 'P': 24, 'Q': 19}.get(k)
        if offset is not None:
            label = stock(f[offset:offset + 8])
            if k == 'R':
                metadata=(chr(f[19]),chr(f[29]),number(f,21,4))
                if metadata[0] not in 'QGSNAPZV ' or metadata[1] not in 'PT':
                    raise ValueError('invalid directory category or authenticity')
                if loc in self.directory_metadata and self.directory_metadata[loc]!=metadata:
                    raise ValueError('directory metadata changed')
                self.directory_metadata[loc]=metadata
                if (loc in self.names and self.names[loc] != label) or (label in self.locates and self.locates[label] != loc):
                    raise ValueError('directory changed symbol')
                self.names[loc], self.locates[label] = label, loc
            elif label != self.names[loc]:
                raise ValueError('order symbol disagrees with directory' if k in ('A', 'F') else 'message symbol disagrees with directory')
        price_at = {'A': 32, 'F': 32, 'P': 32, 'C': 32, 'U': 31, 'Q': 27}.get(k)
        if price_at is not None and number(f, price_at, 4) > 2_000_000_000:
            raise ValueError('price outside Price(4) domain')
        if k == 'H' and chr(f[19]) not in 'HPQT':
            raise ValueError('invalid trading state')
        if k == 'h' and (chr(f[19]) not in 'QBX' or chr(f[20]) not in 'HT'):
            raise ValueError('invalid operational halt')
        if k == 'W' and chr(f[11]) not in '123':
            raise ValueError('invalid circuit breaker')
        if k == 'I' and (chr(f[27]) not in 'BSNOP' or chr(f[48]) not in 'OCHA' or
                         chr(f[49]) not in ' L123456789ABC' or
                         any(number(f, p, 4) > 2_000_000_000 for p in (36, 40, 44))):
            raise ValueError('invalid NOII field')
        if k == 'Q' and chr(f[39]) not in 'OCH':
            raise ValueError('invalid cross type')
        if k in ('A','F','U') and self.session not in ('S','Q','M'):
            raise ValueError('order birth outside system hours')
        if k in ('A', 'F'):
            ref, qty, side = number(f, 11, 8), number(f, 20, 4), chr(f[19])
            if not qty or side not in 'BS':
                raise ValueError('invalid order quantity or side')
            self.birth(ref)
            self.live[ref] = (loc, qty)
        elif k in ('E', 'C', 'X', 'D', 'U'):
            ref = number(f, 11, 8)
            if ref not in self.live or self.live[ref][0] != loc:
                raise ValueError('unknown or cross-symbol reference')
            prior = self.live[ref][1]
            if k == 'U':
                new, qty = number(f, 19, 8), number(f, 27, 4)
                if not qty:
                    raise ValueError('zero replacement quantity')
                self.birth(new)
                del self.live[ref]
                self.live[new] = (loc, qty)
            else:
                qty = prior if k == 'D' else number(f, 19, 4)
                if not qty or qty > prior:
                    raise ValueError('zero or excessive reduction')
                if k == 'C' and chr(f[31]) not in 'YN':
                    raise ValueError('invalid printable flag')
                if qty == prior:
                    del self.live[ref]
                else:
                    self.live[ref] = (loc, prior - qty)
        self.ordinal = ordinal
        return k, loc, ts

    def finish(self):
        if self.session != 'C' or len(self.events) != 6:
            raise ValueError('incomplete session')
