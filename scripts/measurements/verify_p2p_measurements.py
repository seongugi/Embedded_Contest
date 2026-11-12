#!/usr/bin/env python3
"""Check published P2P metrics against the archived event records."""
import collections
import csv
import hashlib
import io
import json
from pathlib import Path
import statistics
import zipfile

ROOT = Path(__file__).resolve().parents[2]
DATA = ROOT / 'docs/measurements/20261007-p2p'
summary = json.loads((DATA / 'summary.json').read_text(encoding='utf-8'))
manifest = json.loads((DATA / 'provenance.json').read_text(encoding='utf-8'))


def key(row):
    return tuple(int(row[k]) for k in ('station_id', 'sequence', 'detection_its_ms'))


for name, expected in manifest['published_sha256'].items():
    blob = (DATA / name).read_bytes()
    if name.endswith(('.csv', '.json')):
        blob = blob.replace(b'\r\n', b'\n')
    assert hashlib.sha256(blob).hexdigest() == expected, name

with zipfile.ZipFile(DATA / manifest['raw_archive']) as archive:
    raw = {}
    for name, expected in manifest['raw_sha256'].items():
        blob = archive.read(name)
        assert hashlib.sha256(blob).hexdigest() == expected, name
        raw[name] = list(csv.DictReader(io.StringIO(blob.decode('utf-8-sig'))))

inputs = collections.Counter(map(key, raw['craft2_sensor_ipc.csv']))
assert len(inputs) == summary['requested'] == 10000 and set(inputs.values()) == {1}
assert {k[1] for k in inputs} == set(range(1, 10001))
go = raw['craft2_events.csv']
for stage in ('GO_IPC', 'TX_BEGIN', 'TX'):
    assert collections.Counter(key(r) for r in go if r['stage'] == stage) == inputs, stage
assert collections.Counter(r['stage'] for r in go) == summary['sender_stages']
begins = {key(r): r for r in go if r['stage'] == 'TX_BEGIN'}
acks = {key(r): r for r in go if r['stage'] == 'ACK_ALL'}
timeouts = {key(r) for r in go if r['stage'] == 'ACK_TIMEOUT'}
assert set(acks).isdisjoint(timeouts) and set(acks) | timeouts == set(inputs)
received_total = 0
for client in summary['clients']:
    device = client['device']
    rows = raw[device + '_events.csv']
    received = collections.Counter(key(r) for r in rows if r['stage'] == 'RX')
    forwarded = collections.Counter(key(r) for r in rows if r['stage'] == 'FILTER_IPC')
    sink = collections.Counter(map(key, raw[device + '_sink_events.csv']))
    stages = collections.Counter(r['stage'] for r in rows)
    assert set(received) <= set(inputs)
    assert forwarded == sink == collections.Counter({k: 1 for k in received})
    assert len(received) == client['unique_rx'] == client['unique_filter_delivered']
    assert len(inputs) - len(received) == client['network_missing']
    assert abs(len(received) / len(inputs) * 100 - client['receive_pct']) < 1e-10
    duplicates = sum(v - 1 for v in received.values())
    assert duplicates == client['duplicate_packets'] == stages['DUPLICATE_SUPPRESSED']
    assert stages['PROCESS_START'] == 1
    assert not any(stages[s] for s in ('QUEUE_DROP', 'DECODE_ERROR', 'CONVERSION_ERROR'))
    received_total += len(received)
assert received_total == summary['received_pairs'] == summary['total_unique_filter_deliveries']
assert summary['expected_pairs'] == len(inputs) * len(summary['clients']) == 40000
assert abs(received_total / 40000 * 100 - summary['receive_pct']) < 1e-10
rtts = sorted((int(r['monotonic_ns']) - int(begins[k]['monotonic_ns'])) / 1e6 for k, r in acks.items())
assert len(rtts) == summary['all_ack_round_trip']['n']
assert abs(statistics.mean(rtts) - summary['all_ack_round_trip']['mean_ms']) < 1e-9
assert abs(statistics.median(rtts) - summary['all_ack_round_trip']['median_ms']) < 1e-9
pos = (len(rtts) - 1) * .95
lo = int(pos)
p95 = rtts[lo] + (rtts[min(lo + 1, len(rtts) - 1)] - rtts[lo]) * (pos - lo)
assert abs(p95 - summary['all_ack_round_trip']['p95_ms']) < 1e-9
assert all(all(record.values()) for record in manifest['restoration'].values())
metrics = json.loads((DATA.parent / 'metrics.json').read_text(encoding='utf-8'))['supporting_selection']['P2P']
assert metrics['received_deliveries'] == received_total
assert metrics['delivery_rate_percent'] == summary['receive_pct']
print(f'PASS: {received_total:,}/40,000 unique receptions, one IPC per received event, hashes and ACK RTT match')
