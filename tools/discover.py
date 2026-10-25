"""The discovery pass: Ghidra headless over ffxivgame.exe, to the per-build metadata.

  python3 tools/discover.py --game <1.23b install> [--reuse] [--ghidra <install dir>]

Copies the install's ffxivgame.exe into generated/images/<build>/, works in
generated/discovery/<build>/ffxivgame/ (a scratch Ghidra project and every step's report:
Square Enix code, gitignored), and writes meta/ffxivgame.<build>.meta.json (addresses and shapes
only, no game bytes).

Pipeline (discovery/, Jython post-scripts):

  import + auto-analysis                 RecompStats -> 00-auto.stats.txt
  NoReturn FIX                           clear non-returning verdicts that can reach a RET
  CrtDispatch                            functions at the CRT math dispatch tables' entries
  round 1 and 2:
    Discover   to a fixpoint             functions at orphaned pointer targets and referenced runs
    Discover2  to a fixpoint             decode never-touched pointer targets and aligned starts
    Switches, SwitchAudit                every jmp [reg*4+table] bounded from its guard, audited
    Conflicts                            decodes that overlap a real instruction
    BogusCheck APPLY                     apply the manual verdicts, list data decoded as code
  gate: conflicts 0, suspect_functions 0, SwitchAudit review 0 -- else stop (exit 2) with the
        reports named: read each site, add verdicts to discovery/verdicts.py, rerun with --reuse
  Classify, Diagnose, RecompStats        coverage reports
  Export                                 the metadata

Ghidra 11.3.x (Jython bundled) and JDK 21: --ghidra, else GHIDRA_INSTALL_DIR, else
/Volumes/AppStorage/tools/ghidra_*_PUBLIC or ~/ghidra*_PUBLIC.
"""
import argparse
import glob
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPTS = os.path.join(ROOT, 'discovery')
METADIR = os.path.join(ROOT, 'meta')
PROJECT = 'discovery'
MAX_ROUNDS = 12
MODULE = 'ffxivgame.exe'

sys.path.insert(0, SCRIPTS)
import verdicts  # noqa: E402


def find_ghidra(arg):
    cands = [arg, os.environ.get('GHIDRA_INSTALL_DIR')]
    cands += sorted(glob.glob('/Volumes/AppStorage/tools/ghidra_*_PUBLIC'), reverse=True)
    cands += sorted(glob.glob(os.path.expanduser('~/ghidra*_PUBLIC')), reverse=True)
    for c in cands:
        if not c:
            continue
        exe = os.path.join(c, 'support', 'analyzeHeadless.bat' if os.name == 'nt' else 'analyzeHeadless')
        if os.path.exists(exe):
            return exe
    raise SystemExit('Ghidra 11.3.x not found: pass --ghidra <install dir> or set GHIDRA_INSTALL_DIR')


def pe_timestamp(path):
    with open(path, 'rb') as f:
        head = f.read(0x400)
    lfanew = struct.unpack_from('<I', head, 0x3c)[0]
    return struct.unpack_from('<I', head, lfanew + 8)[0]


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


class Run:
    def __init__(self, headless, work, image):
        self.headless, self.work, self.image = headless, work, image
        self.name = os.path.basename(image)
        self.step = 0

    def report(self, name):
        return os.path.join(self.work, name)

    def ghidra(self, scripts, imported=False):
        """One headless invocation running scripts [(script, [args])] in order; returns the log."""
        self.step += 1
        tag = '%02d-%s' % (self.step, '+'.join(s[0].split('.')[0] for s in scripts))
        cmd = [self.headless, self.work, PROJECT]
        cmd += ['-import', self.image, '-overwrite'] if imported else ['-process', self.name, '-noanalysis']
        cmd += ['-scriptPath', SCRIPTS]
        for script, args in scripts:
            cmd += ['-postScript', script] + args
        log = os.path.join(self.work, tag + '.log')
        t = time.time()
        print('> %s' % tag, end='', flush=True)
        with open(log, 'w') as f:
            rc = subprocess.call(cmd, stdout=f, stderr=subprocess.STDOUT)
        print('  (%ds)' % (time.time() - t))
        text = open(log, errors='replace').read()
        # A failed post-script does not fail analyzeHeadless: look for the traceback.
        if rc or 'Traceback (most recent call last)' in text or 'SCRIPT ERROR' in text.upper():
            raise SystemExit('ghidra step %s failed (rc %d); see %s' % (tag, rc, log))
        return text

    def fixpoint(self, script, created):
        """Run script until created(report text) is 0."""
        for _ in range(MAX_ROUNDS):
            out = self.report('%s.%d.txt' % (script.split('.')[0], self.step + 1))
            self.ghidra([(script, [out])])
            text = open(out).read()
            n = created(text)
            print('    %s' % text.splitlines()[0])
            if n == 0:
                return
        raise SystemExit('%s did not reach a fixpoint in %d rounds' % (script, MAX_ROUNDS))

    def first_int(self, path, key):
        m = re.search(r'%s\s+(\d+)' % key, open(path).read())
        return int(m.group(1)) if m else None


def discover_created(text):
    return int(re.search(r'created (\d+)', text).group(1))


def discover2_created(text):
    m = re.search(r"created \{([^}]*)\}", text)
    return sum(int(x) for x in re.findall(r':\s*(\d+)', m.group(1)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', required=True, help='a 1.23b install (holds ffxivgame.exe)')
    ap.add_argument('--out', help='default: meta/ffxivgame.<build>.meta.json')
    ap.add_argument('--reuse', action='store_true', help='keep the analysed project; skip import + auto-analysis')
    ap.add_argument('--ghidra', help='the Ghidra install folder')
    ap.add_argument('--export-anyway', action='store_true', help='export even if the gate fails')
    args = ap.parse_args()

    src = os.path.join(args.game, MODULE)
    if not os.path.exists(src):
        raise SystemExit('%s not found' % src)
    ts = pe_timestamp(src)
    if ts not in verdicts.BUILDS:
        raise SystemExit('discovery/verdicts.py does not know PE timestamp 0x%08x' % ts)
    build = verdicts.BUILDS[ts]
    images = os.path.join(ROOT, 'generated', 'images', build)
    os.makedirs(images, exist_ok=True)
    image = os.path.join(images, MODULE)
    if not os.path.exists(image) or sha256(image) != sha256(src):
        shutil.copyfile(src, image)
    out = os.path.abspath(args.out or os.path.join(METADIR, 'ffxivgame.%s.meta.json' % build))
    work = os.path.join(ROOT, 'generated', 'discovery', build, 'ffxivgame')
    os.makedirs(work, exist_ok=True)
    run = Run(find_ghidra(args.ghidra), work, image)
    have_project = os.path.exists(os.path.join(work, PROJECT + '.gpr'))

    if not (args.reuse and have_project):
        print('import + auto-analysis (the slow step: tens of minutes for ffxivgame.exe)')
        run.ghidra([('RecompStats.py', [run.report('00-auto.stats.txt')])], imported=True)

    # Ghidra's non-returning verdicts cascade and cut callers short (free() was one): clear the
    # ones that can reach a RET, and decode what follows their calls.
    print('non-returning functions')
    nr = run.report('NoReturn.txt')
    run.ghidra([('NoReturn.py', [nr, 'FIX'])])
    print('    ' + open(nr).read().splitlines()[0])

    # the CRT's x87 math dispatch tables: unaligned, so DataPointers cannot see them
    print('CRT math dispatch')
    cd = run.report('CrtDispatch.txt')
    run.ghidra([('CrtDispatch.py', [cd])])
    print('    ' + open(cd).read().splitlines()[0])

    gate = {}
    for rnd in (1, 2):
        print('round %d' % rnd)
        run.fixpoint('Discover.py', discover_created)
        run.fixpoint('Discover2.py', discover2_created)
        dp = run.report('DataPointers.r%d.txt' % rnd)
        run.ghidra([('DataPointers.py', [dp])])
        print('    ' + open(dp).read().splitlines()[0])
        r = {k: run.report('%s.r%d.txt' % (k, rnd)) for k in ('Switches', 'SwitchAudit', 'Conflicts', 'BogusCheck')}
        run.ghidra([('Switches.py', [r['Switches']]), ('SwitchAudit.py', [r['SwitchAudit']]),
                    ('Conflicts.py', [r['Conflicts']]), ('BogusCheck.py', [r['BogusCheck'], 'APPLY'])])
        gate = {
            'conflicts': (run.first_int(r['Conflicts'], 'conflicts'), r['Conflicts']),
            'suspect_functions': (run.first_int(r['BogusCheck'], 'suspect_functions'), r['BogusCheck']),
            'switch review': (run.first_int(r['SwitchAudit'], 'review'), r['SwitchAudit']),
        }
        print('    ' + ', '.join('%s %s' % (k, v[0]) for k, v in gate.items()))

    bad = {k: v for k, v in gate.items() if v[0] != 0}
    if bad and not args.export_anyway:
        print('\nGATE FAILED; nothing exported. Read these, record verdicts in discovery/verdicts.py, '
              'then rerun with --reuse:')
        for k, (n, path) in bad.items():
            print('  %s %s: %s' % (k, n, path))
        sys.exit(2)

    print('reports')
    run.ghidra([('Classify.py', [run.report('Classify.txt')]), ('Diagnose.py', [run.report('Diagnose.txt')]),
                ('RecompStats.py', [run.report('RecompStats.txt')])])
    print('export')
    log = run.ghidra([('Export.py', [out, sha256(image), build, MODULE])])
    m = re.search(r'functions \d+ switches \d+ tail_jumps \d+', log)
    print('wrote %s: %s' % (out, m.group(0) if m else '?'))
    print('reports in %s' % work)


if __name__ == '__main__':
    main()
