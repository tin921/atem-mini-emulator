"""Lists which BMDSwitcherAPI methods a code base calls.

    python scan_usage.py <sdk-api.json> <out.txt> <source dir or file>...

Each call `x->Method(` (C++) or `x.Method(` (C#) is resolved to an interface:
  1. by the declared type of `x` (IBMDSwitcherFoo* x / IBMDSwitcherFoo x /
     CComPtr<IBMDSwitcherFoo> x), when that type has the method;
  2. otherwise by the method name, when exactly one interface has it.
Anything still ambiguous is written as "?::Method  [candidates]" for review.
IUnknown methods (AddRef, Release, QueryInterface) are ignored.
"""
import json
import os
import re
import sys

api = json.load(open(sys.argv[1]))
out_path = sys.argv[2]
roots = sys.argv[3:]

# Current interfaces only: IBMDSwitcherFoo_v8_0 etc. are legacy versions kept
# for old clients. Methods of *Callback interfaces are implemented by the
# client (Notify, PlayingChanged...), not called, so they are listed apart.
LEGACY = re.compile(r'_v\d')
by_method = {}
callback_methods = {}
for iface, methods in api.items():
    if LEGACY.search(iface):
        continue
    for m in methods:
        if iface.endswith('Callback'):
            callback_methods.setdefault(m, set()).add(iface)
        else:
            by_method.setdefault(m, set()).add(iface)

IGNORE = {"AddRef", "Release", "QueryInterface"}
decl_re = re.compile(
    r'(?:CComPtr<\s*)?(IBMDSwitcher\w*)\s*>?\s*\*?\s*(?:&\s*)?([A-Za-z_]\w*)\s*(?:[=;,)\[]|$)', re.M)
call_re = re.compile(r'([A-Za-z_][\w\.\[\]]*?)\s*(?:->|\.)\s*([A-Z]\w+)\s*\(')

files = []
for r in roots:
    if os.path.isfile(r):
        files.append(r)
    else:
        for d, _, fs in os.walk(r):
            files += [os.path.join(d, f) for f in fs if f.endswith(('.cpp', '.h', '.cs', '.mm', '.m'))
                      and 'BMDSwitcherAPI' not in f]

# Variable -> declared interface, across all files (members live in headers).
var_type = {}
texts = {}
for f in files:
    t = open(f, encoding='utf-8', errors='replace').read()
    texts[f] = t
    for m in decl_re.finditer(t):
        var_type.setdefault(m.group(2), set()).add(m.group(1))

used = {}      # "Iface::Method" -> set(files)
unresolved = {}
for f, t in texts.items():
    for m in call_re.finditer(t):
        var, meth = m.group(1).split('.')[-1].split('[')[0], m.group(2)
        if meth in IGNORE or meth not in by_method:
            continue
        cands = by_method[meth]
        types = {ty for ty in var_type.get(var, set()) if ty in cands}
        if len(types) == 1:
            key = f"{types.pop()}::{meth}"
        elif len(cands) == 1:
            key = f"{next(iter(cands))}::{meth}"
        else:
            unresolved.setdefault(meth, set()).add(os.path.basename(f))
            continue
        used.setdefault(key, set()).add(os.path.basename(f))

with open(out_path, 'w', encoding='utf-8') as o:
    for k in sorted(used):
        o.write(f"{k}\t{', '.join(sorted(used[k]))}\n")
    for meth in sorted(unresolved):
        o.write(f"?::{meth}\t[{', '.join(sorted(by_method[meth]))[:200]}]\t{', '.join(sorted(unresolved[meth]))}\n")
print(f"{len(used)} resolved, {len(unresolved)} ambiguous -> {out_path}")
