#!/usr/bin/env python3
"""
layout_fingerprint.py - decide whether kern_igfx_hdmib.hpp's private-structure offsets (the `Layout` table) are still valid on
another build of Apple's AppleIntelICLLPGraphicsFramebuffer kext.

How it works: every function of the kext is disassembled and the set of memory displacements it uses ([reg+disp]) is recorded.
A structure field that moved shows up as a changed displacement in the functions that touch it. `diff` then compares a reference
dump (a kext where the offsets are verified, e.g. macOS 26) with a new one.

  1. On the Mac, extract the framebuffer kext from the kernel collection (kcx.pl, in this folder):
        perl kcx.pl /System/Library/KernelCollections/BootKernelExtensions.kc ~/kcx AppleIntelICLLPGraphicsFramebuffer
  2. dump:   python3 layout_fingerprint.py dump  ~/kcx/com.apple.driver.AppleIntelICLLPGraphicsFramebuffer  new.json  [../kern_igfx_hdmib.hpp]
  3. diff:   python3 layout_fingerprint.py diff  reference_darwin25.json  new.json  [../kern_igfx_hdmib.hpp]

Needs:  pip install capstone
Exit status of `diff`: 0 = no layout-relevant change in any function the module uses, 1 = changes or missing symbols (read the report).
"""
import json, re, struct, sys

try:
    import capstone
    from capstone import x86_const
except ImportError:
    sys.exit("capstone is required:  pip install capstone")

def parse_index(d):
    txt = open(d + '/index.txt').read()
    seg = re.search(r'SEGMENT __TEXT vmaddr=(0x[0-9a-f]+) vmsize=(0x[0-9a-f]+)', txt)
    sec = re.search(r'section __TEXT,__text addr=(0x[0-9a-f]+) size=(0x[0-9a-f]+)', txt)
    name = re.search(r'entry (\S+)', txt).group(1)
    return name, int(seg.group(1), 16), int(seg.group(2), 16), int(sec.group(1), 16), int(sec.group(2), 16)

def read_symbols(d, lo, hi):
    st = open(d + '/strtab.bin', 'rb').read()
    sy = open(d + '/symtab.bin', 'rb').read()
    out = {}
    for i in range(0, len(sy) - 15, 16):
        n, t, s, desc, v = struct.unpack('<IBBHQ', sy[i:i + 16])
        if (t & 0x0e) != 0x0e or not (lo <= v < hi) or n >= len(st):
            continue
        out.setdefault(st[n:st.index(b'\0', n)].decode('latin1'), v)
    return out

def mangled_from_header(path):
    s = open(path).read()
    return sorted(set(re.findall(r'"(__ZN?[0-9A-Za-z_]+)"', s)))

def layout_offsets(path):
    """all `uint32_t name = 0x...;` values of struct Layout"""
    s = open(path).read()
    m = re.search(r'struct Layout \{(.*?)\};', s, re.S)
    return {a: int(b, 16) for a, b in re.findall(r'uint32_t (\w+)\s*=\s*(0x[0-9a-f]+);', m.group(1))} if m else {}

def dump(d, out, header=None):
    name, tv, tsz, sa, ss = parse_index(d)
    text = open(d + '/__TEXT.bin', 'rb').read()
    syms = read_symbols(d, sa, sa + ss)
    addrs = sorted(set(syms.values())) + [sa + ss]
    nxt = {a: addrs[i + 1] for i, a in enumerate(addrs[:-1])}
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    md.skipdata = True
    res = {}
    for sym, a in syms.items():
        end = nxt[a]
        code = text[a - tv:end - tv]
        disps, calls = set(), 0
        for ins in md.disasm(code, a):
            if ins.id == 0:          # SKIPDATA pseudo-instruction (bytes that do not decode)
                continue
            if ins.mnemonic == 'call':
                calls += 1
            for op in ins.operands:
                if op.type == x86_const.X86_OP_MEM:
                    b = op.mem.base
                    if b in (x86_const.X86_REG_RIP, x86_const.X86_REG_RSP, x86_const.X86_REG_RBP, 0):
                        continue
                    if op.mem.disp >= 8:
                        disps.add(op.mem.disp)
        res[sym] = {'size': end - a, 'calls': calls, 'disps': sorted(disps)}
    meta = {'kext': name, 'functions': len(res)}
    if header:
        meta['module_symbols'] = mangled_from_header(header)
        meta['layout'] = layout_offsets(header)
    json.dump({'meta': meta, 'fn': res}, open(out, 'w'))
    print('%s: %d functions -> %s' % (name, len(res), out))

def diff(ref, new, header=None):
    r, n = json.load(open(ref)), json.load(open(new))
    rf, nf = r['fn'], n['fn']
    syms = mangled_from_header(header) if header else r['meta'].get('module_symbols', [])
    layout = layout_offsets(header) if header else r['meta'].get('layout', {})
    bad = 0
    common = set(rf) & set(nf)
    print('== 1) symbols the module routes/resolves must exist')
    for s in syms:
        if s not in nf:
            print('  MISSING in new kext: %s' % s); bad += 1
    print('   %d symbols checked' % len(syms))
    print('== 2) every Layout offset must be used by (nearly) the same functions in both kexts')
    print('   (a moved field shows up as a different set of functions touching that displacement)')
    for k, v in sorted(layout.items()):
        a = set(s for s in common if v in rf[s]['disps'])
        b = set(s for s in common if v in nf[s]['disps'])
        u = a | b
        ratio = (len(a & b) / len(u)) if u else 0.0
        status = 'ok' if u and ratio >= 0.8 else ('UNUSED' if not u else 'SUSPICIOUS')
        if status != 'ok': bad += 1
        print('   %-14s %#7x  ref=%3d new=%3d same=%3d  overlap=%.2f  %s' % (k, v, len(a), len(b), len(a & b), ratio, status))
        for s in sorted(a ^ b)[:6]:
            print('        %s %s' % ('-' if s in a else '+', s))
    print('== 3) information: module-hooked functions whose code differs (no Layout offset involved => normal between macOS releases)')
    key = set(layout.values()); note = 0
    for s in syms:
        if s in rf and s in nf and set(rf[s]['disps']) != set(nf[s]['disps']):
            ra, rb = set(rf[s]['disps']), set(nf[s]['disps'])
            inv = (ra ^ rb) & key
            note += 1
            print('   %s %s: -%s +%s (size %d -> %d)%s' % ('!!' if inv else '  ', s[:70], [hex(x) for x in sorted(ra - rb)], [hex(x) for x in sorted(rb - ra)], rf[s]['size'], nf[s]['size'], '  LAYOUT OFFSET INVOLVED' if inv else ''))
            if inv: bad += 1
    same = sum(1 for s in common if rf[s]['disps'] == nf[s]['disps'])
    print('== %d of %d common functions are byte-for-byte identical in the displacements they use' % (same, len(common)))
    if bad:
        print('RESULT: layout-relevant differences found (review above); do NOT mark this macOS version verified')
    else:
        print('RESULT: layout offsets consistent (%d hooked function(s) changed code without touching a Layout offset); run the hardware test checklist before trusting it' % note)
    return 1 if bad else 0

if __name__ == '__main__':
    a = sys.argv[1:]
    if len(a) >= 3 and a[0] == 'dump':
        dump(a[1], a[2], a[3] if len(a) > 3 else None)
    elif len(a) >= 3 and a[0] == 'diff':
        sys.exit(diff(a[1], a[2], a[3] if len(a) > 3 else None))
    else:
        print(__doc__)
