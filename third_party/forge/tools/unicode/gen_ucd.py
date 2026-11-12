#!/usr/bin/env python3
"""Generate os/src/text/ucd_data.inc from the Unicode Character Database (UCD) and DUCET.

Usage: gen_ucd.py <ucd-dir> <out.inc>
<ucd-dir> holds the flat UCD 16.0.0 files (UnicodeData.txt, LineBreak.txt, auxiliary/*, emoji-data.txt,
allkeys.txt ...).  The output is committed so builds need no network; rerun when bumping Unicode.
Layout: one two-stage trie (cp>>7 -> block, block -> record id) over packed property records, plus
sorted arrays for case mappings, decompositions, compositions, brackets/mirrors and the DUCET.
"""
import sys, os, re
D, OUT = sys.argv[1], sys.argv[2]
N = 0x110000

def rd(name):
    for p in (name, 'auxiliary/' + name, 'emoji/' + name, 'extracted/' + name):
        f = os.path.join(D, p)
        if os.path.exists(f): return open(f, encoding='utf-8')
    f = os.path.join(D, os.path.basename(name))
    return open(f, encoding='utf-8')

def ranges(name, field=1):
    for l in rd(name):
        l = l.split('#')[0].strip()
        if not l: continue
        p = [x.strip() for x in l.split(';')]
        r = p[0].split('..'); a = int(r[0], 16); b = int(r[-1], 16)
        yield a, b, p[field] if field < len(p) else None, p

GC = ['Cn','Lu','Ll','Lt','Lm','Lo','Mn','Mc','Me','Nd','Nl','No','Pc','Pd','Ps','Pe','Pi','Pf','Po','Sm','Sc','Sk','So','Zs','Zl','Zp','Cc','Cf','Cs','Co']
GCB = ['Other','CR','LF','Control','Extend','ZWJ','Regional_Indicator','Prepend','SpacingMark','L','V','T','LV','LVT']
WB = ['Other','CR','LF','Newline','Extend','ZWJ','Regional_Indicator','Format','Katakana','Hebrew_Letter','ALetter','Single_Quote','Double_Quote','MidNumLet','MidLetter','MidNum','Numeric','ExtendNumLet','WSegSpace']
SB = ['Other','CR','LF','Extend','Sep','Format','Sp','Lower','Upper','OLetter','Numeric','ATerm','SContinue','STerm','Close']
LB = ['XX','BK','CR','LF','CM','NL','SG','WJ','ZW','GL','SP','ZWJ','B2','BA','BB','HY','CB','CL','CP','EX','IN','NS','OP','QU','IS','NU','PO','PR','SY','AI','AL','CJ','EB','EM','H2','H3','HL','ID','JL','JV','JT','RI','SA','AK','AP','AS','VF','VI']
EAW = ['N','A','F','H','Na','W']
BIDI = ['L','R','AL','EN','ES','ET','AN','CS','NSM','BN','B','S','WS','ON','LRE','LRO','RLE','RLO','PDF','LRI','RLI','FSI','PDI']
INCB = ['None','Linker','Consonant','Extend']
FLAGS = ['Alphabetic','Lowercase','Uppercase','White_Space','Default_Ignorable_Code_Point','Cased','Case_Ignorable','Emoji','Emoji_Presentation','Extended_Pictographic','Emoji_Modifier','Emoji_Modifier_Base','Math','Hex_Digit','Dash','Ideographic','Noncharacter_Code_Point','Variation_Selector','Unified_Ideograph']

gc = [0]*N; ccc = [0]*N; name_first = None
ud = {}
for l in rd('UnicodeData.txt'):
    f = l.strip().split(';'); cp = int(f[0], 16); ud[cp] = f
    if f[1].endswith(', First>'): name_first = cp; continue
    if f[1].endswith(', Last>'):
        for c in range(name_first, cp + 1): gc[c] = GC.index(f[2]); ccc[c] = int(f[3])
        continue
    gc[cp] = GC.index(f[2]); ccc[cp] = int(f[3])
for a, b, v, _ in ranges('DerivedGeneralCategory.txt'):
    for c in range(a, b + 1): gc[c] = GC.index(v)

scripts = ['Unknown', 'Common', 'Inherited']; sc = [0]*N
for a, b, v, _ in ranges('Scripts.txt'):
    if v not in scripts: scripts.append(v)
    for c in range(a, b + 1): sc[c] = scripts.index(v)

def enum_prop(fname, names, default=0, field=1):
    arr = [default]*N
    for a, b, v, _ in ranges(fname, field):
        for c in range(a, b + 1): arr[c] = names.index(v)
    return arr
gcb = enum_prop('GraphemeBreakProperty.txt', GCB)
wb = enum_prop('WordBreakProperty.txt', WB)
sb = enum_prop('SentenceBreakProperty.txt', SB)
lb = enum_prop('LineBreak.txt', LB)
eaw = enum_prop('EastAsianWidth.txt', EAW)
bidi = enum_prop('DerivedBidiClass.txt', BIDI)
incb = [0]*N
flags = [0]*N
for fn in ('DerivedCoreProperties.txt', 'PropList.txt', 'emoji-data.txt'):
    for a, b, v, p in ranges(fn):
        if v == 'InCB':
            for c in range(a, b + 1): incb[c] = INCB.index(p[2])
        elif v in FLAGS:
            bit = 1 << FLAGS.index(v)
            for c in range(a, b + 1): flags[c] |= bit

# Records
recs = {}; rid = [0]*N; reclist = []
for c in range(N):
    r = (gc[c], sc[c], gcb[c], wb[c], sb[c], lb[c], eaw[c], bidi[c], ccc[c], incb[c], flags[c])
    i = recs.get(r)
    if i is None: i = recs[r] = len(reclist); reclist.append(r)
    rid[c] = i
assert len(reclist) < 65536
BS = 128
blocks = {}; blist = []; idx = []
for s in range(0, N, BS):
    t = tuple(rid[s:s + BS]); i = blocks.get(t)
    if i is None: i = blocks[t] = len(blist); blist.append(t)
    idx.append(i)

# Case mappings (simple)
up, lo, ti = {}, {}, {}
for cp, f in ud.items():
    if f[12]: up[cp] = int(f[12], 16)
    if f[13]: lo[cp] = int(f[13], 16)
    if f[14]: ti[cp] = int(f[14], 16)
# Special casing (unconditional only; conditional Final_Sigma handled in code, language ones in code)
sp_lo, sp_ti, sp_up = {}, {}, {}
for l in rd('SpecialCasing.txt'):
    l = l.split('#')[0].strip()
    if not l: continue
    p = [x.strip() for x in l.split(';')]
    if len(p) > 4 and p[4]: continue
    cp = int(p[0], 16)
    def seq(s): return [int(x, 16) for x in s.split()]
    sp_lo[cp] = seq(p[1]); sp_ti[cp] = seq(p[2]); sp_up[cp] = seq(p[3])
fold_s, fold_f = {}, {}
for l in rd('CaseFolding.txt'):
    l = l.split('#')[0].strip()
    if not l: continue
    p = [x.strip() for x in l.split(';')]
    cp = int(p[0], 16); m = [int(x, 16) for x in p[2].split()]
    if p[1] in ('C', 'S'): fold_s[cp] = m[0]
    if p[1] in ('C', 'F'): fold_f[cp] = m

# Decompositions
excl = set()
for a, b, v, _ in ranges('DerivedNormalizationProps.txt'):
    if v == 'Full_Composition_Exclusion':
        for c in range(a, b + 1): excl.add(c)
decomp = {}
for cp, f in ud.items():
    d = f[5]
    if not d: continue
    compat = d.startswith('<')
    if compat: d = d.split('>', 1)[1]
    decomp[cp] = (compat, [int(x, 16) for x in d.split()])
comp = {}
for cp, (compat, d) in decomp.items():
    if not compat and len(d) == 2 and cp not in excl: comp[(d[0], d[1])] = cp

# Brackets / mirroring
brk = {}
for a, b, v, p in ranges('BidiBrackets.txt'):
    brk[a] = (int(p[1], 16), 1 if p[2] == 'o' else 2)
mirror = {}
for a, b, v, p in ranges('BidiMirroring.txt'):
    mirror[a] = int(p[1], 16)

# DUCET
ducet = []; implicit = []
for l in open(os.path.join(D, 'allkeys.txt'), encoding='utf-8'):
    l = l.split('#')[0].strip()
    if l.startswith('@implicitweights'):
        m = re.match(r'@implicitweights\s+([0-9A-F]+)\.\.([0-9A-F]+);\s*([0-9A-F]+)', l)
        implicit.append((int(m[1], 16), int(m[2], 16), int(m[3], 16))); continue
    if not l or l.startswith('@'): continue
    k, v = l.split(';')
    ks = [int(x, 16) for x in k.split()]
    ces = []
    for var, p, s, t in re.findall(r'\[([.*])([0-9A-F]+)\.([0-9A-F]+)\.([0-9A-F]+)\]', v):
        ces.append(int(p, 16) << 15 | int(s, 16) << 6 | int(t, 16) << 1 | (1 if var == '*' else 0))
    ducet.append((ks, ces))
ducet.sort()

o = open(OUT, 'w')
w = o.write
w('// Generated by tools/unicode/gen_ucd.py from the Unicode 16.0.0 UCD + DUCET. Do not edit.\n')
w('// Unicode data files are (c) Unicode, Inc., used under the Unicode License v3 (see NOTICE).\n')
w('namespace forge::os::text::ucd {\n')
w('inline constexpr const char* unicode_version = "16.0.0";\n')
def arr(t, name, vals, per=24):
    w(f'inline constexpr {t} {name}[{len(vals)}] = {{\n')
    for i in range(0, len(vals), per): w(','.join(str(x) for x in vals[i:i + per]) + ',\n')
    w('};\n')
def names(name, lst):
    w(f'inline constexpr const char* {name}[{len(lst)}] = {{' + ','.join('"%s"' % x for x in lst) + '};\n')
names('gc_names', GC); names('script_names', scripts); names('gcb_names', GCB); names('wb_names', WB); names('sb_names', SB)
names('lb_names', LB); names('eaw_names', EAW); names('bidi_names', BIDI); names('flag_names', FLAGS)
w(f'inline constexpr int block_shift = 7;\n')
# record: gc u8, script u8, gcb u8 , wb u8, sb u8, lb u8, eaw u8, bidi u8, ccc u8, incb u8, flags u32
w('struct record { unsigned char gc, script, gcb, wb, sb, lb, eaw, bidi, ccc, incb; unsigned flags; };\n')
w(f'inline constexpr record records[{len(reclist)}] = {{\n')
for i in range(0, len(reclist), 8):
    w(''.join('{%s},' % ','.join(str(x) for x in r) for r in reclist[i:i + 8]) + '\n')
w('};\n')
arr('unsigned short', 'stage1', idx)
flat = [x for b in blist for x in b]
arr('unsigned short', 'stage2', flat, 32)
def pairs(name, d):
    ks = sorted(d); arr('unsigned', name + '_k', ks); arr('unsigned', name + '_v', [d[k] for k in ks])
pairs('upper', up); pairs('lower', lo); pairs('title', ti); pairs('fold', fold_s)
def multi(name, d):
    ks = sorted(d); off = []; pool = []
    for k in ks: off.append(len(pool) << 6 | len(d[k]) << 1); pool += d[k]
    arr('unsigned', name + '_k', ks); arr('unsigned', name + '_o', off); arr('unsigned', name + '_pool', pool or [0])
multi('sp_lower', sp_lo); multi('sp_title', sp_ti); multi('sp_upper', sp_up); multi('fold_full', fold_f)
# decomposition: off = pool<<5 | len<<1 | compat
ks = sorted(decomp); off = []; pool = []
for k in ks:
    c, d = decomp[k]; off.append(len(pool) << 6 | len(d) << 1 | (1 if c else 0)); pool += d
arr('unsigned', 'decomp_k', ks); arr('unsigned', 'decomp_o', off); arr('unsigned', 'decomp_pool', pool)
ck = sorted(comp)
arr('unsigned long long', 'comp_k', [a << 21 | b for a, b in ck], 12); arr('unsigned', 'comp_v', [comp[k] for k in ck])
ks = sorted(brk); arr('unsigned', 'bracket_k', ks); arr('unsigned', 'bracket_v', [brk[k][0] << 2 | brk[k][1] for k in ks])
pairs('mirror', mirror)
# DUCET: keys pool (sorted by key sequence), entry = keyoff<<2|keylen ; ce off<<5|celen
kp = []; ko = []; cp_ = []; co = []
for ks_, ces in ducet:
    ko.append(len(kp) << 2 | len(ks_)); kp += ks_
    co.append(len(cp_) << 5 | len(ces)); cp_ += ces
arr('unsigned', 'ducet_key_o', ko); arr('unsigned', 'ducet_key_pool', kp); arr('unsigned', 'ducet_ce_o', co); arr('unsigned', 'ducet_ce_pool', cp_, 16)
w('struct implicit_range { unsigned lo, hi, base; };\n')
w('inline constexpr implicit_range implicit_ranges[] = {' + ','.join('{%d,%d,%d}' % t for t in implicit) + '};\n')
w('}  // namespace forge::os::text::ucd\n')
o.close()
print('records', len(reclist), 'blocks', len(blist), 'ducet', len(ducet), 'size', os.path.getsize(OUT))
