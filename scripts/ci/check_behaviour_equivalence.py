#!/usr/bin/env python3
"""BEQ-01 -- the C this decompiler emits has to behave like the binary.

Each program in tests/algorithm_recovery/beq/ is compiled at -O0, -O2 and -O3
and decompiled.  The emitted `.buildable.c` is then linked, with its own
`main` renamed away, into a copy of the program built with -DBEQ_DRIVER, in
which the functions under test (`BEQ_ENTRY`) are weak: the decompiled
definitions replace them, while inputs and output stay in the original C.
Both programs run, and stdout and exit status are compared.  So what is
checked is the recovered code of those functions -- control flow above all,
which is what structuring changes -- and not how the decompiler carries
global data or variadic calls, which whole programs would mostly trip over.

With --corpus, prebuilt binaries are checked whole instead: the emitted
program, with a `main` calling what `_start` hands to `__libc_start_main` if
symbols are gone, against the binary.  Few survive that (0/126 when this was
written).

"Equivalent" is not asked of every binary.  The set that is equivalent is
recorded in a baseline file; the check fails when a binary in the baseline
stops being equivalent.  `--update-baseline` rewrites it from the current run.
"""
import argparse, json, os, re, subprocess, sys, tempfile, concurrent.futures as cf

def run(cmd, timeout, **kw):
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=timeout, **kw)
        return p.returncode, p.stdout
    except subprocess.TimeoutExpired:
        return 'timeout', b''
    except OSError as e:
        return 'oserror', str(e).encode()

def check(binary, dec, cc, work, driver_src=None, dec_args=()):
    name = os.path.basename(binary)
    d = os.path.join(work, name)
    os.makedirs(d, exist_ok=True)
    out_c = os.path.join(d, name + '.c')
    rc, _ = run([dec, binary, '-o', out_c] + list(dec_args), 600)
    if rc != 0:
        return name, 'decompile-failed'
    buildable = os.path.join(d, name + '.buildable.c')
    src_path = buildable if os.path.exists(buildable) else out_c
    src = open(src_path, errors='replace').read()
    exe = os.path.join(d, name + '.beq')
    # With symbols, the C runtime's own functions come back by name and
    # collide with the ones the toolchain links in.
    crt = ['-D%s=retdec_crt%s' % (n, n) for n in (
        '_init', '_fini', '_start', 'deregister_tm_clones', 'register_tm_clones',
        '__do_global_dtors_aux', 'frame_dummy', '__libc_start_main', '__cxa_finalize',
        '__gmon_start__')]
    harness = os.path.join(d, name + '.beq.c')
    if driver_src:
        open(harness, 'w').write(src)
        obj = os.path.join(d, name + '.beq.o')
        rc, _ = run([cc, '-c', '-w', '-fpermissive', '-O0', '-Dmain=retdec_main'] + crt
                    + ['-I', d, harness, '-o', obj], 120)
        if rc != 0:
            return name, 'compile-failed'
        rc, _ = run([cc, '-w', '-O0', '-DBEQ_DRIVER', driver_src, obj, '-o', exe], 120)
        if rc != 0:
            return name, 'link-failed'
    else:
        if re.search(r'^int main\(', src, flags=re.M) and not re.search(
                r'^int main\(void\) \{ return 0; \}$', src, flags=re.M):
            pass  # symbols survived: the program's own main is there
        else:
            m = re.search(r'__libc_start_main\(0x([0-9a-f]+)', src)
            if not m or not re.search(r'\bfunction_%s\(' % m.group(1), src):
                return name, 'no-main'
            src = re.sub(r'^int main\(void\) \{ return 0; \}$',
                         'static int retdec_unused_main(void) { return 0; }', src, flags=re.M)
            src += '\nint main(void) { return (int)function_%s(); }\n' % m.group(1)
        open(harness, 'w').write(src)
        rc, _ = run([cc, '-w', '-fpermissive', '-O0'] + crt + ['-I', d, harness, '-o', exe], 120)
        if rc != 0:
            return name, 'compile-failed'
    orc, oout = run([os.path.abspath(binary)], 10)
    drc, dout = run([exe], 10)
    if orc != drc or oout != dout:
        return name, 'differs'
    return name, 'equivalent'

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--decompiler', required=True)
    ap.add_argument('--corpus', default=None,
                    help='prebuilt binaries instead of compiling --sources')
    ap.add_argument('--sources', default='tests/algorithm_recovery/beq')
    ap.add_argument('--opts', default='O0,O2,O3')
    ap.add_argument('--baseline', default='tests/algorithm_recovery/beq_baseline.json')
    ap.add_argument('--cc', default=os.environ.get('CC', 'gcc'))
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--update-baseline', action='store_true')
    ap.add_argument('--verbose', action='store_true')
    ap.add_argument('--decompiler-arg', action='append', default=[],
                    help='extra decompiler argument (repeatable)')
    a = ap.parse_args()
    work = tempfile.mkdtemp(prefix='beq-')
    print('BEQ-01: work directory ' + work)
    if a.corpus:
        jobs = [(os.path.join(a.corpus, f), None) for f in sorted(os.listdir(a.corpus))
                if '.' not in f and os.access(os.path.join(a.corpus, f), os.X_OK)]
    else:
        jobs = []
        bindir = os.path.join(work, 'bin')
        os.makedirs(bindir)
        for f in sorted(os.listdir(a.sources)):
            if not f.endswith('.c'): continue
            for o in a.opts.split(','):
                b = os.path.join(bindir, '%s-gcc-%s' % (f[:-2], o))
                rc, _ = run([a.cc, '-' + o, '-w', os.path.join(a.sources, f), '-o', b], 120)
                if rc == 0: jobs.append((b, os.path.abspath(os.path.join(a.sources, f))))
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        res = dict(ex.map(lambda j: check(j[0], a.decompiler, a.cc, work, j[1], a.decompiler_arg), jobs))
    eq = sorted(n for n, r in res.items() if r == 'equivalent')
    counts = {}
    for r in res.values(): counts[r] = counts.get(r, 0) + 1
    if a.verbose:
        for n in sorted(res): print('  %-28s %s' % (n, res[n]))
    print('BEQ-01: %d/%d equivalent %s' % (len(eq), len(res), json.dumps(counts, sort_keys=True)))
    if a.update_baseline:
        json.dump({'equivalent': eq}, open(a.baseline, 'w'), indent=1)
        open(a.baseline, 'a').write('\n')
        return 0
    base = json.load(open(a.baseline))['equivalent'] if os.path.exists(a.baseline) else []
    lost = [n for n in base if res.get(n) != 'equivalent']
    gained = [n for n in eq if n not in base]
    if gained: print('BEQ-01: newly equivalent: ' + ' '.join(gained))
    if lost:
        print('BEQ-01 FAIL: no longer equivalent: ' + ' '.join('%s(%s)' % (n, res.get(n)) for n in lost))
        return 1
    print('BEQ-01 OK')
    return 0

if __name__ == '__main__':
    sys.exit(main())
