"""Makes a golden record from a sweep run, without the transferred content.

    python slim_record.py <run folder> <golden folder>

Copies coverage.txt, and results.json / wire.jsonl with every file-transfer
payload (FTDa: the bytes of a still or a macro being uploaded or downloaded)
cut to its first 8 bytes (the transfer header) plus its length and SHA-256.
The protocol stays complete for the emulator; the switcher's pictures (and
the megabytes) stay out of the golden record. Keep the full run locally.
"""
import hashlib
import json
import os
import shutil
import sys

KEEP = 8          # FTDa bytes kept: transfer id, size
HEADER = 12       # ATEM packet header, before the fields


def slim_field(name, hexdata):
    if name != 'FTDa' or len(hexdata) <= KEEP * 2:
        return hexdata, None
    data = bytes.fromhex(hexdata)
    note = {'elided': len(data) - KEEP, 'sha256': hashlib.sha256(data).hexdigest()}
    return hexdata[:KEEP * 2], note


def slim_fields(fields):
    out = []
    for f in fields:
        f = dict(f)
        name = f.get('field') or f.get('name')
        if 'hex' in f:
            f['hex'], note = slim_field(name, f['hex'])
            if note:
                f.update(note)
        out.append(f)
    return out


def main():
    src, dst = sys.argv[1], sys.argv[2]
    os.makedirs(dst, exist_ok=True)
    shutil.copy(os.path.join(src, 'coverage.txt'), dst)

    with open(os.path.join(src, 'wire.jsonl'), encoding='utf-8') as fin, \
         open(os.path.join(dst, 'wire.jsonl'), 'w', encoding='utf-8', newline='\n') as fout:
        for line in fin:
            p = json.loads(line)
            fields = p.get('fields', [])
            if any((f.get('name') or f.get('field')) == 'FTDa' for f in fields):
                p['fields'] = slim_fields(fields)
                # The whole datagram: keep the header, the fields carry the rest.
                p['hex'] = p['hex'][:HEADER * 2]
                p['hexElided'] = True
            fout.write(json.dumps(p, separators=(',', ':')) + '\n')

    run = json.load(open(os.path.join(src, 'results.json'), encoding='utf-8'))
    for t in run.get('tests', []):
        wire = t.get('wire')
        if isinstance(wire, dict):
            for k in ('tx', 'rx'):
                if k in wire:
                    wire[k] = slim_fields(wire[k])
    json.dump(run, open(os.path.join(dst, 'results.json'), 'w', encoding='utf-8', newline='\n'), indent=1)

    for name in ('results.json', 'wire.jsonl'):
        print(f'{name}: {os.path.getsize(os.path.join(src, name)) / 1e6:.1f} MB -> '
              f'{os.path.getsize(os.path.join(dst, name)) / 1e6:.1f} MB')


if __name__ == '__main__':
    main()
