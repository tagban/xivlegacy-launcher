"""Build driver (clang, POSIX hosts): the translated game plus the portable runtime.

  python3 tools/build.py boot [--game <1.23b install>] [--run]   the game and runtime in one program
  python3 tools/build.py host                                     the runtime alone (XIVLEGACY_SPLIT)
  python3 tools/build.py module [--out <file>]                    the translation as a game module

The host carries the runtime, graphics, sound, input and networking, with none of the game's code,
so it can ship; the module is the translation of one player's ffxivgame.exe, made on their machine
(runtime/xiv_game.h). CC picks the module's compiler (default clang; "zig cc" works too).

The graphics back end under Direct3D 9 (runtime/portable/gfx9.h) is XIVLEGACY_GFX: "metal"
(gfx9_metal.m; the default on macOS) or "wgpu" (gfx9_wgpu.c on wgpu-native: Direct3D 12 on Windows,
Vulkan on Linux, Metal on macOS; the only one off macOS). wgpu-native is found at WGPU_NATIVE, else
`brew --prefix wgpu-native`. Each back end keeps its own runtime objects (build/obj/<target>-<gfx>).

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
           'runtime/portable/k32_misc.c', 'runtime/portable/user32.c', 'runtime/portable/bug_report.c',
           'runtime/portable/input.c', 'runtime/portable/xiv_k32.c', 'runtime/portable/sqex_args.c', 'runtime/portable/xiv_misc.c', 'runtime/portable/dinput.c', 'runtime/portable/ws2.c', 'runtime/portable/d3d9.c', 'runtime/portable/d3dx9.c', 'runtime/portable/xiv_fiber.c', 'runtime/portable/xiv_net.c', 'runtime/portable/xaudio2.c', 'runtime/portable/xiv_boot.c', 'runtime/portable/xiv_icon.c', 'runtime/portable/xiv_logtabs.c', 'runtime/portable/xiv_notice.c']
BOOT_ONLY = []
SDL = '/opt/homebrew'  # SDL3 from Homebrew
# For a release, SDL's own macOS framework (SDL3-<v>.dmg: SDL3.xcframework/macos-arm64_x86_64, the folder
# holding SDL3.framework) instead: Homebrew's is built for the Mac it was installed on, not older macOS.
SDL_FRAMEWORK = os.environ.get('XIVLEGACY_SDL3_FRAMEWORK') if sys.platform == 'darwin' else None
if SDL_FRAMEWORK:
    CFLAGS += ['-F', SDL_FRAMEWORK]
    LIBS = ['-F', SDL_FRAMEWORK, '-framework', 'SDL3', '-Wl,-rpath,@executable_path/../Frameworks', '-lm', '-lpthread']
else:
    CFLAGS += ['-I', SDL + '/include']
    LIBS = ['-L' + SDL + '/lib', '-lSDL3', '-Wl,-rpath,' + SDL + '/lib', '-lm', '-lpthread']
if sys.platform == 'darwin':
    CFLAGS += ['-DRT_GUEST_FIXED_BASE=0x10000000000ull']  # gwin.c reserves the guest window exactly there.
    # 1 TB: macOS 27 moves any requested address below 0x7000000000 (its heap start) up to there, so
    # the old 0x300000000 (now inside the larger dyld shared cache, too) fails; above it requests hold
    MACOS_MIN = os.environ.get('XIVLEGACY_MACOS_MIN', '13.0')  # the oldest macOS the host runs on
    CFLAGS += ['-mmacosx-version-min=' + MACOS_MIN]
    LIBS += ['-mmacosx-version-min=' + MACOS_MIN]

# the graphics back end (see the top): Metal by default on macOS, wgpu everywhere else
GFX = os.environ.get('XIVLEGACY_GFX') or ('metal' if sys.platform == 'darwin' else 'wgpu')
RUNTIME_FLAGS = []  # the runtime's own compiler flags (not the translation's: build/gen stays as built)
if GFX == 'metal':
    if sys.platform != 'darwin':
        raise SystemExit('XIVLEGACY_GFX=metal is macOS only: use wgpu')
    RUNTIME += ['runtime/portable/sm3.c', 'runtime/portable/sm3_msl.c', 'runtime/portable/sm3_interp.c', 'runtime/portable/gfx9_metal.m']
    LIBS += ['-framework', 'Metal', '-framework', 'QuartzCore', '-framework', 'Foundation', '-framework', 'CoreGraphics', '-framework', 'ImageIO']
elif GFX == 'wgpu':
    WGPU = os.environ.get('WGPU_NATIVE')
    if not WGPU:
        try:
            WGPU = subprocess.run(['brew', '--prefix', 'wgpu-native'], capture_output=True, text=True).stdout.strip()
        except OSError:
            WGPU = ''
    WGPU = WGPU or '/opt/homebrew/opt/wgpu-native'
    RUNTIME_FLAGS += ['-I', WGPU + '/include']
    RUNTIME += ['runtime/portable/sm3.c', 'runtime/portable/sm3_wgsl.c', 'runtime/portable/gfx9_wgpu.c']
    LIBS += ['-L' + WGPU + '/lib', '-lwgpu_native', '-Wl,-rpath,' + WGPU + '/lib']
else:
    raise SystemExit('XIVLEGACY_GFX=%s: metal or wgpu' % GFX)
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


def compile_stale(sources, objdir, flags, header_dirs=(), cc=None):
    os.makedirs(os.path.join(ROOT, objdir), exist_ok=True)
    jobs, objs = [], []
    headers = newest_header(header_dirs)
    for s in sources:
        obj = os.path.join(objdir, os.path.splitext(os.path.basename(s))[0] + '.o')
        objs.append(obj)
        src, dst = os.path.join(ROOT, s), os.path.join(ROOT, obj)
        if not os.path.exists(dst) or os.path.getmtime(dst) < max(os.path.getmtime(src), headers):
            extra = ['-x', 'objective-c', '-fno-objc-arc'] if s.endswith('.m') else []
            jobs.append((cc or ['clang']) + ['-c'] + CFLAGS + flags + extra + [s, '-o', obj])
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


def runtime_objdir(target):
    """the runtime's objects per target and back end (Metal keeps the folders it always had)"""
    return 'build/obj/' + target + ('' if GFX == 'metal' else '-' + GFX)


def boot(args):
    # the translation includes guest.h / runtime.h / sse.h only; the runtime, every header
    flags_changed('build/gen', GEN_FLAGS)
    objdir, flags = runtime_objdir('boot'), ['-O1', '-Wall'] + RUNTIME_FLAGS
    flags_changed(objdir, flags)
    objs = compile_stale(generated(), 'build/gen', GEN_FLAGS)
    objs += compile_stale(RUNTIME + BOOT_ONLY + ['tests/boot.c'], objdir, flags, ('runtime', 'runtime/portable'))
    run(['clang', '-o', 'build/boot'] + objs + LIBS)
    if args.run:
        if not args.game:
            raise SystemExit('--run needs --game <1.23b install>')
        run(['build/boot', args.game])


MODULE_EXT = '.dylib' if sys.platform == 'darwin' else '.dll' if sys.platform == 'win32' else '.so'

# Windows x64, cross-built with zig (`--windows`): the same runtime with plat_win.c and the Windows
# start (host_win.c). SDL3 is the official MinGW development package (SDL3-devel-<v>-mingw.tar.gz)
# unpacked at XIVLEGACY_SDL3_MINGW (its x86_64-w64-mingw32 folder).
WIN_CC = ['zig', 'cc', '-target', 'x86_64-windows-gnu']
WIN_SDL = os.environ.get('XIVLEGACY_SDL3_MINGW', '/Volumes/AppStorage/ffxiv-scratch/deps/sdl3-mingw/SDL3-3.4.16/x86_64-w64-mingw32')
WIN_CFLAGS = ['-std=c11', '-g', '-DRT_GUEST_WINDOW', '-fno-strict-aliasing', '-I', 'runtime', '-I', 'runtime/portable',
              '-I', WIN_SDL + '/include']
WIN_RUNTIME = [s for s in RUNTIME if s != 'runtime/portable/plat_posix.c' and not s.endswith(('.m', 'sm3_msl.c'))] + [
    'runtime/portable/plat_win.c', 'runtime/portable/host_win.c']
# wgpu-native's MinGW release (wgpu-windows-x86_64-gnu-release.zip) unpacked at XIVLEGACY_WGPU_WIN
WIN_WGPU = os.environ.get('XIVLEGACY_WGPU_WIN', '/Volumes/AppStorage/ffxiv-scratch/deps/wgpu-win')
WIN_GFX = ['runtime/portable/gfx9_wgpu.c', 'runtime/portable/sm3_wgsl.c']
if os.environ.get('XIVLEGACY_GFX') == 'null':
    WIN_GFX = ['runtime/portable/gfx9_null.c']
WIN_LIBS = [WIN_SDL + '/lib/libSDL3.dll.a', WIN_WGPU + '/lib/libwgpu_native.dll.a', '-lws2_32', '-lole32', '-loleaut32', '-luser32', '-lgdi32', '-lwinmm',
            '-ladvapi32', '-lshell32', '-lsynchronization']


def win_compile(sources, objdir, flags):
    """compile_stale for the Windows cross build (its own objects and flags)."""
    os.makedirs(os.path.join(ROOT, objdir), exist_ok=True)
    objs, jobs = [], []
    for src in sources:
        obj = os.path.join(objdir, os.path.splitext(os.path.basename(src))[0] + '.o')
        objs.append(obj)
        a, b = os.path.join(ROOT, src), os.path.join(ROOT, obj)
        if not os.path.exists(b) or os.path.getmtime(b) < max(os.path.getmtime(a), newest_header(('runtime', 'runtime/portable'))):
            jobs.append(WIN_CC + ['-c'] + flags + [src, '-o', obj])
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        bad = [j[-3] for j, r in zip(jobs, ex.map(lambda j: subprocess.call(j, cwd=ROOT), jobs)) if r]
    if bad:
        raise SystemExit('failed: ' + ', '.join(bad))
    return objs


def host(args):
    """build/xivlegacy-host: the runtime without the translation; it loads the game with --module."""
    if args.windows:
        # unknown attributes are errors: MinGW compilers ignore an unknown __declspec, thread among them
        objs = win_compile(WIN_RUNTIME + WIN_GFX + ['runtime/portable/xiv_load.c'], 'build/win/obj',
                           WIN_CFLAGS + ['-O1', '-DXIVLEGACY_SPLIT', '-Wno-everything', '-Werror=unknown-attributes', '-I', WIN_WGPU + '/include',
                                         '-I', WIN_WGPU + '/include/webgpu'])
        run(WIN_CC + ['-o', 'build/win/xivlegacy-host.exe'] + objs + WIN_LIBS)
        import shutil
        shutil.copy(os.path.join(WIN_SDL, 'bin', 'SDL3.dll'), os.path.join(ROOT, 'build', 'win', 'SDL3.dll'))
        shutil.copy(os.path.join(WIN_WGPU, 'lib', 'wgpu_native.dll'), os.path.join(ROOT, 'build', 'win', 'wgpu_native.dll'))
        print('built build/win/xivlegacy-host.exe (with SDL3.dll and wgpu_native.dll beside it)')
        return
    objdir, flags = runtime_objdir('host'), ['-O1', '-Wall', '-DXIVLEGACY_SPLIT'] + RUNTIME_FLAGS
    flags_changed(objdir, flags)
    objs = compile_stale(RUNTIME + ['runtime/portable/xiv_load.c', 'tests/boot.c'], objdir, flags,
                         ('runtime', 'runtime/portable'))
    run(['clang', '-o', 'build/xivlegacy-host'] + objs + LIBS)
    print('built build/xivlegacy-host; run: build/xivlegacy-host --module build/xivlegacy-game%s <1.23b install>'
          % MODULE_EXT)


def module(args):
    """build/xivlegacy-game.<ext>: the translation (generated/c) as a shared library (xiv_module.c)."""
    import shlex
    if args.windows:
        objs = win_compile(generated() + ['runtime/xiv_module.c'], 'build/win/mod',
                           [f for f in WIN_CFLAGS if f != '-g'] + GEN_FLAGS + ['-w'])
        out = args.out or os.path.join('build', 'win', 'xivlegacy-game.dll')
        rsp = os.path.join(ROOT, 'build', 'win', 'mod.rsp')
        open(rsp, 'w').write('\n'.join('"%s"' % o for o in objs))
        run(WIN_CC + ['-shared', '-o', out, '@' + rsp])
        print('built ' + out)
        return
    cc = shlex.split(os.environ.get('CC', 'clang'))
    flags = GEN_FLAGS + ['-fPIC', '-fvisibility=hidden']
    flags_changed('build/mod', flags)
    objs = compile_stale(generated() + ['runtime/xiv_module.c'], 'build/mod', flags, cc=cc)
    out = args.out or os.path.join('build', 'xivlegacy-game' + MODULE_EXT)
    if sys.platform == 'darwin':
        link = ['-dynamiclib', '-o', out] + objs + ['-Wl,-install_name,@rpath/' + os.path.basename(out)]
    else:
        link = ['-shared', '-o', out] + objs + ['-lm']
    run(cc + link)
    print('built ' + out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('target', choices=['boot', 'host', 'module'])
    ap.add_argument('--game')
    ap.add_argument('--run', action='store_true')
    ap.add_argument('--out')
    ap.add_argument('--windows', action='store_true', help='cross-build for Windows x64 with zig')
    args = ap.parse_args()
    {'boot': boot, 'host': host, 'module': module}[args.target](args)


if __name__ == '__main__':
    main()
