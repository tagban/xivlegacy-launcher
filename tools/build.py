"""Build driver (clang, POSIX hosts): the translated game plus the portable runtime.

  python3 tools/build.py boot [--game <1.23b install>] [--run]   the headless boot test

The translation (generated/c, from crates/recomp) compiles once into build/gen/*.o and is reused;
only stale objects rebuild. Everything under generated/ and build/ is derived from Square Enix's
code: never commit it.
"""
import argparse
import concurrent.futures
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CFLAGS = ['-std=c11', '-g', '-DRT_GUEST_WINDOW', '-fno-strict-aliasing', '-I', 'runtime', '-I', 'runtime/portable']
GEN_FLAGS = ['-O2', '-I', 'generated/c', '-Wno-unused-label', '-Wno-unused-variable', '-Wno-unused-but-set-variable',
             '-Wno-unused-function', '-Wno-parentheses-equality', '-Wno-unreachable-code']
RUNTIME = ['runtime/runtime.c', 'runtime/portable/plat_posix.c', 'runtime/portable/gwin.c', 'runtime/portable/gthread.c',
           'runtime/portable/thunk.c', 'runtime/portable/pe.c', 'runtime/portable/k32.c', 'runtime/portable/vfs.c',
           'runtime/portable/kobj.c', 'runtime/portable/k32_io.c', 'runtime/portable/reg.c', 'runtime/portable/ole.c',
           'runtime/portable/k32_misc.c', 'runtime/portable/user32.c',
           'runtime/portable/input.c', 'runtime/portable/xiv_k32.c', 'runtime/portable/sqex_args.c', 'runtime/portable/xiv_misc.c', 'runtime/portable/dinput.c', 'runtime/portable/ws2.c', 'runtime/portable/d3d9.c', 'runtime/portable/d3dx9.c', 'runtime/portable/xiv_fiber.c', 'runtime/portable/xiv_net.c', 'runtime/portable/xaudio2.c']
BOOT_ONLY = []
SDL = '/opt/homebrew'  # SDL3 from Homebrew
CFLAGS += ['-I', SDL + '/include']
LIBS = ['-L' + SDL + '/lib', '-lSDL3', '-Wl,-rpath,' + SDL + '/lib', '-lm', '-lpthread']
if sys.platform == 'darwin':
    CFLAGS += ['-DRT_GUEST_FIXED_BASE=0x300000000ull']  # gwin.c reserves the guest window exactly there
    RUNTIME += ['runtime/portable/sm3.c', 'runtime/portable/sm3_msl.c', 'runtime/portable/sm3_interp.c', 'runtime/portable/gfx9_metal.m']
    LIBS += ['-framework', 'Metal', '-framework', 'QuartzCore', '-framework', 'Foundation', '-framework', 'CoreGraphics', '-framework', 'ImageIO']
if sys.platform.startswith('linux'):
    CFLAGS.append('-D_GNU_SOURCE')


def run(cmd):
    print('$ ' + ' '.join(cmd[:6]) + (' ...' if len(cmd) > 6 else ''), flush=True)
    subprocess.check_call(cmd, cwd=ROOT)


def newest_header(dirs):
    """the newest mtime of the headers in dirs (a header change rebuilds what includes them)"""
    t = 0
    for d in dirs:
        for f in os.listdir(os.path.join(ROOT, d)):
            if f.endswith('.h'):
                t = max(t, os.path.getmtime(os.path.join(ROOT, d, f)))
    return t


def compile_stale(sources, objdir, flags, header_dirs=()):
    os.makedirs(os.path.join(ROOT, objdir), exist_ok=True)
    jobs, objs = [], []
    headers = newest_header(header_dirs)
    for s in sources:
        obj = os.path.join(objdir, os.path.splitext(os.path.basename(s))[0] + '.o')
        objs.append(obj)
        src, dst = os.path.join(ROOT, s), os.path.join(ROOT, obj)
        if not os.path.exists(dst) or os.path.getmtime(dst) < max(os.path.getmtime(src), headers):
            extra = ['-x', 'objective-c', '-fno-objc-arc'] if s.endswith('.m') else []
            jobs.append(['clang', '-c'] + CFLAGS + flags + extra + [s, '-o', obj])
    if jobs:
        t = time.time()
        failed = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
            futures = {ex.submit(subprocess.call, cmd, cwd=ROOT): cmd for cmd in jobs}
            for n, fut in enumerate(concurrent.futures.as_completed(futures), 1):
                if fut.result():
                    failed.append(futures[fut][-3])
                print('\r%s: %d/%d' % (objdir, n, len(jobs)), end='', flush=True)
        print('  (%ds)' % (time.time() - t))
        if failed:
            raise SystemExit('failed: ' + ', '.join(failed))
    return objs


def generated():
    d = os.path.join(ROOT, 'generated', 'c')
    if not os.path.isdir(d):
        raise SystemExit('no generated/c: run the recompiler first (crates/recomp)')
    return ['generated/c/%s' % f for f in sorted(os.listdir(d)) if f.endswith('.c')]


def flags_changed(objdir, flags):
    """the compiler flags an object folder was built with: a change rebuilds everything in it"""
    stamp = os.path.join(ROOT, objdir, '.flags')
    now = ' '.join(CFLAGS + flags)
    if os.path.exists(stamp) and open(stamp).read() == now:
        return
    if os.path.isdir(os.path.join(ROOT, objdir)):
        for f in os.listdir(os.path.join(ROOT, objdir)):
            if f.endswith('.o'):
                os.remove(os.path.join(ROOT, objdir, f))
    os.makedirs(os.path.join(ROOT, objdir), exist_ok=True)
    open(stamp, 'w').write(now)


def boot(args):
    # the translation includes guest.h / runtime.h / sse.h only; the runtime, every header
    flags_changed('build/gen', GEN_FLAGS)
    flags_changed('build/obj/boot', ['-O1', '-Wall'])
    objs = compile_stale(generated(), 'build/gen', GEN_FLAGS)
    objs += compile_stale(RUNTIME + BOOT_ONLY + ['tests/boot.c'], 'build/obj/boot', ['-O1', '-Wall'],
                          ('runtime', 'runtime/portable'))
    run(['clang', '-o', 'build/boot'] + objs + LIBS)
    if args.run:
        if not args.game:
            raise SystemExit('--run needs --game <1.23b install>')
        run(['build/boot', args.game])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('target', choices=['boot'])
    ap.add_argument('--game')
    ap.add_argument('--run', action='store_true')
    args = ap.parse_args()
    {'boot': boot}[args.target](args)


if __name__ == '__main__':
    main()
