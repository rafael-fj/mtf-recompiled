#!/usr/bin/env python3
"""Package a clean, game-free MTF Recomp source/launcher release archive."""

from __future__ import annotations

import argparse
import concurrent.futures
import io
import re
import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

INCLUDE_DIRS = [
    "docs",
    "installer",
    "profiles",
    "recomp",
    "runtime",
    "tests",
    "tools",
]

INCLUDE_FILES = [
    "AGENTS.md",
    "LICENSE",
    "README.md",
    "THIRD_PARTY_NOTICES.md",
    "CMakeLists.txt",
    "VERSION",
    "config.example.toml",
    "recomplauncher.json",
]

SKIP_PARTS = {
    "__pycache__",
    ".git",
    "build",
    "generated",
    "work",
    "bin",
    "obj",
}


def should_skip(path: Path) -> bool:
    if any(part in SKIP_PARTS for part in path.parts):
        return True
    if path.suffix.lower() in {".exe", ".dll", ".o", ".obj", ".spv", ".nro", ".nso", ".sqlite"}:
        return True
    return False


def make_release(version: str, out_dir: Path) -> Path:
    stage = out_dir / f"mtf-recomp-{version}-src"
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True, exist_ok=True)

    for rel in INCLUDE_FILES:
        src = ROOT / rel
        if src.is_file():
            shutil.copy2(src, stage / rel)

    for rel in INCLUDE_DIRS:
        src_dir = ROOT / rel
        if not src_dir.is_dir():
            continue
        for p in src_dir.rglob("*"):
            if p.is_dir():
                continue
            rel_p = p.relative_to(ROOT)
            if should_skip(rel_p):
                continue
            dst = stage / rel_p
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(p, dst)

    # Verify zero game content inside staged directory using tools/check_content.py
    subprocess.check_call([sys.executable, str(ROOT / "tools" / "check_content.py")], cwd=str(stage))

    zip_path = out_dir / f"mtf-recomp-{version}-src.zip"
    if zip_path.exists():
        zip_path.unlink()
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED, strict_timestamps=False) as zf:
        for p in sorted(stage.rglob("*")):
            if p.is_file():
                zf.write(p, p.relative_to(out_dir))
    return zip_path


PLAYER_DIRS = [
    "installer",
    "profiles",
    "recomp",
    "runtime",
    "tools",
]

PLAYER_FILES = [
    "LICENSE",
    "README.md",
    "THIRD_PARTY_NOTICES.md",
    "CMakeLists.txt",
    "VERSION",
    "recomplauncher.json",
]


def make_windows_zip(version: str, out_dir: Path, setup_path: Path, bundle_dir: Path | None = None) -> Path:
    """Package MTF-Recomp-v<version>-Windows-x64.zip for players / RecompLauncher (no docs/tests/installer .py)."""
    win_zip = out_dir / f"MTF-Recomp-v{version}-Windows-x64.zip"
    top = Path(f"MTF-Recomp-v{version}-Windows-x64")
    if win_zip.exists():
        win_zip.unlink()
    with zipfile.ZipFile(win_zip, "w", compression=zipfile.ZIP_DEFLATED, strict_timestamps=False) as zf:
        for rel in PLAYER_FILES:
            src = ROOT / rel
            if src.is_file():
                zf.write(src, top / rel)
        for rel in PLAYER_DIRS:
            src_dir = ROOT / rel
            if not src_dir.is_dir():
                continue
            for p in sorted(src_dir.rglob("*")):
                if p.is_file() and not should_skip(p.relative_to(ROOT)):
                    zf.write(p, top / p.relative_to(ROOT))
        if setup_path.is_dir():
            for p in sorted(setup_path.rglob("*")):
                if p.is_file():
                    zf.write(p, top / p.relative_to(setup_path))
        elif setup_path.is_file():
            zf.write(setup_path, top / setup_path.name)
        if bundle_dir is not None and bundle_dir.is_dir():
            for p in sorted(bundle_dir.rglob("*")):
                if p.is_file():
                    zf.write(p, top / p.relative_to(bundle_dir))
    return win_zip


def make_runtime_zip(version: str, out_dir: Path, bundle_dir: Path | None = None) -> Path | None:
    """Package internal-updater-v<version>-win64.zip for fast in-game runtime updates (~12 MB)."""
    if bundle_dir is None:
        return None
    rt_exe = bundle_dir / "deps" / "bin" / "mtf-recomp.exe"
    upd_exe = bundle_dir / "deps" / "bin" / "mtf-updater.exe"
    if not rt_exe.is_file():
        return None
    rt_zip = out_dir / f"internal-updater-v{version}-win64.zip"
    top = Path(f"internal-updater-v{version}-win64")
    if rt_zip.exists():
        rt_zip.unlink()
    manifest = f'{{\n  "version": "{version}",\n  "game_abi": 1,\n  "runtime_only": true\n}}\n'
    with zipfile.ZipFile(rt_zip, "w", compression=zipfile.ZIP_DEFLATED, strict_timestamps=False) as zf:
        zf.writestr(str(top / "mtf-runtime-manifest.json"), manifest)
        zf.write(rt_exe, top / "mtf-recomp.exe")
        if upd_exe.is_file():
            zf.write(upd_exe, top / "mtf-updater.exe")
        for dll in sorted((bundle_dir / "deps" / "bin").glob("*.dll")):
            zf.write(dll, top / dll.name)
        for name in ("libc++.dll", "libunwind.dll", "libwinpthread-1.dll"):
            p = bundle_dir / "llvm" / "x86_64-w64-mingw32" / "bin" / name
            if p.is_file():
                zf.write(p, top / name)
        for rel in ("LICENSE", "THIRD_PARTY_NOTICES.md", "VERSION"):
            src = ROOT / rel
            if src.is_file():
                zf.write(src, top / rel)
    return rt_zip


def make_linux_tar(version: str, out_dir: Path, bundle_dir: Path | None = None) -> Path:
    """Package MTF-Recomp-v<version>-Linux-x64-Experimental.tar.gz (Experimental Linux source+installer bundle)."""
    out_dir.mkdir(parents=True, exist_ok=True)
    tar_path = out_dir / f"MTF-Recomp-v{version}-Linux-x64-Experimental.tar.gz"
    top = Path(f"MTF-Recomp-v{version}-Linux-x64-Experimental")
    if tar_path.exists():
        tar_path.unlink()
    readme_text = (
        f"MTF Recomp v{version} — Linux x64 (EXPERIMENTAL)\n"
        "================================================\n\n"
        "WARNING: Linux support is EXPERIMENTAL. It is not yet pre-bundled with\n"
        "Linux native libraries or a standalone binary.\n\n"
        "System requirements (install via your distro package manager):\n"
        "  - Python 3.11+ (with python3-tk for GUI)\n"
        "  - CMake, Ninja, Clang / Clang++ (LLD)\n"
        "  - .NET 8/9 Runtime (for tools/shaders/translate.py with bin/xlate)\n"
        "  - Development libraries: Vulkan, SDL3, libvpx, opus, libdatachannel\n\n"
        "Usage:\n"
        "  python3 -m installer\n\n"
        "First-run note: Brief shader/pipeline compilation stutters may occur on\n"
        "your first launch; subsequent runs will be smooth.\n"
    ).encode("utf-8")
    with tarfile.open(tar_path, "w:gz") as tf:
        ti = tarfile.TarInfo((top / "README-LINUX-EXPERIMENTAL.txt").as_posix())
        ti.size = len(readme_text)
        ti.mode = 0o644
        tf.addfile(ti, io.BytesIO(readme_text))
        for rel in PLAYER_FILES:
            src = ROOT / rel
            if src.is_file():
                tf.add(src, arcname=(top / rel).as_posix())
        for rel in PLAYER_DIRS:
            src_dir = ROOT / rel
            if not src_dir.is_dir():
                continue
            for p in sorted(src_dir.rglob("*")):
                if p.is_file() and not should_skip(p.relative_to(ROOT)):
                    tf.add(p, arcname=(top / p.relative_to(ROOT)).as_posix())
        if bundle_dir is not None and (bundle_dir / "bin" / "xlate").is_dir():
            for p in sorted((bundle_dir / "bin" / "xlate").rglob("*")):
                if p.is_file():
                    arc = top / "bin" / "xlate" / p.relative_to(bundle_dir / "bin" / "xlate")
                    tf.add(p, arcname=arc.as_posix())
    return tar_path


def build_runtime_lib(llvm_dir: Path, deps_dir: Path, out_lib_dir: Path) -> None:
    """Pre-compile runtime C/C++ sources (except loader.c/memory.c which include layout.h) into libmtfrt.a + mtf.rc.obj."""
    cmake_text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    m = re.search(r"set\(MTF_RUNTIME_SOURCES\s+(.*?)\r?\n\)", cmake_text, re.S)
    if not m:
        raise RuntimeError("MTF_RUNTIME_SOURCES not found in CMakeLists.txt")
    sources = []
    for raw in m.group(1).splitlines():
        line = raw.split("#", 1)[0].strip()
        if line:
            sources.append(line)
    m_win = re.search(r"if\(WIN32\)\s+list\(APPEND MTF_RUNTIME_SOURCES\s+(.*?)\r?\n\s*\)", cmake_text, re.S)
    if m_win:
        for raw in m_win.group(1).splitlines():
            line = raw.split("#", 1)[0].strip()
            if line:
                sources.append(line)

    obj_root = ROOT / "build" / "rt-objs"
    if obj_root.exists():
        shutil.rmtree(obj_root)
    obj_root.mkdir(parents=True, exist_ok=True)
    out_lib_dir.mkdir(parents=True, exist_ok=True)

    cc = str(llvm_dir / "bin" / "clang.exe")
    cxx_exe = llvm_dir / "bin" / "clang++.exe"
    cxx_cmd = [str(cxx_exe)] if cxx_exe.is_file() else [cc, "--driver-mode=g++"]
    rc = str(llvm_dir / "bin" / "llvm-windres.exe")
    ar = str(llvm_dir / "bin" / "llvm-ar.exe")
    port_ver = (ROOT / "VERSION").read_text(encoding="utf-8").strip() if (ROOT / "VERSION").is_file() else "0.1.0"
    common = [
        "--target=x86_64-w64-windows-gnu", "-O2", "-g", "-Wall", "-fno-strict-aliasing",
        "-march=x86-64-v2", "-D_WIN32_WINNT=0x0A00", "-DWINVER=0x0A00", "-DNDEBUG",
        f'-DMTF_PORT_VERSION="{port_ver}"',
        f"-ffile-prefix-map={ROOT.as_posix()}=.",
        f"-I{(ROOT / 'runtime').as_posix()}",
        f"-I{(ROOT / 'runtime' / 'third_party' / 'imgui').as_posix()}",
        "-isystem", (deps_dir / "include").as_posix(),
        "-isystem", (deps_dir / "include" / "vma").as_posix(),
    ]

    ar_objs: list[str] = []
    split_objs: list[str] = []
    rc_obj_path = out_lib_dir / "mtf.rc.obj"
    tasks: list[tuple[list[str], Path, bool, str]] = []
    for rel in sources:
        src = ROOT / rel
        obj = (obj_root / rel).with_suffix(".obj")
        obj.parent.mkdir(parents=True, exist_ok=True)
        ext = src.suffix.lower()
        if ext == ".rc":
            tasks.append(([rc, "--target=pe-x86-64", "-O", "coff", "-i", str(src), "-o", str(rc_obj_path)], obj, False, rel))
        elif ext == ".cpp":
            if rel == "runtime/vk_vma.cpp":
                extra = ["-std=c++17", "-Wno-nullability-completeness"]
            elif rel in ("runtime/ui/menu.cpp", "runtime/ui/menu_win.cpp", "runtime/ui/option_rows.cpp"):
                extra = ["-std=c++20"]
            elif rel.startswith("runtime/third_party/imgui/"):
                extra = ["-std=c++17", "-w"]
            else:
                extra = ["-std=c++17"]
            tasks.append(([*cxx_cmd, *common, *extra, "-c", str(src), "-o", str(obj)], obj, True, rel))
        elif rel == "runtime/game_host.c":
            split_obj = (obj_root / "runtime" / "game_host_split.obj")
            tasks.append(([cc, *common, "-std=gnu17", "-c", str(src), "-o", str(obj)], obj, "ar", rel))
            tasks.append(([cc, *common, "-DMTF_RUNTIME_SPLIT", "-std=gnu17", "-c", str(src), "-o", str(split_obj)], split_obj, "split", rel))
        else:
            tasks.append(([cc, *common, "-std=gnu17", "-c", str(src), "-o", str(obj)], obj, True, rel))

    def _run(cmd_obj: tuple[list[str], Path, bool, str]) -> tuple[Path, bool, str]:
        cmd, obj, is_ar, rel = cmd_obj
        subprocess.check_call(cmd)
        return obj, is_ar, rel

    with concurrent.futures.ThreadPoolExecutor() as pool:
        for obj, kind, rel in pool.map(_run, tasks):
            if kind == "split":
                split_objs.append(str(obj))
            elif kind == "ar":
                ar_objs.append(str(obj))
            elif kind:
                split_objs.append(str(obj))
                if rel not in ("runtime/loader.c", "runtime/memory.c"):
                    ar_objs.append(str(obj))

    lib_path = out_lib_dir / "libmtfrt.a"
    if lib_path.exists():
        lib_path.unlink()
    subprocess.check_call([ar, "rcs", str(lib_path), *ar_objs])
    print(f"Built {lib_path} ({lib_path.stat().st_size / (1024 * 1024):.2f} MiB)")

    rt_exe = out_lib_dir.parent / "bin" / "mtf-recomp.exe"
    rt_implib = out_lib_dir / "libmtf-recomp.dll.a"
    rt_exe.parent.mkdir(parents=True, exist_ok=True)
    rt_rsp = obj_root / "rt_split.rsp"
    rt_rsp.write_text("\n".join(f'"{Path(o).as_posix()}"' for o in [*split_objs, str(rc_obj_path)]) + "\n", encoding="utf-8")
    subprocess.check_call([
        *cxx_cmd, "--target=x86_64-w64-windows-gnu", "-O2", "-g", "-march=x86-64-v2",
        "-fuse-ld=lld", "-Wl,--stack,16777216", "-Wl,--error-limit=0",
        f"-Wl,--export-all-symbols,--out-implib,{rt_implib.as_posix()}",
        f"@{rt_rsp.as_posix()}", f"-L{out_lib_dir.as_posix()}",
        "-lvulkan-1", "-lSDL3", "-lopus", "-lvpx", "-ldatachannel", "-ljuice", "-lsrtp2",
        "-lusrsctp", "-lmbedtls", "-lmbedx509", "-lmbedcrypto", "-leverest", "-lp256m",
        "-liphlpapi", "-lpsapi", "-lbcrypt", "-luser32", "-lgdi32", "-ldwmapi", "-limm32",
        "-lwinmm", "-lole32", "-loleaut32", "-lmfplat", "-lmfuuid", "-lshell32", "-luuid",
        "-lpdh", "-lrstrtmgr", "-lws2_32", "-lwinhttp", "-o", str(rt_exe),
    ])
    print(f"Built split runtime {rt_exe} ({rt_exe.stat().st_size / (1024 * 1024):.2f} MiB)")
    shutil.rmtree(obj_root, ignore_errors=True)

    updater_bin = out_lib_dir.parent / "bin" / "mtf-updater.exe"
    updater_bin.parent.mkdir(parents=True, exist_ok=True)
    subprocess.check_call([cc, "--target=x86_64-w64-windows-gnu", "-O2", "-Wall", "-mwindows", "-fuse-ld=lld",
                           str(ROOT / "tools" / "mtf_updater.c"), "-lcomctl32", "-o", str(updater_bin)])
    print(f"Built {updater_bin}")


def main() -> int:
    ap = argparse.ArgumentParser(description="Create a clean MTF Recomp release archive")
    ap.add_argument("--version", default="0.1.0-dev", help="Release version tag")
    ap.add_argument("--out", type=Path, default=ROOT / "build" / "release", help="Output directory")
    ap.add_argument("--windows-zip", action="store_true", help="Also build MTF-Recomp-v<version>-Windows-x64.zip")
    ap.add_argument("--linux-tar", action="store_true", help="Also build MTF-Recomp-v<version>-Linux-x64-Experimental.tar.gz")
    ap.add_argument("--bundle-dir", type=Path, default=None, help="Optional self-contained toolchain dir (python/llvm/cmake/bin/deps) to include in Windows zip")
    ap.add_argument("--make-runtime-lib", action="store_true", help="Build libmtfrt.a + mtf.rc.obj into <bundle-dir>/deps/lib before packaging")
    args = ap.parse_args()
    if args.make_runtime_lib:
        bdir = (args.bundle_dir or (ROOT / "build" / "release-bundle")).resolve()
        llvm = next((p for p in (ROOT / "toolchain").glob("llvm-mingw-*") if p.is_dir()), bdir / "llvm")
        deps = (ROOT / "toolchain" / "deps-mingw") if (ROOT / "toolchain" / "deps-mingw").is_dir() else (bdir / "deps")
        build_runtime_lib(llvm, deps, bdir / "deps" / "lib")
    zip_path = make_release(args.version, args.out)
    size_mb = zip_path.stat().st_size / (1024 * 1024)
    print(f"Created clean release archive: {zip_path} ({size_mb:.2f} MiB)")
    if args.windows_zip:
        onedir = args.out / "MTF-Recomp-Setup"
        setup_path = onedir if onedir.is_dir() else (args.out / "MTF-Recomp-Setup.exe")
        win_zip = make_windows_zip(args.version, args.out, setup_path, args.bundle_dir)
        win_mb = win_zip.stat().st_size / (1024 * 1024)
        print(f"Created RecompLauncher Windows archive: {win_zip} ({win_mb:.2f} MiB)")
        rt_zip = make_runtime_zip(args.version, args.out, args.bundle_dir)
        if rt_zip is not None:
            rt_mb = rt_zip.stat().st_size / (1024 * 1024)
            print(f"Created Fast Runtime Update archive: {rt_zip} ({rt_mb:.2f} MiB)")
    if args.linux_tar:
        lin_tar = make_linux_tar(args.version, args.out, args.bundle_dir)
        lin_mb = lin_tar.stat().st_size / (1024 * 1024)
        print(f"Created Experimental Linux archive: {lin_tar} ({lin_mb:.2f} MiB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
