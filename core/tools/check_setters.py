"""Checks setters_spec.py against a golden record.

    python check_setters.py <golden folder> [CMD ...]

Replays every recorded command that setters_spec.py describes, in order,
from the recorded connect dump, and compares the state field the table
predicts with what the switcher actually sent back (at the value offsets;
unused bytes hold leftovers and are ignored). Reports, per command:
  - values the switcher stored differently (with the sent -> stored pairs,
    so clamps and refused values show)
  - answers the table didn't expect, and expected answers that never came
Each command is matched with the next answer for its field and key, and the
state follows the switcher's answers, so one mismatch doesn't cascade.
"""
import collections
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from setters_spec import SETTERS, EQ_RANGES, ACTIONS  # noqa: E402


def read_int(b, off, size, signed):
    return int.from_bytes(b[off:off + size], 'big', signed=signed)


def write_int(b, off, size, value, signed):
    b[off:off + size] = int(value).to_bytes(size, 'big', signed=signed)


def apply_rule(rule, sent, old, buf):
    """(stored value, refused). A refused command changes nothing and gets no answer.
    buf is the state field (for rules that depend on other values)."""
    if not rule:
        return sent, False
    if rule[0] == 'unitwrap':
        whole = int(sent / 1000)
        v = (((whole + 32768) % 65536) - 32768) * 1000 + (sent - whole * 1000)
        return (v if rule[1] is None else max(rule[1], v)), False
    if rule[0] == 'negdec':
        return (sent - 1 if sent < 0 and sent % 1000 == 0 else sent), False
    if rule[0] == 'allowbits':
        ok = sent > 0 and sent & (sent - 1) == 0 and sent & buf[rule[1]]
        return (sent, False) if ok else (old, True)
    if rule[0] == 'eqfreq':
        lo, hi = EQ_RANGES.get(buf[rule[1]], (0, 0xFFFFFFFF))
        return max(lo, min(hi, sent)), False
    if rule[0] == 'refuseif':
        return (old, True) if buf[rule[1]] in rule[2] else (sent, False)
    if rule[0] == 'clamp':
        return max(rule[1], min(rule[2], sent)), False
    if rule[0] == 'mod':
        return sent % rule[1], False
    if rule[0] == 'allow':
        return (sent, False) if sent in rule[1] else (old, True)
    if rule[0] == 'ignore':
        return old, True
    raise ValueError(rule)


def chroma_cursor(b, size_set):
    """The chroma sample cursor stays inside the frame (X +-16000, Y +-9000 =
    960 x 540 pixels from the centre). Clamped positions are whole pixels;
    a size change re-reads the position in whole pixels too."""
    size = read_int(b, 8, 2, True)
    # Half the cursor in pixels, measured at sizes 620, 2500, 5000, 9925.
    pts = [(620, 37), (2500, 140), (5000, 275), (9925, 540)]
    half = pts[-1][1]
    for (s0, h0), (s1, h1) in zip(pts, pts[1:]):
        if size <= s1:
            half = h0 + (h1 - h0) * (max(size, s0) - s0) // (s1 - s0)
            break
    for off, pixels in ((4, 960), (6, 540)):
        v = read_int(b, off, 2, True)
        if size_set:
            v = int(int(v * 3 / 50) * 50 / 3)
        lim = int((pixels - half) * 50 / 3)
        write_int(b, off, 2, max(-lim, min(lim, v)), True)


def load_dump(golden):
    fields = collections.defaultdict(list)
    with open(os.path.join(golden, 'wire.jsonl'), encoding='utf-8') as f:
        for line in f:
            p = json.loads(line)
            if p['dir'] != 'rx':
                continue
            for fl in p['fields']:
                fields[fl['field']].append(bytearray.fromhex(fl['hex']))
                if fl['field'] == 'InCm':
                    return fields
    return fields


def find(state, name, keybytes):
    for b in state.get(name, []):
        if all(b[off] == v for off, v in keybytes):
            return b
    return None


def main():
    golden = sys.argv[1]
    only = set(sys.argv[2:])
    state = load_dump(golden)
    run = json.load(open(os.path.join(golden, 'results.json'), encoding='utf-8'))
    problems = collections.defaultdict(list)
    pairs = collections.defaultdict(collections.Counter)   # (cmd, prop) -> (sent, stored) counts
    checked = collections.Counter()
    spec_fields = {sp['field']: sp for sp in SETTERS.values()}
    same_stats = collections.defaultdict(collections.Counter)
    skipped = coalesced = 0

    for t in run['tests']:
        w = t.get('wire') or {}
        rx = [(f['field'], bytearray.fromhex(f['hex'])) for f in w.get('rx', [])]
        used = [False] * len(rx)   # each answer belongs to one command, in order
        txs = w.get('tx', [])
        # An action's answers can't be told from a setter's: check only the
        # setters before the first action.
        first = next((i for i, f in enumerate(txs) if f['field'] in ACTIONS), None)
        if first is not None:
            skipped += len(txs) - first
            txs = txs[:first]

        def next_answer(field, fkey):
            for i, (n, b) in enumerate(rx):
                if not used[i] and n == field and all(b[off] == v for off, v in fkey):
                    used[i] = True
                    return b
            return None

        def predict(spec, c, buf):
            """Applies one command to a copy of buf: (new buf, [(prop, sent)], refused, changed)."""
            mask = read_int(c, 0, spec['mask'], False) if spec['mask'] else -1   # no mask: all values
            out = bytearray(buf)
            props, refusals = [], []
            for p in spec['props']:
                if p['bit'] is not None and not mask & (1 << p['bit']):
                    continue
                sent = read_int(c, p['cmd'], p['size'], p['signed'])
                old = read_int(out, p['field'], p['size'], p['signed'])
                new, no = apply_rule(p['rule'], sent, old, out)
                refusals.append(no)
                write_int(out, p['field'], p['size'], new, p['signed'])
                for other, table in (p.get('effects') or {}).items():
                    if new in table:
                        q = next(x for x in spec['props'] if x['name'] == other)
                        write_int(out, q['field'], q['size'], table[new], q['signed'])
                props.append((p, sent))
            # A refused value is left as it was; the command is refused (no
            # answer) only when all its values are.
            refused = bool(refusals) and all(refusals)
            if not refused and spec.get('post') == 'chromaCursor':
                chroma_cursor(out, bool(mask & 0x10))
            changed = any(out[p['field']:p['field'] + p['size']] != buf[p['field']:p['field'] + p['size']]
                          for p, _ in props)
            return out, props, refused, changed, mask

        skip_next = 0
        for i_tx, f in enumerate(txs):
            cmd = f['field']
            spec = SETTERS.get(cmd)
            if not spec or (only and cmd not in only):
                continue
            if skip_next:
                skip_next -= 1
                continue
            c = bytearray.fromhex(f['hex'])
            fkey = [(foff, c[coff]) for coff, foff in spec['key']]
            target = find(state, spec['field'], fkey)
            if target is None:
                problems[cmd].append(f"{t['id']}: no {spec['field']} with key {fkey}")
                continue
            predicted, changed_props, refused, changed, mask = predict(spec, c, target)
            checked[cmd] += 1
            sent_text = ', '.join(f"{p['name']}={s}" for p, s in changed_props)
            got = next_answer(spec['field'], fkey)
            # Two commands for one field in the same frame get one answer: if
            # the answer fits this command and the next one together, take it.
            same = lambda a, b: all(read_int(a, p['field'], p['size'], p['signed']) ==
                                    read_int(b, p['field'], p['size'], p['signed']) for p in spec['props'])
            if got is not None and not same(predicted, got):
                both, j = predicted, i_tx + 1
                while j < len(txs) and txs[j]['field'] == cmd:
                    c2 = bytearray.fromhex(txs[j]['hex'])
                    if [(foff, c2[coff]) for coff, foff in spec['key']] != fkey:
                        break
                    both = predict(spec, c2, both)[0]
                    j += 1
                    if same(both, got):
                        coalesced += j - i_tx - 1
                        skip_next = j - i_tx - 1
                        checked[cmd] += j - i_tx - 1
                        break
                if skip_next:
                    target[:] = got
                    continue
            if not refused:
                same_stats[cmd][('changed' if changed else 'same value', 'answered' if got is not None else 'no answer')] += 1
            echo_same = spec.get('echoSame', True) and any(p.get('echoSame', True) for p, _ in changed_props)
            if not refused and not changed and not echo_same:
                if got is not None:
                    problems[cmd].append(f"{t['id']}: answered a same-value set, the table says it doesn't [{sent_text}]")
                    target[:] = got
                continue
            for p, s in changed_props:
                if got is None:
                    pairs[(cmd, p['name'])][(s, 'no answer')] += 1
            if refused:
                if got is not None:
                    problems[cmd].append(f"{t['id']}: the switcher answered, the table refuses [{sent_text}]")
                    target[:] = got
                continue
            if got is None:
                problems[cmd].append(f"{t['id']}: no answer (the switcher refused?) [{sent_text}]")
                continue
            for p in spec['props']:
                exp = read_int(predicted, p['field'], p['size'], p['signed'])
                real = read_int(got, p['field'], p['size'], p['signed'])
                if p['bit'] is None or mask & (1 << p['bit']):
                    pairs[(cmd, p['name'])][(read_int(c, p['cmd'], p['size'], p['signed']), real)] += 1
                if exp != real:
                    problems[cmd].append(f"{t['id']}: {p['name']} expected {exp}, the switcher has {real}")
            target[:] = got   # follow the switcher from here on
        # Answers no command claimed (side effects of other commands) keep the state in step.
        for i, (n, b) in enumerate(rx):
            if not used[i] and n in spec_fields:
                sp = spec_fields[n]
                fkey = [(foff, b[foff]) for _, foff in sp['key']]
                tgt = find(state, n, fkey)
                if tgt is not None:
                    tgt[:] = b

    print(f'{skipped} commands not checked (after an action command), {coalesced} commands answered together with the next one')
    for cmd in SETTERS:
        if only and cmd not in only:
            continue
        print(f'== {cmd}: {checked[cmd]} commands, {len(problems[cmd])} problems   {dict(same_stats[cmd])}')
        for line in problems[cmd][:12]:
            print('   ' + line)
        for (c, name), ctr in pairs.items():
            if c != cmd:
                continue
            odd = [(s, r) for (s, r) in ctr if s != r]
            if odd:
                print(f'   {name}: sent -> stored differs: {sorted(odd, key=str)[:12]}')
                answered = sorted({s for (s, r) in ctr if r != 'no answer'})
                refused = sorted({s for (s, r) in ctr if r == 'no answer'})
                if refused:
                    print(f'      suggest allow {answered}  (refused {refused})')


if __name__ == '__main__':
    main()
