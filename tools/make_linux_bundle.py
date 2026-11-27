#!/usr/bin/env python3
"""Build a self-contained Experimental Linux x64 release bundle on Ubuntu 24.04."""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import re
import shutil
import stat
import subprocess
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

PLAYER_DIRS = ["installer", "profiles", "recomp", "runtime", "tools"]
PLAYER_FILES = ["LICENSE", "README.md", "THIRD_PARTY_NOTICES.md", "CMakeLists.txt", "VERSION", "recomplauncher.json"]
SKIP_NAMES = {"__pycache__", ".pytest_cache", ".mypy_cache", ".git", ".DS_Store"}


def should_skip(rel: Path) -> bool:
    return any(part in SKIP_NAMES for part in rel.parts) or rel.suffix == ".pyc"


def _write_exec(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")
    path.chmod(0o755)


def _zig_wrapper(tool: str) -> str:
    return (
        '#!/usr/bin/env sh\n'
        'HERE="$(cd "$(dirname "$0")" && pwd)"\n'
        'n=$#\n'
        'i=0\n'
        'while [ "$i" -lt "$n" ]; do\n'
        '  a="$1"; shift\n'
        '  case "$a" in\n'
        '    -march=x86-64-v2) set -- "$@" -mcpu=x86_64_v2 ;;\n'
        '    -march=x86-64-v3) set -- "$@" -mcpu=x86_64_v3 ;;\n'
        '    *) set -- "$@" "$a" ;;\n'
        '  esac\n'
        '  i=$((i + 1))\n'
        'done\n'
        f'exec "$HERE/../zig/zig" {tool} -target x86_64-linux-gnu.2.31 "$@"\n'
    )


def setup_zig_wrappers(zig_dir: Path, llvm_dir: Path) -> None:
    """Expose Zig cc/c++/ar as llvm/bin/{clang,clang++,llvm-ar} targeting glibc 2.31."""
    bin_dir = llvm_dir / "bin"
    bin_dir.mkdir(parents=True, exist_ok=True)
    zig_bin = zig_dir / "zig"
    if not zig_bin.is_file():
        raise FileNotFoundError(f"zig binary not found at {zig_bin}")
    _write_exec(bin_dir / "clang", _zig_wrapper("cc"))
    _write_exec(bin_dir / "clang++", _zig_wrapper("c++"))
    _write_exec(
        bin_dir / "llvm-ar",
        '#!/usr/bin/env sh\n'
        'HERE="$(cd "$(dirname "$0")" && pwd)"\n'
        'exec "$HERE/../zig/zig" ar "$@"\n',
    )


def build_linux_runtime_lib(llvm_dir: Path, deps_dir: Path, out_lib_dir: Path) -> Path:
    """Pre-compile Linux MTF_RUNTIME_SOURCES into libmtfrt.a using the bundled Zig/Clang."""
    cmake_text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    m = re.search(r"set\(MTF_RUNTIME_SOURCES\s+(.*?)\r?\n\)", cmake_text, re.S)
    if not m:
        raise RuntimeError("MTF_RUNTIME_SOURCES not found in CMakeLists.txt")
    sources = [raw.split("#", 1)[0].strip() for raw in m.group(1).splitlines() if raw.split("#", 1)[0].strip()]
    sources += [
        "runtime/gfx_window_sdl.c",
        "runtime/platform/linux.c",
        "runtime/ui/menu_sdl.cpp",
        "runtime/stream_linux_stub.c",
    ]

    obj_root = ROOT / "build" / "rt-objs-linux"
    if obj_root.exists():
        shutil.rmtree(obj_root)
    obj_root.mkdir(parents=True, exist_ok=True)
    out_lib_dir.mkdir(parents=True, exist_ok=True)

    cc = str(llvm_dir / "bin" / "clang")
    cxx = str(llvm_dir / "bin" / "clang++")
    ar = str(llvm_dir / "bin" / "llvm-ar")
    port_ver = (ROOT / "VERSION").read_text(encoding="utf-8").strip() if (ROOT / "VERSION").is_file() else "1.0.0"

    common = [
        "-O2", "-g", "-Wall", "-fno-strict-aliasing", "-march=x86-64-v2",
        "-D_GNU_SOURCE", "-DNDEBUG", f'-DMTF_PORT_VERSION="{port_ver}"',
        f"-ffile-prefix-map={ROOT.as_posix()}=.",
        f"-I{(ROOT / 'runtime').as_posix()}",
        f"-I{(ROOT / 'runtime' / 'third_party' / 'imgui').as_posix()}",
        "-isystem", (deps_dir / "include").as_posix(),
        "-isystem", (deps_dir / "include" / "vma").as_posix(),
    ]

    tasks: list[tuple[list[str], str]] = []
    ar_objs: list[str] = []
    for rel in sources:
        if rel in ("runtime/loader.c", "runtime/memory.c"):
            continue
        src = ROOT / rel
        obj = (obj_root / rel).with_suffix(".o")
        obj.parent.mkdir(parents=True, exist_ok=True)
        ar_objs.append(str(obj))
        ext = src.suffix.lower()
        if ext == ".cpp":
            if rel == "runtime/vk_vma.cpp":
                extra = ["-std=c++17", "-Wno-nullability-completeness"]
            elif rel == "runtime/ui/menu.cpp":
                extra = ["-std=c++20", f"-Wa,-I{(ROOT / 'runtime' / 'ui').as_posix()}"]
            elif rel in ("runtime/ui/menu_sdl.cpp", "runtime/ui/option_rows.cpp"):
                extra = ["-std=c++20"]
            elif rel.startswith("runtime/third_party/imgui/"):
                extra = ["-std=c++17", "-w"]
            else:
                extra = ["-std=c++17"]
            cmd = [cxx, *common, *extra, "-c", str(src), "-o", str(obj)]
        else:
            cmd = [cc, *common, "-std=gnu17", "-c", str(src), "-o", str(obj)]
        tasks.append((cmd, rel))

    def _run(item: tuple[list[str], str]) -> None:
        cmd, rel = item
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            raise RuntimeError(f"Failed compiling {rel}:\n{res.stdout}\n{res.stderr}")

    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        list(pool.map(_run, tasks))

    lib_path = out_lib_dir / "libmtfrt.a"
    if lib_path.exists():
        lib_path.unlink()
    subprocess.check_call([ar, "rcs", str(lib_path), *ar_objs])
    shutil.rmtree(obj_root, ignore_errors=True)
    print(f"Built {lib_path} ({lib_path.stat().st_size / (1024 * 1024):.2f} MiB)")
    return lib_path


def package_linux_tar(version: str, out_dir: Path, bundle_dir: Path) -> Path:
    out_dir.mkdir(parents=True, exist_ok=True)
    tar_path = out_dir / f"MTF-Recomp-v{version}-Linux-x64-Experimental.tar.gz"
    top = Path(f"MTF-Recomp-v{version}-Linux-x64-Experimental")
    if tar_path.exists():
        tar_path.unlink()

    _write_exec(
        bundle_dir / "MTF-Recomp-Setup",
        '#!/usr/bin/env sh\n'
        'HERE="$(cd "$(dirname "$0")" && pwd)"\n'
        'exec "$HERE/python/bin/python3" -m installer.gui "$@"\n',
    )
    _write_exec(
        bundle_dir / "MTF-Recomp-Setup-CLI",
        '#!/usr/bin/env sh\n'
        'HERE="$(cd "$(dirname "$0")" && pwd)"\n'
        'exec "$HERE/python/bin/python3" -m installer "$@"\n',
    )
    (bundle_dir / "README-LINUX-EXPERIMENTAL.txt").write_text(
        f"MTF Recomp v{version} — Linux x64 (EXPERIMENTAL, Self-Contained)\n"
        "===============================================================\n\n"
        "This is a self-contained experimental Linux x64 bundle (includes Python,\n"
        "Zig/Clang, Ninja, .NET 10 runtime + xlate, SDL3, libvpx, opus, libmtfrt.a).\n\n"
        "Usage:\n"
        "  ./MTF-Recomp-Setup        (GUI installer)\n"
        "  ./MTF-Recomp-Setup-CLI    (CLI installer)\n\n"
        "Only a Vulkan 1.3 capable GPU driver (Mesa RADV / NVIDIA) is required.\n"
        "First-run note: Brief shader/pipeline compilation stutters may occur on\n"
        "your first launch; subsequent runs will be smooth.\n",
        encoding="utf-8",
    )

    with tarfile.open(tar_path, "w:gz") as tf:
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
        for p in sorted(bundle_dir.rglob("*")):
            if p.is_file() or p.is_symlink():
                tf.add(p, arcname=(top / p.relative_to(bundle_dir)).as_posix(), recursive=False)
    print(f"Created self-contained Experimental Linux archive: {tar_path} ({tar_path.stat().st_size / (1024 * 1024):.2f} MiB)")
    return tar_path


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True)
    ap.add_argument("--bundle-dir", type=Path, required=True)
    ap.add_argument("--out", type=Path, default=ROOT / "build" / "release")
    args = ap.parse_args()

    bdir = args.bundle_dir.resolve()
    zig_dir = bdir / "llvm" / "zig"
    if zig_dir.is_dir():
        setup_zig_wrappers(zig_dir, bdir / "llvm")
    build_linux_runtime_lib(bdir / "llvm", bdir / "deps", bdir / "deps" / "lib")
    package_linux_tar(args.version, args.out.resolve(), bdir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
