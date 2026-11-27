"""Where the installer finds what it runs: the one place that maps the release folder and a repository checkout.

Release (tools/make_release.py), root = the unpacked folder:

    MTF-Setup.exe           starts python\\pythonw.exe -m installer.gui
    python/python.exe       runs every tool; its ._pth puts .., ../tools, ../tools/shaders and ../recomp on sys.path
    llvm/                   llvm-mingw
    cmake/bin/cmake.exe     builds the game (the sources of CMakeLists.txt and runtime/, with the player's lifted code)
    bin/ninja.exe
    bin/xlate/dmsh_xlate.exe  the shader translator, self-contained
    deps/                   include/, lib/ and bin/ of SDL3, Vulkan, VMA, Opus, libvpx, zstd
    CMakeLists.txt, runtime/, recomp/, tools/, profiles/
    VERSION                 the release version, one line
    temp/                   the default temp folder (created)

Dev mode, root = a repository checkout: .venv's Python, the toolchain of docs/building.md under build/toolchain, the
version from VERSION with -dev and the commit, build/generated/installer as the temp folder.
"""
import json
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

IS_WIN = os.name == 'nt'


@dataclass(frozen=True)
class Layout:
    root: Path
    dev: bool
    release: str
    python: Path
    llvm: Path            # llvm-mingw tree
    cmake: Path
    ninja: Path
    deps: Path            # include/, lib/, bin/
    xlate: Path           # the shader translator: an .exe in a release, a .dll for dotnet in a checkout
    temp: Path

    @property
    def runtime(self):
        return self.root / 'runtime'

    def missing(self):
        """Paths the steps need that do not exist."""
        clang = 'clang.exe' if IS_WIN else 'clang'
        need = [self.python, self.llvm / 'bin' / clang, self.ninja, self.deps / 'include' / 'SDL3' / 'SDL.h',
                self.deps / 'lib', self.xlate, self.root / 'CMakeLists.txt', self.runtime / 'main.c',
                self.root / 'recomp' / 'lift.py', self.root / 'recomp' / 'glue.py', self.root / 'tools' / 'nup_extract.py',
                self.root / 'tools' / 'shaders' / 'translate.py']
        if self.cmake is not None:
            need.append(self.cmake)
        return [str(p) for p in need if not p.exists()]


def release_layout(root):
    version = root / 'VERSION'
    if IS_WIN:
        py = root / 'python' / 'python.exe'
        ninja = root / 'bin' / 'ninja.exe'
        xlate = root / 'bin' / 'xlate' / 'dmsh_xlate.exe'
    else:
        py = root / 'python' / 'bin' / 'python3'
        ninja = root / 'bin' / 'ninja'
        xlate = root / 'bin' / 'xlate' / 'dmsh_xlate'
    return Layout(root=root, dev=False,
                  release=version.read_text(encoding='utf-8').strip() if version.is_file() else 'unknown',
                  python=py, llvm=root / 'llvm', cmake=None,
                  ninja=ninja, deps=root / 'deps', xlate=xlate,
                  temp=root / 'temp')


def _first_existing(candidates):
    for p in candidates:
        if p and Path(p).exists():
            return Path(p)
    return Path(candidates[0]) if candidates else Path('missing')


def _find_cmake(root, tools):
    cands = []
    if tools.get('cmake_path'):
        cands.append(tools['cmake_path'])
    w = shutil.which('cmake')
    if w:
        cands.append(w)
    for vs in sorted(Path('C:/Program Files/Microsoft Visual Studio').glob('*/**/CMake/CMake/bin/cmake.exe')):
        cands.append(vs)
    cands.append(root / 'build' / 'toolchain' / 'cmake-missing')
    return _first_existing(cands)


def dev_layout(root):
    commit = ''
    if shutil.which('git'):
        try:
            commit = subprocess.run(['git', '-C', str(root), 'rev-parse', '--short', 'HEAD'], capture_output=True,
                                    text=True, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0)).stdout.strip()
        except OSError:
            commit = ''
    vf = root / 'VERSION'
    semver = vf.read_text(encoding='utf-8').strip() if vf.is_file() else '0.1.0'
    tc = root / 'toolchain'
    btc = root / 'build' / 'toolchain'
    manifest = btc / 'toolchain-manifest.json'
    tools = json.loads(manifest.read_text(encoding='utf-8-sig')) if manifest.is_file() else {}
    trees = sorted(p.parent.parent for base in (tc, btc) if base.is_dir() for p in base.glob('llvm-mingw-*/bin/clang.exe'))
    py_cands = [root / '.venv' / 'Scripts' / 'python.exe', tc / 'python' / 'python.exe']
    if not getattr(sys, 'frozen', False):
        py_cands.append(Path(sys.executable))
    py_which = shutil.which('python')
    if py_which:
        py_cands.append(Path(py_which))
    ninja_cands = [tc / 'ninja' / 'ninja.exe']
    if tools.get('ninja_path'):
        ninja_cands.append(Path(tools['ninja_path']))
    nw = shutil.which('ninja')
    if nw:
        ninja_cands.append(Path(nw))
    deps = _first_existing([tc / 'deps-mingw', btc / 'deps'])
    xlate = _first_existing([
        btc / 'xlate' / 'dmsh_xlate.exe',
        root / 'bin' / 'xlate' / 'dmsh_xlate.exe',
        btc / 'dmsh_xlate-sbuf' / 'dmsh_xlate.dll',
    ])
    return Layout(root=root, dev=True, release=f'{semver}-dev+{commit or "unknown"}',
                  python=_first_existing(py_cands),
                  llvm=trees[-1] if trees else tc / 'llvm-mingw-missing',
                  cmake=_find_cmake(root, tools),
                  ninja=_first_existing(ninja_cands), deps=deps,
                  xlate=xlate, temp=root / 'build' / 'generated' / 'installer')


def is_release(root):
    return (root / 'python' / 'python.exe').is_file() if IS_WIN else (root / 'python' / 'bin' / 'python3').is_file()


def find(root=None):
    """The layout of root (default: the folder above this package, or near the frozen executable)."""
    if root:
        r = Path(root).resolve()
    elif getattr(sys, 'frozen', False):
        exe_dir = Path(sys.executable).resolve().parent
        r = next((d for d in (exe_dir, exe_dir.parent, exe_dir.parent.parent) if (d / 'CMakeLists.txt').is_file()), exe_dir)
    else:
        r = Path(__file__).resolve().parent.parent.resolve()
    return release_layout(r) if is_release(r) else dev_layout(r)
