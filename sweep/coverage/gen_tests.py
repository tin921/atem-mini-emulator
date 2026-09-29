"""Generates sweep/src/tests_generated.cpp from BMDSwitcherAPI.idl.

    python gen_tests.py <BMDSwitcherAPI.idl>

For every interface with methods in category 3 (api-supported.tsv, "3 sweep")
and an accessor in tests_access.cpp, it registers:

  g.<iface>.state            every simple getter, recorded
  g.<iface>.<prop>.<value>   one test per value for each Set<X>/Get<X> pair:
                             the original is read, the value set and read back
                             (recorded: good and bad values alike), then the
                             original is put back and checked
  g.<iface>.<method>.<arg>   getters that take one enum/int argument
  g.<iface>.<action>         argument-less actions (Reset, ...): every settable
                             value is saved first and put back after
  g.<iface>.callbacks        AddCallback + RemoveCallback with a generated sink

Only methods in category 3 get tests (their partner getters are used for the
read-back). Everything else — storage (macros, stills), multi-argument
setters, iterators — is written by hand in the other tests_*.cpp files.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'src', 'tests_generated.cpp')

# interface -> (test id prefix, accessor keys). One set of tests per key.
TARGETS = {
    'IBMDSwitcher': ('sw', ['IBMDSwitcher']),
    'IBMDSwitcherMixEffectBlock': ('me', ['IBMDSwitcherMixEffectBlock']),
    'IBMDSwitcherInput': ('input', ['IBMDSwitcherInput']),
    'IBMDSwitcherInputColor': ('color', ['IBMDSwitcherInputColor#1', 'IBMDSwitcherInputColor#2']),
    'IBMDSwitcherInputAux': ('aux', ['IBMDSwitcherInputAux']),
    'IBMDSwitcherKeyDVEParameters': ('dve', ['IBMDSwitcherKeyDVEParameters']),
    'IBMDSwitcherKeyLumaParameters': ('luma', ['IBMDSwitcherKeyLumaParameters']),
    'IBMDSwitcherKeyAdvancedChromaParameters': ('achroma', ['IBMDSwitcherKeyAdvancedChromaParameters']),
    'IBMDSwitcherKeyPatternParameters': ('pattern', ['IBMDSwitcherKeyPatternParameters']),
    'IBMDSwitcherKeyFlyKeyFrameParameters': ('keyframe', ['IBMDSwitcherKeyFlyKeyFrameParameters#A',
                                                          'IBMDSwitcherKeyFlyKeyFrameParameters#B']),
    'IBMDSwitcherTransitionMixParameters': ('mix', ['IBMDSwitcherTransitionMixParameters']),
    'IBMDSwitcherTransitionDipParameters': ('dip', ['IBMDSwitcherTransitionDipParameters']),
    'IBMDSwitcherTransitionWipeParameters': ('wipe', ['IBMDSwitcherTransitionWipeParameters']),
    'IBMDSwitcherTransitionDVEParameters': ('trdve', ['IBMDSwitcherTransitionDVEParameters']),
    'IBMDSwitcherFairlightAudioMixer': ('fl.mixer', ['IBMDSwitcherFairlightAudioMixer']),
    'IBMDSwitcherFairlightAudioInput': ('fl.input', ['IBMDSwitcherFairlightAudioInput']),
    'IBMDSwitcherFairlightAnalogAudioInput': ('fl.analog', ['IBMDSwitcherFairlightAnalogAudioInput']),
    'IBMDSwitcherFairlightAudioSource': ('fl.source', ['IBMDSwitcherFairlightAudioSource']),
    'IBMDSwitcherFairlightAudioEqualizer': ('fl.eq', ['IBMDSwitcherFairlightAudioEqualizer']),
    'IBMDSwitcherFairlightAudioEqualizerBand': ('fl.eqband', ['IBMDSwitcherFairlightAudioEqualizerBand']),
    'IBMDSwitcherFairlightAudioDynamicsProcessor': ('fl.dyn', ['IBMDSwitcherFairlightAudioDynamicsProcessor']),
    'IBMDSwitcherFairlightAudioCompressor': ('fl.comp', ['IBMDSwitcherFairlightAudioCompressor']),
    'IBMDSwitcherFairlightAudioLimiter': ('fl.limit', ['IBMDSwitcherFairlightAudioLimiter']),
    'IBMDSwitcherFairlightAudioExpander': ('fl.expand', ['IBMDSwitcherFairlightAudioExpander']),
    'IBMDSwitcherStillCapture': ('capture', ['IBMDSwitcherStillCapture']),
    'IBMDSwitcherMediaPool': ('media', ['IBMDSwitcherMediaPool']),
}

# Setters that need an opt-in flag (they do more than change a setting).
OPT_IN = {
    'IBMDSwitcherFairlightAnalogAudioInput::SetMicPowerMode': 'allowMicPower',
}

INT_TYPES = {'int', 'short', 'long long', 'unsigned short', 'unsigned int', 'unsigned long long',
             'BMDSwitcherAudioInputId', 'BMDSwitcherFairlightAudioSourceId', 'BMDSwitcherRecordDiskId',
             'BMDSwitcherHyperDeckClipId', 'BMDSwitcherHyperDeckId', 'BMDSwitcherAudioOutputId',
             'BMDSwitcherAudioRoutingSourceId', 'BMDSwitcherAudioRoutingOutputId'}


def parse(idl):
    text = open(idl, encoding='utf-8', errors='replace').read()
    enums = {}
    for m in re.finditer(r'typedef \[v1_enum\] enum _(\w+) \{(.*?)\} (\w+);', text, re.S):
        members = re.findall(r'(\w+)\s*=\s*(?:/\*[^*]*\*/\s*)?(0x[0-9A-Fa-f]+|\d+)', m.group(2))
        enums[m.group(3)] = [(n, int(v, 0)) for n, v in members]
    ifaces = {}
    for m in re.finditer(r'\] interface (IBMDSwitcher\w*) : IUnknown\s*\{(.*?)\n\};', text, re.S):
        name = m.group(1)
        if '_v' in name:
            continue
        methods = []
        for mm in re.finditer(r'^\s*(\w[\w ]*?)\s+(\w+)\s*\((.*?)\);', m.group(2), re.M | re.S):
            ret, mname, args = mm.group(1), mm.group(2), re.sub(r'/\*.*?\*/', '', mm.group(3)).strip()
            params = []
            if args and args != 'void':
                for a in args.split(','):
                    pm = re.match(r'\s*\[(in|out)[^\]]*\]\s*(.+?)\s*(\w+)\s*$', a.strip())
                    if not pm:
                        params = None
                        break
                    params.append((pm.group(1), pm.group(2).strip(), pm.group(3)))
            methods.append({'ret': ret, 'name': mname, 'params': params})
        ifaces[name] = methods
    return enums, ifaces


def kind(t, enums):
    if t == 'BOOL':
        return 'bool'
    if t in ('double', 'float'):
        return 'double'
    if t == 'BMDSwitcherInputId':
        return 'source'
    if t in INT_TYPES:
        return 'unsigned' if t.startswith('unsigned') or t.startswith('BMDSwitcherAudioRouting') else 'int'
    if t in enums:
        return 'enum'
    return None


def values(t, k, enums):
    """(C++ literal, id suffix, label) per value; bad values included."""
    if k == 'bool':
        return [('TRUE', 'on', 'on'), ('FALSE', 'off', 'off')]
    if k == 'double':
        vs = [0, 0.25, 0.5, 1, 10, 50, 100, -1, -100, 1e6]
        return [(repr(float(v)), num_id(v), str(v)) for v in vs]
    if k == 'source':
        vs = [0, 1, 2, 3, 4, 1000, 2001, 2002, 3010, 3011, 8001, 10010, 10011, 11001, 5, 9999, -1, 6000]
        return [(f'{v}LL', num_id(v), str(v)) for v in vs]
    if k == 'int':
        vs = [0, 1, 2, 10, 100, 1000, 65535, -1]
        return [(f'static_cast<{t}>({v})', num_id(v), str(v)) for v in vs]
    if k == 'unsigned':
        vs = [0, 1, 2, 10, 100, 1000, 20000, 65535, 4294967295]
        return [(f'static_cast<{t}>({v}u)', num_id(v), str(v)) for v in vs]
    if k == 'enum':
        out = [(f'static_cast<{t}>({v})', short_enum(n), n) for n, v in enums[t]]
        bad = 0x78787878
        if all(v != bad for _, v in enums[t]):
            out.append((f'static_cast<{t}>({bad})', 'bad', 'not a valid value'))
        return out
    return []


def num_id(v):
    s = ('%g' % v).replace('-', 'm').replace('.', 'p').replace('+', '')
    return s


def short_enum(n):
    for p in ('bmdSwitcher', 'bmd'):
        if n.startswith(p):
            n = n[len(p):]
    return n[0].lower() + n[1:]


def jsfn(t, enums):
    """The recorder for a value of type t: integers as numbers."""
    k = kind(t, enums)
    return 'jsNum' if k in ('int', 'unsigned') else 'js'


def simple_out(p, enums):
    return p[0] == 'out' and p[1].endswith('*') and kind(p[1][:-1].strip(), enums) is not None


def main():
    enums, ifaces = parse(sys.argv[1])
    # Targets: methods not yet emulated (categories 2 and 3), plus every method
    # generated before (generated-methods.txt), so tests don't disappear once
    # the emulator implements a method and it moves to category 1.
    cat3 = set()
    for line in open(os.path.join(HERE, 'api-supported.tsv'), encoding='utf-8'):
        if line.startswith('#'):
            continue
        parts = line.rstrip('\n').split('\t')
        if len(parts) > 1 and parts[1] in ('2 samples', '3 sweep'):
            cat3.add(parts[0])
    frozen = os.path.join(HERE, 'generated-methods.txt')
    if os.path.exists(frozen):
        cat3 |= {l.strip() for l in open(frozen, encoding='utf-8') if '::' in l and not l.startswith('#')}
    covered = set()

    cb_classes = {}   # callback interface -> generated class
    body = []
    snaps = []
    count = 0

    def pairs_of(iface):
        methods = {m['name']: m for m in ifaces[iface] if m['params'] is not None}
        out = []
        for n, m in methods.items():
            if not n.startswith('Set') or m['ret'] != 'HRESULT' or len(m['params']) != 1 or m['params'][0][0] != 'in':
                continue
            t = m['params'][0][1]
            k = kind(t, enums)
            g = methods.get('Get' + n[3:])
            if not k or not g or len(g['params']) != 1 or g['params'][0] != ('out', t + '*', g['params'][0][2]):
                continue
            out.append((n[3:], t, k))
        return out

    # Every settable value of each target (and its children), so a test can put
    # back side effects too: e.g. a smaller chroma cursor moves the cursor, an
    # EQ reset resets its bands. Plain calls (not SDK_CALL): not coverage.
    CHILDREN = {
        'IBMDSwitcherFairlightAudioEqualizer': [
            '    Com<IBMDSwitcherFairlightAudioEqualizerBandIterator> it;',
            '    if (SUCCEEDED(o->CreateIterator(__uuidof(IBMDSwitcherFairlightAudioEqualizerBandIterator), reinterpret_cast<void**>(it.out()))) && it) {',
            '        IBMDSwitcherFairlightAudioEqualizerBand* band = nullptr;',
            '        while (it->Next(&band) == S_OK && band) { snap_IBMDSwitcherFairlightAudioEqualizerBand(band, s); band->Release(); band = nullptr; }',
            '    }'],
        'IBMDSwitcherFairlightAudioDynamicsProcessor': [
            '    { IBMDSwitcherFairlightAudioCompressor* p = nullptr; if (SUCCEEDED(o->GetProcessor(__uuidof(IBMDSwitcherFairlightAudioCompressor), reinterpret_cast<void**>(&p))) && p) { snap_IBMDSwitcherFairlightAudioCompressor(p, s); p->Release(); } }',
            '    { IBMDSwitcherFairlightAudioLimiter* p = nullptr; if (SUCCEEDED(o->GetProcessor(__uuidof(IBMDSwitcherFairlightAudioLimiter), reinterpret_cast<void**>(&p))) && p) { snap_IBMDSwitcherFairlightAudioLimiter(p, s); p->Release(); } }',
            '    { IBMDSwitcherFairlightAudioExpander* p = nullptr; if (SUCCEEDED(o->GetProcessor(__uuidof(IBMDSwitcherFairlightAudioExpander), reinterpret_cast<void**>(&p))) && p) { snap_IBMDSwitcherFairlightAudioExpander(p, s); p->Release(); } }'],
    }
    for iface in TARGETS:
        snaps.append(f'void snap_{iface}({iface}* o, Saved& s);')
    for iface, (prefix, _) in TARGETS.items():
        snaps.append(f'void snap_{iface}({iface}* o, Saved& s) {{')
        snaps.append('    o->AddRef();')
        snaps.append('    s.refs.push_back(o);')
        for prop, t, k in pairs_of(iface):
            if f'{iface}::Set{prop}' in OPT_IN:
                continue
            snaps.append(f'    {{ {t} v{{}}; if (SUCCEEDED(o->Get{prop}(&v))) s.values.push_back({{ "{prefix}.{prop}", '
                         f'[o, v] {{ {t} x{{}}; return SUCCEEDED(o->Get{prop}(&x)) && same(x, v); }}, '
                         f'[o, v] {{ return o->Set{prop}(v); }} }}); }}')
        snaps += CHILDREN.get(iface, [])
        snaps.append('}')
        snaps.append('')

    for iface, (prefix, keys) in TARGETS.items():
        methods = {m['name']: m for m in ifaces[iface] if m['params'] is not None}
        wanted = lambda n: f'{iface}::{n}' in cat3

        # Pairs Set<X>/Get<X> with one simple value.
        pairs = []
        for n, m in methods.items():
            if not n.startswith('Set') or m['ret'] != 'HRESULT' or len(m['params']) != 1 or m['params'][0][0] != 'in':
                continue
            t = m['params'][0][1]
            k = kind(t, enums)
            g = methods.get('Get' + n[3:])
            if not k or not g or len(g['params']) != 1 or g['params'][0] != ('out', t + '*', g['params'][0][2]):
                continue
            pairs.append((n[3:], t, k))

        for key in keys:
            tag = key.split('#')[1].lower() if '#' in key else ''
            base = f'g.{prefix}' + (f'.{tag}' if tag else '')
            obj = f'Com<{iface}> o(static_cast<{iface}*>(accessObject(c, "{key}"))); if (!o) c.skip("not reachable: {key}");'
            save = f'Saved saved; snap_{iface}(o.p, saved);' + (f' saveKeyFrameStored(c, saved, "{key}");' if '#' in key and 'KeyFrame' in iface else '')

            # State: every simple getter.
            getters = [m for n, m in methods.items()
                       if m['ret'] == 'HRESULT' and m['params'] and all(simple_out(p, enums) for p in m['params'])
                       and (n.startswith('Get') or n.startswith('Is') or n.startswith('Has') or n.startswith('Does') or n.startswith('Can'))]
            if any(wanted(m['name']) for m in getters):
                lines = [f'    addTest("{base}.state", "{iface}{" " + tag.upper() if tag else ""}: all getters", [](Ctx& c) {{',
                         f'        {obj}']
                for m in getters:
                    decls = ', '.join(f'&v{i}' for i in range(len(m['params'])))
                    lines.append('        {')
                    for i, p in enumerate(m['params']):
                        lines.append(f'            {p[1][:-1].strip()} v{i}{{}};')
                    lines.append(f'            HRESULT hr = SDK_CALL({iface}, o.p, {m["name"]}, {decls});')
                    if len(m['params']) == 1:
                        lines.append(f'            if (SUCCEEDED(hr)) c.observe("{m["name"]}", {jsfn(m["params"][0][1][:-1].strip(), enums)}(v0)); else c.observe("{m["name"]}", "error " + hrText(hr));')
                    else:
                        obs = ', '.join(f'{{ "{p[2]}", {jsfn(p[1][:-1].strip(), enums)}(v{i}) }}' for i, p in enumerate(m['params']))
                        lines.append(f'            if (SUCCEEDED(hr)) c.observe("{m["name"]}", QJsonObject{{ {obs} }}); else c.observe("{m["name"]}", "error " + hrText(hr));')
                    lines.append('        }')
                lines.append('    });')
                body += lines
                count += 1

            # Value pairs.
            for prop, t, k in pairs:
                if not (wanted('Set' + prop) or wanted('Get' + prop)):
                    continue
                opt = OPT_IN.get(f'{iface}::Set{prop}')
                for lit, vid, label in values(t, k, enums):
                    tid = f'{base}.{prop[0].lower() + prop[1:]}.{vid}'
                    body.append(f'    addTest("{tid}", "{iface}{" " + tag.upper() if tag else ""} {prop} = {label}", [](Ctx& c) {{')
                    if opt:
                        body.append(f'        if (!c.opt.{opt}) c.skip("opt-in: --{opt_flag(opt)}");')
                    body.append(f'        {obj}')
                    body.append(f'        {save}')
                    body.append(f'        probeRestore<{t}>(c, {lit},')
                    body.append(f'            [&]({t} x) {{ return SDK_CALL({iface}, o.p, Set{prop}, x); }},')
                    body.append(f'            [&]({t}* x) {{ return SDK_CALL({iface}, o.p, Get{prop}, x); }});')
                    body.append('        restoreAll(c, saved);')
                    body.append('    });')
                    covered.update({f'{iface}::Set{prop}', f'{iface}::Get{prop}'})
                    count += 1

            # Getters with one enum/int argument.
            for n, m in methods.items():
                ps = m['params']
                if not wanted(n) or m['ret'] != 'HRESULT' or not ps or len(ps) < 2 or ps[0][0] != 'in':
                    continue
                if not all(simple_out(p, enums) for p in ps[1:]):
                    continue
                t0 = ps[0][1]
                k0 = kind(t0, enums)
                if k0 not in ('enum', 'int', 'unsigned'):
                    continue
                args = values(t0, k0, enums) if k0 == 'enum' else [(f'static_cast<{t0}>({v})', str(v), str(v)) for v in (0, 1, 2)]
                for lit, vid, label in args:
                    body.append(f'    addTest("{base}.{n[0].lower() + n[1:]}.{vid}", "{iface} {n}({label})", [](Ctx& c) {{')
                    body.append(f'        {obj}')
                    for i, p in enumerate(ps[1:]):
                        body.append(f'        {p[1][:-1].strip()} v{i}{{}};')
                    outs = ', '.join(f'&v{i}' for i in range(len(ps) - 1))
                    body.append(f'        HRESULT hr = SDK_CALL({iface}, o.p, {n}, {lit}, {outs});')
                    body.append('        c.hr("call", hr);')
                    obs = ', '.join(f'{{ "{p[2]}", {jsfn(p[1][:-1].strip(), enums)}(v{i}) }}' for i, p in enumerate(ps[1:]))
                    body.append(f'        if (SUCCEEDED(hr)) c.observe("result", QJsonObject{{ {obs} }});')
                    body.append('    });')
                    count += 1

            # Argument-less actions: everything settable saved and put back.
            for n, m in methods.items():
                if not wanted(n) or m['ret'] != 'HRESULT' or m['params'] or n in ('AddCallback', 'RemoveCallback'):
                    continue
                body.append(f'    addTest("{base}.{n[0].lower() + n[1:]}", "{iface}{" " + tag.upper() if tag else ""} {n}() (settings put back after)", [](Ctx& c) {{')
                body.append(f'        {obj}')
                body.append(f'        {save}')
                body.append(f'        c.hr("call", SDK_CALL({iface}, o.p, {n}));')
                body.append('        c.settle();')
                for prop, t, k in pairs:
                    body.append(f'        {{ {t} v{{}}; if (SUCCEEDED(SDK_CALL({iface}, o.p, Get{prop}, &v))) c.observe("after.{prop}", {jsfn(t, enums)}(v)); }}')
                body.append('        restoreAll(c, saved);')
                body.append('    });')
                covered.add(f'{iface}::{n}')
                count += 1

            # Callbacks.
            add = methods.get('AddCallback')
            if add and wanted('AddCallback'):
                cb = add['params'][0][1].rstrip('*').strip()
                cb_classes[cb] = f'Gen{cb[len("IBMDSwitcher"):]}'
                body.append(f'    addTest("{base}.callbacks", "{iface}{" " + tag.upper() if tag else ""}: AddCallback, RemoveCallback", [](Ctx& c) {{')
                body.append(f'        {obj}')
                body.append(f'        auto* sink = new {cb_classes[cb]}(c.s.events, "{prefix}");')
                body.append(f'        c.hr("add", SDK_CALL({iface}, o.p, AddCallback, sink));')
                body.append('        c.settle();')
                body.append(f'        c.hr("remove", SDK_CALL({iface}, o.p, RemoveCallback, sink));')
                body.append('        sink->Release();')
                body.append('    });')
                count += 1

    # Callback sink classes: every method returns S_OK and logs its name.
    classes = []
    for cb, cls in sorted(cb_classes.items()):
        classes.append(f'class {cls} : public {cb} {{')
        classes.append('public:')
        classes.append(f'    {cls}(EventLog& log, const char* source) : m_log(log), m_source(source) {{}}')
        classes.append('    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppv) override {')
        classes.append('        if (!ppv) return E_POINTER;')
        classes.append(f'        if (iid == IID_IUnknown || iid == __uuidof({cb})) {{ *ppv = static_cast<{cb}*>(this); AddRef(); return S_OK; }}')
        classes.append('        *ppv = nullptr; return E_NOINTERFACE;')
        classes.append('    }')
        classes.append('    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }')
        classes.append('    ULONG STDMETHODCALLTYPE Release() override { ULONG n = --m_ref; if (!n) delete this; return n; }')
        for m in ifaces[cb]:
            if m['params'] is None:
                continue
            args = ', '.join(f'{p[1]} {p[2]}' for p in m['params'])
            classes.append(f'    HRESULT STDMETHODCALLTYPE {m["name"]}({args}) override {{ m_log.add(m_source, 0); return S_OK; }}')
        classes.append('private:')
        classes.append(f'    virtual ~{cls}() = default;')
        classes.append('    EventLog& m_log;')
        classes.append('    QString m_source;')
        classes.append('    std::atomic<ULONG> m_ref{ 1 };')
        classes.append('};')
        classes.append('')

    old = set()
    if os.path.exists(frozen):
        old = {l.strip() for l in open(frozen, encoding='utf-8') if '::' in l and not l.startswith('#')}
    with open(frozen, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# Methods gen_tests.py generates tests for (kept, so tests stay once emulated).\n')
        f.write('\n'.join(sorted(old | {m for m in covered if m in cat3})) + '\n')

    src = ['// GENERATED by coverage/gen_tests.py from BMDSwitcherAPI.idl — do not edit.',
           '// Tests for the category-3 SDK methods (see coverage/api-supported.tsv).',
           '#include "tests_common.h"',
           '#include "tests_access.h"',
           '',
           '#include <QJsonObject>',
           '#include <atomic>',
           '',
           'namespace {',
           ''] + classes + snaps + ['} // namespace', '', 'void registerGeneratedTests() {'] + body + ['}', '']
    open(OUT, 'w', encoding='utf-8', newline='\n').write('\n'.join(src))
    print(f'{count} tests, {len(cb_classes)} callback classes -> {os.path.relpath(OUT, HERE)}')


def opt_flag(opt):
    return re.sub(r'([A-Z])', lambda m: '-' + m.group(1).lower(), opt)


if __name__ == '__main__':
    main()
