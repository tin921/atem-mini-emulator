"""Prints the file transfers in a sweep run's wire log, in order.

    python show_transfer.py <run folder> [test id ...]

For each test (or the tests named), the transfer and lock commands and
answers (FT**, LOCK, LKST, LKOB, PLCK, MPfe, MPrp, MPCS, CSTL, SMPS, ...),
in the order they crossed the wire. FTDa payloads are shortened.
"""
import json
import os
import sys

SHOW = {'LOCK', 'LKST', 'LKOB', 'PLCK', 'MPfe', 'MPrp', 'MPCS', 'CSTL', 'SMPS', 'Capt', 'CLMP', 'MSRc',
        'MRCP', 'MPSS', 'MRcS', 'CMPr', 'MAct', 'MSlp'}


def main():
    run = sys.argv[1]
    want = set(sys.argv[2:])
    results = json.load(open(os.path.join(run, 'results.json'), encoding='utf-8'))
    # Each test's tx fields, in order, to find where it starts in the wire log.
    tests = [(t['id'], [f['hex'] for f in (t.get('wire') or {}).get('tx', [])]) for t in results['tests']]
    lines = [json.loads(l) for l in open(os.path.join(run, 'wire.jsonl'), encoding='utf-8')]
    txpos = []   # (line index, hex) of every tx field
    for i, p in enumerate(lines):
        if p['dir'] == 'tx':
            for f in p['fields']:
                txpos.append((i, f['hex']))
    at = 0
    starts = []
    for tid, txs in tests:
        if not txs:
            starts.append((tid, None))
            continue
        while at < len(txpos) and txpos[at][1] != txs[0]:
            at += 1
        starts.append((tid, txpos[at][0] if at < len(txpos) else None))
        at += len(txs)
    for k, (tid, start) in enumerate(starts):
        if want and tid not in want or start is None:
            continue
        end = next((s for _, s in starts[k + 1:] if s is not None), len(lines))
        print(f'===== {tid}')
        t0 = lines[start]['ms']
        for p in lines[start:end]:
            for f in p['fields']:
                n = f['field']
                if n.startswith('FT') or n in SHOW:
                    h = f['hex']
                    if n == 'FTDa' and len(h) > 48:
                        h = f'{h[:32]}... ({len(h) // 2} bytes)'
                    print(f"  {p['ms'] - t0:6d} {p['dir']} {n} {h}")


if __name__ == '__main__':
    main()
