"""The installer's steps (docs/installer.md): check, extract, fonts, lift, capture, lift-nro, shaders, build, dlss, finish.

Install folder:
  mtf-recomp.exe and its DLLs        the game, built from the player's files on this machine
  game/updates/327680/exefs, romfs   the 1.1.1 ExeFS and RomFS (the base game's RomFS is not needed at run time)
  game/updates/firmware/shared-fonts the system fonts, from the firmware the player gave (optional)
  shaders/                           the game's shaders, translated to SPIR-V
  profile/                           saves (created at the first start)
  mtf.ini                            settings (the in-game menu writes it)
  nvngx_dlss.dll                     NVIDIA DLSS, when the player asked for it
  mtf-install.json                   what built this folder (state.py)

Everything else derived from the game (generated C, the captured NRO, the shaders' sources, objects) stays in the temp
folder and goes when the install ends.
"""
import ctypes
import hashlib
import http.client
import os
import re
import shutil
import subprocess
import urllib.request
from pathlib import Path

from .runner import InstallError, Step

TITLE = '0400D4201C6EA000'
UPDATE = '327680'   # 1.1.1
EXE = 'mtf-recomp.exe'
GB = 1000 ** 3
DEST_NEED = 7 * GB     # the game's files (base and update, while extracting), the shaders and the executable
TEMP_NEED = 2 * GB     # generated C and objects, while the install runs

FONT_FILES = ('0-standard.bfttf', '1-chinese-simplified.bfttf', '2-chinese-simplified-extension.bfttf',
              '3-chinese-traditional.bfttf', '4-korean.bfttf', '5-nintendo-extension.bfttf')

DLSS_DLL = 'nvngx_dlss.dll'
DLSS_VERSION = '310.9.1'
DLSS_URL = f'https://raw.githubusercontent.com/NVIDIA/DLSS/v{DLSS_VERSION}/lib/Windows_x86_64/rel/{DLSS_DLL}'
DLSS_SIZE = 58956912
DLSS_SHA256 = '3975567b8943c53acce397f2b72380092f84f162d00b0d2c7d08a1025c563983'
DLSS_LICENSE = 'https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt'

# What the tools print about their progress
NINJA_LINE = re.compile(rb'\[(\d+)/(\d+)\]')
PARTS_LINE = re.compile(rb'^(\d+)/(\d+) (?:batches|archive ranges)')
GIB_LINE = re.compile(rb'^\s*([\d.]+)/([\d.]+) GiB')
GIB = 1024 ** 3


# ---- helpers ---------------------------------------------------------------------------------------------------------

def game(ctx):
    return ctx.dest / 'game'


def exefs(ctx):
    return game(ctx) / 'updates' / UPDATE / 'exefs'


def romfs(ctx):
    return game(ctx) / 'updates' / UPDATE / 'romfs'


def gen(ctx, name):
    return ctx.temp / 'gen' / f'{name}-full'


def captured(ctx):
    return ctx.temp / 'nro-captured.nro'


def fingerprint(*parts):
    h = hashlib.sha256()
    for p in parts:
        h.update(str(p).encode('utf-8') + b'\0')
    return h.hexdigest()[:32]


def file_sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while block := f.read(1 << 20):
            h.update(block)
    return h.hexdigest()


def has_c_files(folder):
    return folder.is_dir() and next(folder.rglob('*.c'), None) is not None


def tool_env(ctx):
    """The environment of every tool. A release keeps the player's own PATH out of the build: a compiler or a library
    installed on the PC must not change what gets built."""
    lay = ctx.layout
    env = dict(os.environ, PYTHONUTF8='1', PYTHONIOENCODING='utf-8', PYTHONUNBUFFERED='1', MTF_LLVM_MINGW_ROOT=str(lay.llvm),
               MTF_GENERATED_ROOT=str(ctx.temp), MTF_CONTENT_ROOT=str(game(ctx)))   # where game-derived output may go
    if not lay.dev:
        system = os.environ.get('SystemRoot', r'C:\Windows')
        env['PATH'] = os.pathsep.join([str(lay.llvm / 'bin'), str(lay.ninja.parent), str(lay.deps / 'bin'),
                                       rf'{system}\System32', system])
    return env


def run_tool(ctx, step, args, what, phase=None, progress=None, env=None, cwd=None):
    """Runs a tool with its output in <temp>/logs/<what>.log. progress: (regex with groups done and total, unit, scale):
    a line it matches becomes a progress event. A failure raises an error that names the log and quotes its end."""
    rep = ctx.reporter
    logs = ctx.temp / 'logs'
    logs.mkdir(parents=True, exist_ok=True)
    log = logs / f'{what}.log'
    if phase:
        rep.progress(step, phase, force=True)
    full_env = tool_env(ctx)
    full_env.update(env or {})
    rep.log(f'run {what}: ' + ' '.join(str(a) for a in args))
    with open(log, 'ab') as f:
        f.write(b'\n$ ' + ' '.join(str(a) for a in args).encode('utf-8', 'replace') + b'\n')
        p = subprocess.Popen([str(a) for a in args], cwd=str(cwd or ctx.layout.root), env=full_env, stdin=subprocess.DEVNULL,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        rest = b''
        try:
            while chunk := p.stdout.read1(1 << 16):
                f.write(chunk)
                f.flush()
                if progress:
                    pieces = re.split(rb'[\r\n]', rest + chunk)
                    rest = pieces.pop()
                    for line in pieces:
                        if m := progress[0].search(line):
                            scale = progress[2] if len(progress) > 2 else 1
                            rep.progress(step, phase, int(float(m[1]) * scale), int(float(m[2]) * scale), progress[1])
            code = p.wait()
        except KeyboardInterrupt:
            try:
                p.wait(30)   # Ctrl+Break reached the tool too
            except subprocess.TimeoutExpired:
                p.kill()
            raise
    if code:
        tail = log.read_text(encoding='utf-8', errors='replace').splitlines()[-12:]
        raise InstallError('tool.failed', tool=what, code=code, log=str(log), tail='\n'.join(tail))


def link_or_copy(src, dst):
    """A hard link when src and dst are on one NTFS volume (no second copy of gigabytes), else a copy."""
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists():
        dst.unlink()
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def tree_link_or_copy(src, dst):
    n = 0
    for f in src.rglob('*'):
        if f.is_file():
            link_or_copy(f, dst / f.relative_to(src))
            n += 1
    return n


def content_parts(folder):
    """(the folder with exefs/ and romfs/ of 1.1.1, the folder with the system fonts or None) of a game folder: what
    tools.nup_extract wrote, or a plain folder with exefs/ and romfs/ directly in it."""
    written = folder / 'updates' / UPDATE
    if (written / 'exefs' / 'main').is_file() and (written / 'romfs').is_dir():
        firmware = folder / 'updates' / 'firmware'
        return written, firmware if firmware.is_dir() else None
    if (folder / 'exefs' / 'main').is_file() and (folder / 'romfs').is_dir():
        firmware = folder / 'firmware'
        return folder, firmware if firmware.is_dir() else None
    return None


def fonts_dir(ctx):
    return game(ctx) / 'updates' / 'firmware' / 'shared-fonts'


def fonts_folder(folder):
    """The folder holding the six decoded system fonts, in folder or where the extraction leaves them; None if none."""
    for f in (folder, folder / 'shared-fonts', folder / 'updates' / 'firmware' / 'shared-fonts'):
        if all((f / name).is_file() for name in FONT_FILES):
            return f
    return None


def firmware_dir(folder):
    """The folder with the firmware's NCAs: the one given, or its Contents/registered (also from a NAND or system folder)."""
    for f in (folder, folder / 'Contents' / 'registered', folder / 'system' / 'Contents' / 'registered',
              folder / 'nand' / 'system' / 'Contents' / 'registered'):
        if f.is_dir() and next(f.glob('*.nca'), None) is not None:
            return f
    return None


def gpu_names():
    """The display adapters Windows lists; empty when it cannot say."""
    class DisplayDevice(ctypes.Structure):
        _fields_ = [('cb', ctypes.c_uint32), ('name', ctypes.c_wchar * 32), ('text', ctypes.c_wchar * 128),
                    ('flags', ctypes.c_uint32), ('id', ctypes.c_wchar * 128), ('key', ctypes.c_wchar * 128)]
    found = []
    try:
        for i in range(32):
            d = DisplayDevice()
            d.cb = ctypes.sizeof(d)
            if not ctypes.windll.user32.EnumDisplayDevicesW(None, i, ctypes.byref(d), 0):
                break
            found.append(d.text)
    except (AttributeError, OSError):
        return []
    return found


def cpu_level():
    """v3 (AVX2) when the CPU has it, else v2: the x86-64 level the game is compiled for."""
    PF_AVX2_INSTRUCTIONS_AVAILABLE = 40
    try:
        return 'v3' if ctypes.windll.kernel32.IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE) else 'v2'
    except (AttributeError, OSError):
        return 'v3'


def disk_id(path):
    p = Path(path)
    while not p.exists() and p.parent != p:
        p = p.parent
    return os.stat(p).st_dev, p


# ---- 1. check --------------------------------------------------------------------------------------------------------

class Check(Step):
    id = 'check'
    always = True

    def run(self, ctx):
        rep, lay, opts = ctx.reporter, ctx.layout, ctx.options
        rep.progress('check', 'inputs', force=True)
        missing = lay.missing()
        if missing:
            raise InstallError('check.release_incomplete', path=missing[0])
        if ctx.will_run('extract'):
            if not opts.nup and not opts.content:
                raise InstallError('check.inputs_needed')
            if opts.nup and not opts.nup.is_file():
                raise InstallError('check.game_missing', path=str(opts.nup))
            if opts.content and content_parts(opts.content) is None:
                raise InstallError('check.content_bad', path=str(opts.content), update='1.1.1')
        if opts.firmware:
            if fonts_folder(opts.firmware) is None:
                if firmware_dir(opts.firmware) is None:
                    raise InstallError('check.firmware_bad', path=str(opts.firmware))
                if not opts.keys:
                    raise InstallError('check.keys_needed')
                if not opts.keys.is_file():
                    raise InstallError('check.keys_missing', path=str(opts.keys))
        if opts.dlss_file and (not opts.dlss_file.is_file() or opts.dlss_file.name.lower() != DLSS_DLL):
            raise InstallError('check.dlss_file_bad', path=str(opts.dlss_file))
        system = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32'
        if not (system / 'vulkan-1.dll').is_file():
            raise InstallError('check.no_vulkan')
        gpus = gpu_names()
        if gpus and not any('NVIDIA' in g.upper() for g in gpus):
            rep.warning('warn.gpu_untested', gpu=gpus[0])

        rep.progress('check', 'space', force=True)
        dest_need = DEST_NEED if ctx.will_run('extract') else (DEST_NEED // 4 if ctx.will_run('shaders') else GB)
        temp_need = TEMP_NEED if ctx.will_run('lift') or ctx.will_run('build') else 0
        (dest_dev, dest_at), (temp_dev, temp_at) = disk_id(ctx.dest), disk_id(ctx.temp)
        dest_free, temp_free = shutil.disk_usage(dest_at).free, shutil.disk_usage(temp_at).free
        same = dest_dev == temp_dev
        need = dest_need + temp_need if same else dest_need
        rep.emit('space', dest=str(ctx.dest), need=need, free=dest_free, temp=str(ctx.temp), temp_need=temp_need,
                 temp_free=temp_free, same_disk=same)
        if dest_free < need:
            raise InstallError('check.disk_space', path=str(ctx.dest), need_gb=round(need / GB, 1), free_gb=round(dest_free / GB, 1))
        if not same and temp_free < temp_need:
            raise InstallError('check.disk_space', path=str(ctx.temp), need_gb=round(temp_need / GB, 1), free_gb=round(temp_free / GB, 1))
        ctx.dest.mkdir(parents=True, exist_ok=True)
        ctx.temp.mkdir(parents=True, exist_ok=True)


# ---- 2. extract ------------------------------------------------------------------------------------------------------

class Extract(Step):
    id = 'extract'

    def key(self, ctx):   # the audit and the build check the game's files: only the version the release builds counts
        return fingerprint('extract-1', TITLE, UPDATE)

    def current(self, ctx, record):
        return (exefs(ctx) / 'main').is_file() and romfs(ctx).is_dir()

    def run(self, ctx):
        opts = ctx.options
        if opts.content:
            ctx.reporter.progress('extract', 'copy', force=True)
            update, firmware = content_parts(opts.content)
            for part in ('exefs', 'romfs'):
                tree_link_or_copy(update / part, game(ctx) / 'updates' / UPDATE / part)
            if firmware:
                tree_link_or_copy(firmware, game(ctx) / 'updates' / 'firmware')
            return {'source': 'folder'}
        py = ctx.layout.python
        audit = ctx.temp / 'nup-audit.json'
        audit.parent.mkdir(parents=True, exist_ok=True)
        run_tool(ctx, 'extract', [py, '-m', 'tools.nup_audit', opts.nup, '--out', audit], 'nup_audit', phase='audit')
        run_tool(ctx, 'extract', [py, '-m', 'tools.nup_extract', audit, '--out', game(ctx)], 'nup_extract', phase='extract',
                 progress=(GIB_LINE, 'bytes', GIB))
        base = game(ctx) / 'base'
        if base.is_dir():   # the update's RomFS is complete; the base game's is not read at run time
            shutil.rmtree(base)
        return {'source': 'nup'}


# ---- 3. the system fonts --------------------------------------------------------------------------------------------

class Fonts(Step):
    """The console's system fonts, optional: the button icons of the game's text (L, R, A...) are in them, and without them
    a stand-in font shows ?? instead. From a folder with the decoded fonts, or read from the firmware's NCAs, which takes
    the player's prod.keys. The keys file is only read, by the vendored extractor; nothing of it is stored."""
    id = 'fonts'

    def key(self, ctx):
        firmware = ctx.options.firmware
        return fingerprint('fonts-1', str(firmware) if firmware else 'none')

    def current(self, ctx, record):
        return not ctx.options.firmware or fonts_folder(fonts_dir(ctx)) is not None

    def run(self, ctx):
        opts, rep = ctx.options, ctx.reporter
        if not opts.firmware:
            return {}
        ready = fonts_folder(opts.firmware)
        if ready:
            rep.progress('fonts', 'copy', force=True)
            for name in FONT_FILES:
                link_or_copy(ready / name, fonts_dir(ctx) / name)
            return {'source': 'folder'}
        out = ctx.temp / 'shared-fonts'
        shutil.rmtree(out, ignore_errors=True)
        lay = ctx.layout
        run_tool(ctx, 'fonts', [lay.python, lay.root / 'tools' / 'vendor' / 'macnativegames' / 'extract_shared_fonts.py',
                                '--keys', opts.keys, '--firmware', firmware_dir(opts.firmware), '--output', out],
                 'fonts', phase='extract', cwd=ctx.temp)
        missing = [name for name in FONT_FILES if not (out / name).is_file()]
        if missing:
            raise InstallError('fonts.incomplete', files=', '.join(missing), log=str(ctx.temp / 'logs' / 'fonts.log'))
        for name in FONT_FILES:
            link_or_copy(out / name, fonts_dir(ctx) / name)
        shutil.rmtree(out, ignore_errors=True)
        return {'source': 'firmware'}


# ---- building the runtime against lifted modules --------------------------------------------------------------------

def _runtime_sources_from_cmake(cmake_file):
    text = cmake_file.read_text(encoding='utf-8')
    m = re.search(r'set\(MTF_RUNTIME_SOURCES\s+(.*?)\r?\n\)', text, re.S)
    if not m:
        raise InstallError('build.no_sources', path=str(cmake_file))
    out = []
    for raw in m.group(1).splitlines():
        line = raw.split('#', 1)[0].strip()
        if line:
            out.append(line)
    return out


def _write_build_ninja(lay, tree, dirs, glue, out, march, cpu_def):
    """Emit build.ninja directly without requiring CMake."""
    tree.mkdir(parents=True, exist_ok=True)
    out.mkdir(parents=True, exist_ok=True)
    cc = (lay.llvm / 'bin' / 'clang.exe').as_posix()
    cxx = (lay.llvm / 'bin' / 'clang++.exe').as_posix()
    rc = (lay.llvm / 'bin' / 'llvm-windres.exe').as_posix()

    def esc(s):
        return s.replace('$', '$$').replace(':', '$:').replace(' ', '$ ')

    fmap = ' '.join(
        f'"-ffile-prefix-map={p.as_posix()}=."'
        for p in (lay.root, tree, glue, lay.deps, *dirs)
    )
    gen_flags = f'-O2 -fwrapv -fno-strict-aliasing -Wno-return-type -Wno-unused-label -fvisibility=hidden {march} -DNDEBUG {fmap} -I"{(lay.root / "runtime").as_posix()}"'
    rt_inc = f'-I"{(lay.root / "runtime").as_posix()}" -I"{(lay.root / "runtime" / "third_party" / "imgui").as_posix()}" -I"{glue.as_posix()}" -isystem "{(lay.deps / "include").as_posix()}" -isystem "{(lay.deps / "include" / "vma").as_posix()}"'
    rt_defs = f'-D{cpu_def} -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNDEBUG'
    rt_common = f'-O2 -g -Wall -fno-strict-aliasing {march} {rt_defs} {fmap} {rt_inc}'

    split_rt = portable and (lay.deps / 'lib' / 'libmtf-recomp.dll.a').is_file() and (lay.deps / 'bin' / 'mtf-recomp.exe').is_file()
    prebuilt_rt = (not split_rt) and (lay.deps / 'lib' / 'libmtfrt.a').is_file()
    link_driver = '$cc' if (split_rt or prebuilt_rt) else '$cxx'
    rt_libs = '-Wl,--whole-archive -lmtfrt -Wl,--no-whole-archive -lc++ ' if prebuilt_rt else ''
    rsp_file = tree / 'objects.rsp'

    if split_rt:
        link_cmd = (f'$cc --target=x86_64-w64-windows-gnu -shared -O2 -g {march} -fuse-ld=lld -Wl,--error-limit=0 '
                    f'@"{rsp_file.as_posix()}" -L"{(lay.deps / "lib").as_posix()}" -lmtf-recomp -o $out')
    else:
        link_cmd = (f'{link_driver} --target=x86_64-w64-windows-gnu -O2 -g {march} -fuse-ld=lld -Wl,--stack,16777216 '
                    f'-Wl,--error-limit=0 @"{rsp_file.as_posix()}" -L"{(lay.deps / "lib").as_posix()}" {rt_libs}'
                    f'-lvulkan-1 -lSDL3 -lopus -lvpx -ldatachannel -ljuice -lsrtp2 -lusrsctp -lmbedtls -lmbedx509 '
                    f'-lmbedcrypto -leverest -lp256m -liphlpapi -lpsapi -lbcrypt -luser32 -lgdi32 -ldwmapi -limm32 '
                    f'-lwinmm -lole32 -loleaut32 -lmfplat -lmfuuid -lshell32 -luuid -lpdh -lrstrtmgr -lws2_32 -lwinhttp -o $out')

    lines = [
        'ninja_required_version = 1.5',
        f'cc = "{cc}"',
        f'cxx = "{cxx}"',
        f'rc = "{rc}"',
        'rule cc_gen',
        f'  command = $cc --target=x86_64-w64-windows-gnu {gen_flags} -std=gnu17 -MD -MF $out.d -c $in -o $out',
        '  depfile = $out.d',
        '  deps = gcc',
        'rule cc_rt',
        f'  command = $cc --target=x86_64-w64-windows-gnu {rt_common} -std=gnu17 $extra -MD -MF $out.d -c $in -o $out',
        '  depfile = $out.d',
        '  deps = gcc',
        'rule cxx_rt',
        f'  command = $cxx --target=x86_64-w64-windows-gnu {rt_common} $extra -MD -MF $out.d -c $in -o $out',
        '  depfile = $out.d',
        '  deps = gcc',
        'rule rc_rt',
        f'  command = $rc --target=pe-x86-64 -O coff -i $in -o $out',
        'rule link',
        f'  command = {link_cmd}',
        '',
    ]

    objs = []
    obj_dir = tree / 'obj'
    for d in dirs:
        for src in sorted(d.rglob('*.c')):
            rel = src.relative_to(d).with_suffix('.obj')
            obj = obj_dir / 'gen' / d.name / rel
            obj.parent.mkdir(parents=True, exist_ok=True)
            lines.append(f'build {esc(obj.as_posix())}: cc_gen {esc(src.as_posix())}')
            objs.append(obj)

    if split_rt:
        pass
    elif prebuilt_rt:
        rc_obj = lay.deps / 'lib' / 'mtf.rc.obj'
        if rc_obj.is_file():
            objs.append(rc_obj)
        for rel_src in ('runtime/loader.c', 'runtime/memory.c'):
            src = lay.root / rel_src
            obj = (obj_dir / 'rt' / rel_src).with_suffix('.obj')
            obj.parent.mkdir(parents=True, exist_ok=True)
            lines.append(f'build {esc(obj.as_posix())}: cc_rt {esc(src.as_posix())}')
            lines.append('  extra = ')
            objs.append(obj)
    else:
        for rel_src in _runtime_sources_from_cmake(lay.root / 'CMakeLists.txt'):
            src = lay.root / rel_src
            obj = (obj_dir / 'rt' / rel_src).with_suffix('.obj')
            obj.parent.mkdir(parents=True, exist_ok=True)
            ext = src.suffix.lower()
            if ext == '.rc':
                lines.append(f'build {esc(obj.as_posix())}: rc_rt {esc(src.as_posix())}')
            elif ext == '.cpp':
                if rel_src == 'runtime/vk_vma.cpp':
                    extra = '-std=c++17 -Wno-nullability-completeness'
                elif rel_src in ('runtime/ui/menu.cpp', 'runtime/ui/menu_win.cpp', 'runtime/ui/option_rows.cpp'):
                    extra = '-std=c++20'
                elif rel_src.startswith('runtime/third_party/imgui/'):
                    extra = '-std=c++17 -w'
                else:
                    extra = '-std=c++17'
                lines.append(f'build {esc(obj.as_posix())}: cxx_rt {esc(src.as_posix())}')
                lines.append(f'  extra = {extra}')
            else:
                lines.append(f'build {esc(obj.as_posix())}: cc_rt {esc(src.as_posix())}')
                lines.append('  extra = ')
            objs.append(obj)

    glue_c = glue / 'modules.c'
    glue_obj = obj_dir / 'glue' / 'modules.obj'
    glue_obj.parent.mkdir(parents=True, exist_ok=True)
    lines.append(f'build {esc(glue_obj.as_posix())}: cc_rt {esc(glue_c.as_posix())}')
    lines.append('  extra = -DMTF_GAME_DLL' if split_rt else '  extra = ')
    objs.append(glue_obj)

    target_bin = out / ('mtf-game.dll' if split_rt else EXE)
    rsp_file.write_text('\n'.join(f'"{o.as_posix()}"' for o in objs) + '\n', encoding='utf-8')
    lines.append(f'build {esc(target_bin.as_posix())}: link ' + ' '.join(esc(o.as_posix()) for o in objs))
    lines.append(f'default {esc(target_bin.as_posix())}')
    (tree / 'build.ninja').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    return target_bin


def build(ctx, step, modules, out, portable):
    """Glue and build of mtf-recomp.exe into out (its own build tree in the temp folder)."""
    lay = ctx.layout
    glue = ctx.temp / ('glue-portable' if portable else 'glue')
    dirs = [gen(ctx, m) for m in modules]
    glue_args = [lay.python, lay.root / 'recomp' / 'glue.py', '--generated', *dirs, '--out', glue]
    if portable:   # paths relative to the exe's folder; the hashes come from the files in place
        glue_args += ['--portable', '--exefs', f'game/updates/{UPDATE}/exefs', '--romfs', f'game/updates/{UPDATE}/romfs',
                      '--user', 'profile', '--check-exefs', exefs(ctx)]
    else:
        glue_args += ['--exefs', exefs(ctx), '--romfs', romfs(ctx), '--user', ctx.temp / 'profile']
    run_tool(ctx, step, glue_args, 'glue', phase='glue')
    tree = ctx.temp / ('cmake-portable' if portable else 'cmake')
    if lay.cmake is not None and lay.cmake.is_file():
        toolchain = lay.root / 'tools' / 'cmake' / 'llvm-mingw.cmake'
        run_tool(ctx, step, [lay.cmake, '-S', lay.root, '-B', tree, '-G', 'Ninja',
                             f'-DCMAKE_TOOLCHAIN_FILE={toolchain.as_posix()}', '-DCMAKE_BUILD_TYPE=Release',
                             f'-DCMAKE_MAKE_PROGRAM={lay.ninja.as_posix()}',
                             f'-DMTF_GENERATED_DIRS={";".join(d.as_posix() for d in dirs)}', f'-DMTF_GLUE_DIR={glue.as_posix()}',
                             f'-DMTF_OUTPUT_DIR={out.as_posix()}', f'-DMTF_DEPS_DIR={lay.deps.as_posix()}',
                             f'-DMTF_CPU_LEVEL={cpu_level()}'], 'cmake-configure', phase='configure')
        run_tool(ctx, step, [lay.cmake, '--build', tree, '--parallel', ctx.options.jobs], 'cmake-build', phase='compile',
                 progress=(NINJA_LINE, 'objects'))
    else:
        lvl = cpu_level()
        march = '-march=x86-64-v2' if lvl == 'v2' else '-march=x86-64-v3'
        cpu_def = 'RT_CPU_LEVEL=2' if lvl == 'v2' else 'RT_CPU_LEVEL=3'
        _write_build_ninja(lay, tree, dirs, glue, out, march, cpu_def)
        run_tool(ctx, step, [lay.ninja, '-C', tree, '-j', ctx.options.jobs], 'ninja-build', phase='compile',
                 progress=(NINJA_LINE, 'objects'))
        for dll in (lay.deps / 'bin').glob('*.dll'):
            shutil.copy2(dll, out / dll.name)
        rt_bin = lay.llvm / 'x86_64-w64-mingw32' / 'bin'
        for name in ('libc++.dll', 'libunwind.dll', 'libwinpthread-1.dll'):
            src_dll = rt_bin / name
            if src_dll.is_file():
                shutil.copy2(src_dll, out / name)
    if split_rt:
        shutil.copy2(lay.deps / 'bin' / EXE, out / EXE)
    exe = out / EXE
    if not exe.is_file():
        raise InstallError('build.no_exe', path=str(exe))
    return exe


# ---- 4. lift main and sdk -------------------------------------------------------------------------------------------

class Lift(Step):
    id = 'lift'
    transient = True
    readers = ('capture', 'build')

    def key(self, ctx):
        return fingerprint('lift-1', ctx.keys['extract'], ctx.layout.release)

    def current(self, ctx, record):
        return has_c_files(gen(ctx, 'main')) and has_c_files(gen(ctx, 'sdk'))

    def run(self, ctx):
        lay = ctx.layout
        for module in ('main', 'sdk'):
            run_tool(ctx, 'lift', [lay.python, lay.root / 'recomp' / 'lift.py', '--exefs', exefs(ctx), '--out', gen(ctx, module),
                                   '--modules', module, '--jobs', ctx.options.jobs], f'lift-{module}', phase=module)


# ---- 5. capture the game's NRO --------------------------------------------------------------------------------------

class Capture(Step):
    """The game's code is not a file: the boot stub decodes it at run time. A runtime with main and sdk boots the game
    until nn::ro::LoadModule and writes the decoded NRO, verified against the NRR in the RomFS."""
    id = 'capture'
    transient = True
    readers = ('lift-nro', 'shaders')

    def key(self, ctx):
        return fingerprint('capture-1', ctx.keys['lift'])

    def current(self, ctx, record):
        return captured(ctx).is_file()

    def run(self, ctx):
        exe = build(ctx, 'capture', ['main', 'sdk'], ctx.temp / 'bin-capture', portable=False)
        out = captured(ctx)
        if out.exists():
            out.unlink()
        run_tool(ctx, 'capture', [exe, '--capture-nro', '--exefs', exefs(ctx), '--romfs', romfs(ctx),
                                  '--profile', ctx.temp / 'profile'], 'capture', phase='run',
                 env={'MTF_NRO_CAPTURE': str(out)}, cwd=ctx.temp)
        if not out.is_file():
            raise InstallError('capture.no_nro', log=str(ctx.temp / 'logs' / 'capture.log'))


# ---- 6. lift the NRO ------------------------------------------------------------------------------------------------

class LiftNro(Step):
    id = 'lift-nro'
    transient = True
    readers = ('build',)

    def key(self, ctx):
        return fingerprint('lift-nro-1', ctx.keys['capture'], ctx.layout.release)

    def current(self, ctx, record):
        return has_c_files(gen(ctx, 'nro'))

    def run(self, ctx):
        lay = ctx.layout
        run_tool(ctx, 'lift-nro', [lay.python, lay.root / 'recomp' / 'nro_lift.py', '--exefs', exefs(ctx), '--nro', captured(ctx),
                                   '--out', gen(ctx, 'nro'), '--jobs', ctx.options.jobs], 'lift-nro', phase='run')


# ---- 7. shaders -----------------------------------------------------------------------------------------------------

class Shaders(Step):
    id = 'shaders'

    def key(self, ctx):
        lay = ctx.layout
        answers = lay.root / 'profiles' / TITLE / 'shader_answers.tsv'
        xlate_hash = file_sha256(lay.xlate) if lay.xlate.is_file() else 'missing'
        return fingerprint('shaders-1', ctx.keys['extract'], xlate_hash, file_sha256(answers),
                           file_sha256(lay.root / 'tools' / 'shaders' / 'translate.py'))

    def current(self, ctx, record):
        return (ctx.dest / 'shaders').is_dir()

    def run(self, ctx):
        lay, opts = ctx.layout, ctx.options
        dst = ctx.dest / 'shaders'
        if opts.shaders_from:
            ctx.reporter.progress('shaders', 'copy', force=True)
            tree_link_or_copy(opts.shaders_from, dst)
            return
        shutil.rmtree(dst, ignore_errors=True)
        src = ctx.temp / 'shaders-src'
        tools = lay.root / 'tools' / 'shaders'
        run_tool(ctx, 'shaders', [lay.python, tools / 'extract.py', '--archive', romfs(ctx) / '42', '--nro', captured(ctx),
                                  '--out', src, '--jobs', opts.jobs], 'shader-extract', phase='extract',
                 progress=(PARTS_LINE, 'parts'))
        translate = [lay.python, tools / 'translate.py', '--src', src, '--out', dst, '--xlate', lay.xlate, '--jobs', opts.jobs]
        run_tool(ctx, 'shaders', translate, 'shader-translate', phase='translate', progress=(PARTS_LINE, 'parts'))
        # the vertex shaders' last-frame variants: DLAA's motion vectors for the characters
        run_tool(ctx, 'shaders', [*translate, '--mvprev'], 'shader-translate-motion', phase='motion',
                 progress=(PARTS_LINE, 'parts'))


# ---- 8. build the game ----------------------------------------------------------------------------------------------

class Build(Step):
    id = 'build'

    def key(self, ctx):
        return fingerprint('build-1', ctx.keys['lift'], ctx.keys['lift-nro'], ctx.layout.release, cpu_level())

    def current(self, ctx, record):
        return (ctx.dest / EXE).is_file()

    def run(self, ctx):
        out = ctx.temp / 'bin'
        build(ctx, 'build', ['main', 'sdk', 'nro'], out, portable=True)
        for f in out.iterdir():
            if f.suffix.lower() in ('.exe', '.dll') and f.name != DLSS_DLL:
                shutil.copy2(f, ctx.dest / f.name)
        for cand in (ctx.layout.root / 'mtf-updater.exe', ctx.layout.deps / 'bin' / 'mtf-updater.exe'):
            if cand.is_file():
                shutil.copy2(cand, ctx.dest / 'mtf-updater.exe')
                break


# ---- 9. NVIDIA DLSS --------------------------------------------------------------------------------------------------

class Dlss(Step):
    """With --dlss on a PC with an NVIDIA RTX GPU: nvngx_dlss.dll next to the game. A DLSS library the player put there
    (another version) is kept. A failed download warns and is tried again on the next run; the game runs without it."""
    id = 'dlss'

    def key(self, ctx):
        opts = ctx.options
        if opts.dlss_file:
            return fingerprint('dlss-1', 'file', file_sha256(opts.dlss_file))
        return fingerprint('dlss-1', 'off') if not opts.dlss else fingerprint('dlss-1', DLSS_VERSION, DLSS_SHA256)

    def current(self, ctx, record):
        if record.get('failed'):
            return False
        return not record.get('installed') or (ctx.dest / DLSS_DLL).is_file()

    def run(self, ctx):
        opts, rep, target = ctx.options, ctx.reporter, ctx.dest / DLSS_DLL
        if opts.dlss_file:
            shutil.copy2(opts.dlss_file, target)
            return {'installed': 'file'}
        if not opts.dlss:
            return {}
        gpus = gpu_names()
        if gpus and not any('NVIDIA' in g.upper() and 'RTX' in g.upper() for g in gpus):
            # The box is ticked by default, so most players without an RTX GPU never chose it: a log line, no warning
            rep.log('dlss: no NVIDIA RTX GPU among the display adapters, nothing downloaded')
            return {'skipped': 'no_rtx'}
        if target.is_file():
            if file_sha256(target) == DLSS_SHA256:
                return {'installed': DLSS_VERSION}
            rep.warning('warn.dlss_kept', path=str(target))
            return {'kept': True}
        part = ctx.temp / (DLSS_DLL + '.part')
        rep.progress('dlss', 'download', 0, DLSS_SIZE, 'bytes', force=True)
        rep.log(f'dlss: {DLSS_URL} (license {DLSS_LICENSE})')
        try:
            part.parent.mkdir(parents=True, exist_ok=True)
            h, done = hashlib.sha256(), 0
            with urllib.request.urlopen(DLSS_URL, timeout=60) as r, open(part, 'wb') as f:
                while block := r.read(1 << 20):
                    f.write(block)
                    h.update(block)
                    done += len(block)
                    rep.progress('dlss', 'download', min(done, DLSS_SIZE), DLSS_SIZE, 'bytes')
            if done != DLSS_SIZE or h.hexdigest() != DLSS_SHA256:
                raise OSError(f'{done} bytes, SHA-256 {h.hexdigest()[:16]}, not the expected file')
            os.replace(part, target)
        # urllib's errors, timeouts and a bad file; a connection cut mid-file raises http.client.IncompleteRead,
        # which is not an OSError
        except (OSError, http.client.HTTPException) as e:
            part.unlink(missing_ok=True)
            rep.warning('warn.dlss_download_failed', url=DLSS_URL, detail=str(e)[:300])
            return {'failed': True}
        return {'installed': DLSS_VERSION}


# ---- 10. finish -------------------------------------------------------------------------------------------------------

class Finish(Step):
    id = 'finish'
    always = True

    def run(self, ctx):
        ctx.reporter.progress('finish', 'settings', force=True)
        ini = ctx.dest / 'mtf.ini'
        if not ini.exists():
            extra = ''
            if (ctx.dest / DLSS_DLL).is_file():
                extra += 'DLAA = 1\n'
            lang = getattr(ctx.options, 'lang', None)
            if lang:
                extra += f'MENU_LANG = {lang}\n'
            ini.write_text('# MTF Recomp settings, written by the in-game menu (Minus+Plus, F1 or Esc).\n'
                           '# KEY = value; the MTF_KEY environment variable wins over a line here.\n'
                           + extra, encoding='utf-8')
        (ctx.dest / 'profile').mkdir(exist_ok=True)
        if ctx.options.keep_work or ctx.options.only:
            return
        ctx.reporter.progress('finish', 'cleanup', force=True)
        for name in ('shared-fonts', 'gen', 'glue', 'glue-portable', 'cmake', 'cmake-portable', 'bin', 'bin-capture', 'shaders-src', 'profile'):
            shutil.rmtree(ctx.temp / name, ignore_errors=True)
        for name in (captured(ctx).name, 'nup-audit.json'):
            (ctx.temp / name).unlink(missing_ok=True)


STEPS = (Check, Extract, Lift, Capture, LiftNro, Shaders, Build, Dlss, Finish)
STEP_IDS = [s.id for s in STEPS]


def all_steps():
    return [s() for s in STEPS]
