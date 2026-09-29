"""Suggests command/field offsets for a setter, from a golden record.

    python infer_layout.py <golden folder> CMD FIELD <test id prefix> [--mask N]

For each property tested (the test id's 4th part, e.g. g.fl.source.<prop>.*)
it looks at the first command of each test (the value being set) and the
first FIELD answer, finds the command bytes that change with the value, and
the field offset where those same bytes appear. A starting point for
setters_spec.py; check_setters.py then proves it.
"""
import argparse
import collections
import json
import os


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('golden')
    ap.add_argument('cmd')
    ap.add_argument('field')
    ap.add_argument('prefix')
    ap.add_argument('--mask', type=int, default=1)
    a = ap.parse_args()
    r = json.load(open(os.path.join(a.golden, 'results.json'), encoding='utf-8'))
    by_prop = collections.defaultdict(list)
    for t in r['tests']:
        if not t['id'].startswith(a.prefix):
            continue
        w = t.get('wire') or {}
        tx = [bytes.fromhex(f['hex']) for f in w.get('tx', []) if f['field'] == a.cmd]
        rx = [bytes.fromhex(f['hex']) for f in w.get('rx', []) if f['field'] == a.field]
        if tx:
            by_prop[t['id'][len(a.prefix):].split('.')[0]].append((tx[0], rx[0] if rx else None))
    for prop, pairs in by_prop.items():
        mask = int.from_bytes(pairs[0][0][:a.mask], 'big')
        bit = mask.bit_length() - 1
        # Bytes that vary across this property's tests (the value), ignoring the mask.
        n = len(pairs[0][0])
        varying = [i for i in range(a.mask, n) if len({p[0][i] for p in pairs if len(p[0]) == n}) > 1]
        if not varying:
            print(f'{prop}: bit {bit}, no varying command bytes')
            continue
        lo, hi = min(varying), max(varying)
        # Round to a field width: 1, 2 or 4 bytes ending at hi.
        size = next(s for s in (1, 2, 4, 8) if hi - lo + 1 <= s)
        start = hi - size + 1
        # Where do those bytes land in the answer?
        hits = collections.Counter()
        for c, ans in pairs:
            if ans is None:
                continue
            val = c[start:start + size]
            for off in range(0, len(ans) - size + 1):
                if ans[off:off + size] == val:
                    hits[off] += 1
        answered = sum(1 for _, ans in pairs if ans is not None)
        best = hits.most_common(3)
        print(f'{prop}: bit {bit}, cmd offset {start} size {size}; field offset candidates {best} of {answered} answers')


if __name__ == '__main__':
    main()
