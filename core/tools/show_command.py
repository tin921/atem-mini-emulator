"""Shows how the real switcher answered a command, from a golden record.

    python show_command.py <golden folder> <CMD> [<FIELD>] [--max N]

For every test that sent CMD: the command payloads sent and the FIELD
payloads that came back (default: all fields), plus FIELD in the connect
dump. The raw material for the layouts in setters.txt.
"""
import argparse
import json
import os


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('golden')
    ap.add_argument('cmd')
    ap.add_argument('field', nargs='?')
    ap.add_argument('--max', type=int, default=12)
    a = ap.parse_args()
    r = json.load(open(os.path.join(a.golden, 'results.json'), encoding='utf-8'))
    if a.field:
        with open(os.path.join(a.golden, 'wire.jsonl'), encoding='utf-8') as f:
            for line in f:
                p = json.loads(line)
                if p['dir'] != 'rx':
                    continue
                done = False
                for fl in p['fields']:
                    if fl['field'] == a.field:
                        print(f'dump {a.field}: {fl["hex"]}')
                    done = done or fl['field'] == 'InCm'
                if done:
                    break
    shown = 0
    for t in r['tests']:
        w = t.get('wire') or {}
        tx = [f['hex'] for f in w.get('tx', []) if f['field'] == a.cmd]
        if not tx:
            continue
        rx = [f"{f['field']} {f['hex']}" for f in w.get('rx', []) if (f['field'] == a.field if a.field else f['field'] != 'Time')]
        rb = t.get('obs', {}).get('readback')
        print(f"{t['id']}  (value {t.get('obs', {}).get('value')}, readback {rb}, set {t.get('obs', {}).get('set')})")
        for h in tx:
            print(f'   tx {a.cmd} {h}')
        for h in rx[:6]:
            print(f'   rx {h}')
        shown += 1
        if shown >= a.max:
            break


if __name__ == '__main__':
    main()
