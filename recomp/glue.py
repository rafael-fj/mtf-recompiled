"""Write the runtime's module glue (modules.c, layout.h) for the lifted modules.

The runtime (runtime/) loads and relocates
the lifted C. It needs, per build: the module layout the generator used, each linked module's function table, the
expected SHA-256 of every ExeFS file it reads, and the default content paths. This tool collects those from the
generator output directories and the pinned title profile, checks that they agree, and writes them under
build/generated/glue/. Game-derived values (hashes, paths) stay in that ignored directory.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PROFILE = PROJECT_ROOT / "profiles" / "0400D4201C6EA000" / "profile.json"
DEFAULT_EXEFS = PROJECT_ROOT / "build" / "content" / "0400D4201C6EA000" / "updates" / "327680" / "exefs"
DEFAULT_ROMFS = PROJECT_ROOT / "build" / "content" / "0400D4201C6EA000" / "updates" / "327680" / "romfs"
DEFAULT_USER = PROJECT_ROOT / "build" / "user"
DEFAULT_OUT = PROJECT_ROOT / "build" / "generated" / "glue"
EXEFS_FILES = ("main", "sdk", "main.npdm")
PORT_VERSION = (PROJECT_ROOT / "VERSION").read_text(encoding="utf-8").strip() if (PROJECT_ROOT / "VERSION").is_file() else "0.1.0"


def _c_string(text: str) -> str:
    return json.dumps(text)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def _read_layout(generated_dir: Path) -> list[tuple[str, int, int]]:
    data = json.loads((generated_dir / "layout.json").read_text(encoding="utf-8"))
    return [(entry["name"], int(entry["base"], 16), int(entry["size"], 16)) for entry in data]


def _linked_module(generated_dir: Path) -> list[str]:
    """Module names whose function table this generator directory holds."""
    return sorted(child.name for child in generated_dir.iterdir()
                  if child.is_dir() and (child / f"{child.name}_table.c").is_file())


def collect(generated_dirs: list[Path], exefs: Path, profile_path: Path) -> dict[str, object]:
    # The static modules' output knows main and sdk; the NRO's output (recomp/nro_lift.py) also knows the
    # runtime-loaded nro after them. The longest layout is used; every module two layouts share must agree.
    layouts = [_read_layout(directory) for directory in generated_dirs]
    layout = max(layouts, key=len)
    for other in layouts:
        if other != layout[:len(other)]:
            raise ValueError("the generator directories were written with different module layouts")
    linked: list[str] = []
    for directory in generated_dirs:
        linked.extend(_linked_module(directory))
    layout_names = [name for name, _, _ in layout]
    unknown = sorted(set(linked) - set(layout_names))
    if unknown:
        raise ValueError(f"lifted modules {unknown} are not in the layout {layout_names}")

    profile = json.loads(profile_path.read_text(encoding="utf-8"))
    hashes = {}
    for name in EXEFS_FILES:
        path = exefs / name
        if not path.is_file():
            raise FileNotFoundError(f"{path} is missing; extract the game first (README.md)")
        hashes[name] = _sha256(path)
    for name in ("main", "sdk"):
        pinned = profile["modules"][name]["sha256"]
        if hashes[name] != pinned:
            raise ValueError(f"{exefs / name} has SHA-256 {hashes[name]}, the v1.1.1 profile pins {pinned}")
    layout_dir = generated_dirs[[len(other) for other in layouts].index(len(layout))]
    return {"layout": layout, "layout_dir": layout_dir, "linked": linked, "hashes": hashes, "profile": profile}


def module_glue(info: dict[str, object], exefs: Path, romfs: Path, user: Path, portable: bool = False) -> str:
    layout = info["layout"]
    linked = info["linked"]
    hashes = info["hashes"]
    out = ["// Module glue written by recomp/glue.py; do not edit.",
           '#include "dispatch.h"', '#include "game_abi.h"', '#include "host.h"', '#include "layout.h"', ""]
    for name, base, size in layout:
        upper = name.upper()
        out.append(f"_Static_assert(MOD_{upper}_BASE == {base:#x}ull && MOD_{upper}_SIZE == {size:#x}ull, "
                   f'"layout.h and layout.json disagree on {name}");')
    out += ["", "static const RtLayoutEntry glue_layout[] = {"]
    out += [f'    {{"{name}", MOD_{name.upper()}_BASE, MOD_{name.upper()}_SIZE}},' for name, _, _ in layout]
    out += ["};", ""]
    for name in linked:
        out += [f"extern const uint64_t {name}_fn_addrs[];", f"extern const GuestFn {name}_fn_ptrs[];",
                f"extern const uint64_t {name}_fn_count;"]
    out += ["", "static int glue_linked_modules(RtModule *out, int max) {", "    int n = 0;"]
    for name in linked:
        upper = name.upper()
        out.append(f'    if (n < max) out[n++] = (RtModule){{"{name}", MOD_{upper}_BASE, MOD_{upper}_SIZE, '
                   f"{name}_fn_addrs, {name}_fn_ptrs, {name}_fn_count}};")
    out += ["    return n;", "}", "", "static const RtFileHash glue_exefs_sha256[] = {"]
    out += [f"    {{{_c_string(name)}, {_c_string(digest)}}}," for name, digest in sorted(hashes.items())]
    out += ["};", ""]
    def where(p: Path) -> str:   # relative paths stay relative: the runtime takes them from the exe's folder
        return p.as_posix() if not p.is_absolute() and portable else p.resolve().as_posix()
    out += [f"static const char glue_default_exefs[] = {_c_string(where(exefs))};",
            f"static const char glue_default_romfs[] = {_c_string(where(romfs))};",
            f"static const char glue_default_profile[] = {_c_string(where(user))};",
            "",
            "#ifdef MTF_GAME_DLL",
            "__declspec(dllexport)",
            "#endif",
            "const MtfGameDesc *mtf_game_desc(void) {",
            "    static const MtfGameDesc desc = {",
            "        MTF_GAME_ABI_VERSION,",
            "        glue_layout,",
            f"        {len(layout)},",
            "        glue_linked_modules,",
            "        glue_exefs_sha256,",
            f"        {len(hashes)},",
            "        glue_default_exefs,",
            "        glue_default_romfs,",
            "        glue_default_profile,",
            "        111,",
            "    };",
            "    return &desc;",
            "}", ""]
    return "\n".join(out)


def _write_if_changed(path: Path, text: str) -> None:
    if path.is_file() and path.read_text(encoding="utf-8") == text:
        return
    path.write_text(text, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--generated", type=Path, nargs="+", required=True,
                        help="generator output directories (each holds layout.json and <module>/)")
    parser.add_argument("--exefs", type=Path, default=DEFAULT_EXEFS)
    parser.add_argument("--romfs", type=Path, default=DEFAULT_ROMFS)
    parser.add_argument("--user", type=Path, default=DEFAULT_USER, help="save data and settings directory")
    parser.add_argument("--profile", type=Path, default=DEFAULT_PROFILE)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--portable", action="store_true",
                        help="an installed folder: --exefs, --romfs and --user are relative to mtf-recomp.exe's folder")
    parser.add_argument("--check-exefs", type=Path, default=None,
                        help="with --portable: the ExeFS to read the hashes from now (default: --exefs)")
    args = parser.parse_args()

    info = collect(args.generated, args.check_exefs or args.exefs, args.profile)
    args.out.mkdir(parents=True, exist_ok=True)
    _write_if_changed(args.out / "layout.h", (info["layout_dir"] / "layout.h").read_text(encoding="utf-8"))
    _write_if_changed(args.out / "modules.c", module_glue(info, args.exefs, args.romfs, args.user, args.portable))
    print(f"glue: {len(info['layout'])} modules in the layout, linked {', '.join(info['linked'])}; "
          f"wrote {args.out / 'modules.c'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
