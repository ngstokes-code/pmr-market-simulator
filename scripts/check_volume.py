#!/usr/bin/env python3
"""Independent raw-byte accounting check; does not call the C++ decoder or ledger."""
import argparse
import collections
import csv
import json
import math
import pathlib
import struct
import tempfile
from itch_reference import Reader, State

RECORD = struct.Struct("<QQQQQHc")
INTERVAL = 300 * 10**9
KINDS = ("E", "CY", "P", "Q", "CN")
LENGTHS = {"A": 36, "F": 40, "E": 31, "C": 36, "P": 44, "Q": 40, "B": 19, "R": 39, "S": 12}



def check_fraction(row, counts):
    denominator = sum(counts[:3])
    if not denominator:
        if row["non_displayed_fraction"] != "NA":
            raise ValueError("undefined fraction is not NA")
    else:
        value = float(row["non_displayed_fraction"])
        if not math.isfinite(value) or abs(value - counts[2] / denominator) > 1e-12:
            raise ValueError("fraction disagreement")


def expected_cells(report, names):
    result = {}
    with open(pathlib.Path(report) / "cells.csv", newline="") as source:
        for row in csv.DictReader(source):
            locate, time = int(row["locate"]), int(row["interval_ns"])
            key = (locate, time // INTERVAL)
            if time % INTERVAL or key in result:
                raise ValueError("unaligned or duplicate cell")
            if names.get(locate) != row["stock"]:
                raise ValueError("cell symbol label disagreement")
            counts = tuple(int(row[k]) for k in KINDS)
            check_fraction(row, counts)
            result[key] = counts
    return result


def stock_label(data):
    text = bytes(data).decode("ascii").rstrip(" ")
    if not text or any(ord(c) < 32 or ord(c) > 126 for c in text):
        raise ValueError("invalid report stock label")
    return text


def check(path, report, output, partitions):
    cells = collections.defaultdict(lambda: [0] * 5)
    messages = records = breaks = corrected = companions = corrected_nonprintable = 0
    peak_records = 0
    events = []
    names = {}
    reader, protocol = Reader(path), State()
    with tempfile.TemporaryDirectory(prefix="msim-volume-check-") as temporary:
        paths = [pathlib.Path(temporary) / str(p) for p in range(partitions)]
        files = [p.open("wb") for p in paths]
        try:
            for ordinal, frame in reader:
                messages = ordinal
                protocol.accept(ordinal, frame)
                kind = chr(frame[0])
                if kind not in LENGTHS:
                    continue
                if len(frame) != LENGTHS[kind]:
                    raise ValueError("wrong analysis message length")
                locate = struct.unpack_from(">H", frame, 1)[0]
                if kind == "R":
                    name = stock_label(frame[11:19])
                    if locate in names and names[locate] != name:
                        raise ValueError("directory changed symbol")
                    names[locate] = name
                    continue
                timestamp = int.from_bytes(frame[5:11], "big")
                if timestamp >= 86400 * 10**9:
                    raise ValueError("timestamp outside day")
                if kind == "S":
                    events.append((chr(frame[11]), timestamp))
                    continue
                if kind in ("A", "F"):
                    if locate in names and names[locate] != stock_label(frame[24:32]):
                        raise ValueError("order symbol disagrees with directory")
                    continue
                ref = quantity = 0
                if kind in ("E", "C"):
                    ref, quantity, match = struct.unpack_from(">QIQ", frame, 11)
                    if kind == "C":
                        if frame[31] not in (ord("Y"), ord("N")):
                            raise ValueError("invalid printable flag")
                        kind = "CY" if frame[31] == ord("Y") else "CN"
                elif kind == "P":
                    # The raw side byte is deliberately not used.
                    quantity = struct.unpack_from(">I", frame, 20)[0]
                    match = struct.unpack_from(">Q", frame, 36)[0]
                    if names.get(locate) != stock_label(frame[24:32]):
                        raise ValueError("trade symbol disagrees with directory")
                elif kind == "Q":
                    quantity = struct.unpack_from(">Q", frame, 11)[0]
                    match = struct.unpack_from(">Q", frame, 31)[0]
                    if names.get(locate) != stock_label(frame[19:27]):
                        raise ValueError("cross symbol disagrees with directory")
                else:
                    match = struct.unpack_from(">Q", frame, 11)[0]
                if kind not in ("B", "Q") and not quantity:
                    raise ValueError("zero executed shares")
                encoded_kind = {"CY": b"Y", "CN": b"N"}.get(kind, kind.encode())
                files[match % partitions].write(RECORD.pack(match, ordinal, timestamp, ref, quantity, locate, encoded_kind))
                records += 1
        finally:
            for stream in files:
                stream.close()
        protocol.finish()
        for partition in paths:
            originals, corrections, nonprintable = {}, {}, collections.defaultdict(dict)
            with partition.open("rb") as stream:
                while packed := stream.read(RECORD.size):
                    if len(packed) != RECORD.size:
                        raise ValueError("partial checker record")
                    match, ordinal, timestamp, ref, quantity, locate, raw_kind = RECORD.unpack(packed)
                    kind = {b"Y": "CY", b"N": "CN"}.get(raw_kind, raw_kind.decode())
                    if kind == "B":
                        if match in corrections:
                            raise ValueError("duplicate correction")
                        corrections[match] = (ordinal, locate)
                    elif kind == "CN":
                        if ref in nonprintable[match]:
                            raise ValueError("duplicate non-printable execution")
                        nonprintable[match][ref] = (ordinal, timestamp, quantity, locate, kind)
                    else:
                        if match in originals:
                            raise ValueError(f"duplicate volume-bearing match {match}")
                        originals[match] = (ordinal, timestamp, quantity, locate, kind)
            peak_records = max(peak_records, len(originals) + len(corrections) + sum(map(len, nonprintable.values())))
            for match in originals.keys() | nonprintable.keys() | corrections.keys():
                entries = list(nonprintable.get(match, {}).values())
                included = originals.get(match)
                if included:
                    if entries:
                        companions += 1
                    entries.append(included)
                if not entries:
                    raise ValueError("unknown correction")
                if len({entry[3] for entry in entries}) != 1:
                    raise ValueError("match crosses symbols")
                correction = corrections.get(match)
                if correction is not None:
                    correction_ordinal, correction_locate = correction
                    if correction_locate != entries[0][3]:
                        raise ValueError("break crosses symbols")
                    if correction_ordinal <= max(entry[0] for entry in entries) or (included and included[4] == "Q"):
                        raise ValueError("early or unsupported cross correction")
                    breaks += 1
                    corrected += included[2] if included else 0
                    corrected_nonprintable += sum(entry[2] for entry in entries if entry[4] == "CN")
                for _, timestamp, quantity, locate, kind in entries:
                    row = cells[(locate, timestamp // INTERVAL)]
                    if correction is None:
                        row[KINDS.index(kind)] += quantity
    actual = {key: tuple(value) for key, value in cells.items()}
    expected = expected_cells(report, names)
    if actual != expected:
        difference = next(k for k in actual.keys() | expected.keys() if actual.get(k) != expected.get(k))
        raise ValueError(f"cell disagreement at {difference}: {actual.get(difference)} != {expected.get(difference)}")
    with open(pathlib.Path(report) / "system-events.csv", newline="") as stream:
        supplied_events = [(row["event"], int(row["timestamp_ns"])) for row in csv.DictReader(stream)]
    if events != supplied_events:
        raise ValueError("system event disagreement")
    totals = dict(zip(KINDS, map(sum, zip(*actual.values()))))
    summary = json.loads((pathlib.Path(report) / "summary.json").read_text())
    for key, value in dict(totals, messages=messages, records=records, breaks=breaks, corrected_shares=corrected, companion_groups=companions, corrected_nonprintable_shares=corrected_nonprintable).items():
        if summary[key] != value:
            raise ValueError("summary disagreement: " + key)
    if summary['input_bytes'] != reader.bytes or summary['input_sha256'] != reader.sha.hexdigest():
        raise ValueError('input identity disagreement')
    # Independently reconcile the two other public aggregation surfaces.
    for filename, key_name in (("symbols.csv", "locate"), ("intervals.csv", "interval_ns")):
        grouped = collections.defaultdict(lambda: [0] * 5)
        for (locate, interval), values in actual.items():
            key = locate if key_name == "locate" else interval * INTERVAL
            for i, value in enumerate(values):
                grouped[key][i] += value
        with open(pathlib.Path(report) / filename, newline="") as stream:
            rows = list(csv.DictReader(stream))
            if key_name == "locate" and any(names.get(int(row["locate"])) != row["stock"] for row in rows):
                raise ValueError("symbol label disagreement")
            supplied = {}
            for row in rows:
                key = int(row[key_name])
                if key in supplied:
                    raise ValueError("duplicate aggregate row")
                values = tuple(int(row[k]) for k in KINDS)
                check_fraction(row, values)
                supplied[key] = values
        if supplied != {k: tuple(v) for k, v in grouped.items()}:
            raise ValueError("aggregate disagreement: " + filename)
    result = dict(status="PASS", input_bytes=reader.bytes, input_sha256=reader.sha.hexdigest(), terminal_event=protocol.session, messages=messages, records=records, breaks=breaks, corrected_shares=corrected, companion_groups=companions, corrected_nonprintable_shares=corrected_nonprintable, cells=len(actual), totals=totals, partitions=partitions, peak_partition_records=peak_records, implementation="independent Python framing, field decoding, dictionaries and aggregation")
    pathlib.Path(output).write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input")
    parser.add_argument("report")
    parser.add_argument("output")
    parser.add_argument("--partitions", type=int, default=16)
    args = parser.parse_args()
    if not 1 <= args.partitions <= 256:
        parser.error("partitions must be 1 to 256")
    check(args.input, args.report, args.output, args.partitions)
