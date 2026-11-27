"""Translate the extracted shaders (tools/shaders/extract.py) to SPIR-V with the Ampere translator.

The translator is tools/xlate (a Maxwell/Ampere shader translator library and a small front end), built from source
into build/toolchain/dmsh_xlate-sbuf. For each NVN2 shader it gets: the code area cut to the instructions (a 0x80-byte SPH and
128-bit instructions; control +0x38 long), the shader's c1 constant segment (code area between control +0x40 and +0x44),
the Ampere flag, compute's driver constant layout (control +0xa9..+0xba) and the profile's answers
(profiles/<id>/shader_facts.tsv). It writes <key>.spv and <key>.res.json, the reflection the runtime binds by.
It is built with
  dotnet build tools/xlate/dmsh_xlate.csproj -c Release -o build/toolchain/dmsh_xlate-sbuf
Every job carries sbuf_align (--sbuf-align, 16): storage buffers are addressed from their guest address rounded down to
it, which is where the runtime binds them (vk_caps.c, vk_exec_draw.c ssbo_info).

The job layout is adapted code (THIRD_PARTY_NOTICES.md). Task and mesh
shaders (none in this game) are refused.
"""

from __future__ import annotations

import argparse
import concurrent.futures as cf
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SRC = PROJECT_ROOT / "build" / "generated" / "shaders" / "src"
DEFAULT_OUT = PROJECT_ROOT / "build" / "generated" / "shaders" / "spv"
DEFAULT_FACTS = PROJECT_ROOT / "profiles" / "0400D4201C6EA000" / "shader_facts.tsv"
DEFAULT_ANSWERS = PROJECT_ROOT / "profiles" / "0400D4201C6EA000" / "shader_answers.tsv"   # always applied
DEFAULT_XLATE = PROJECT_ROOT / "build" / "toolchain" / "dmsh_xlate-sbuf" / "dmsh_xlate.dll"
SBUF_ALIGN = 16   # vk_caps.c PREBUILT_SBUF_ALIGN: the runtime binds storage buffers at their address rounded down to it
TESS_MODE_DEFAULT = 0x202   # quads, equal spacing, counter-clockwise, connected: what this game's hull shaders declare
DOTNET = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "dotnet" / "dotnet.exe"


def load_facts(path: Path) -> dict:
    facts = {}
    if path.is_file():
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.strip() and not line.startswith("#"):
                key, value = line.split("\t", 1)
                facts[key] = json.loads(value)
    return facts


def load_answers(path: Path) -> dict:
    """Run-time answers the runtime wrote (gfx_shader.c gfx_shader_answer_note): key <TAB> attr|fout <TAB> slot <TAB>
    float|sint|uint, one per line; the last one for a slot wins. Returned as {key: {"attr": {...}, "fout": {...}}}."""
    answers: dict = {}
    if path.is_file():
        for line in path.read_text(encoding="utf-8").splitlines():
            parts = line.split("	")
            if len(parts) == 4 and not line.startswith("#") and parts[1] in ("attr", "fout"):
                answers.setdefault(parts[0], {"attr": {}, "fout": {}})[parts[1]][parts[2]] = parts[3]
    return answers


def job_for(key: str, stage: str, src: Path, work: Path, facts: dict, sbuf_align: int = SBUF_ALIGN, mvprev: bool = False) -> dict:
    ctl = (src / f"{key}.ctl").read_bytes()
    code = (src / f"{key}.code").read_bytes()
    c1_start, c1_end = struct.unpack_from("<II", ctl, 0x40)
    c1 = code[c1_start:c1_end] if c1_start <= c1_end <= len(code) else b""
    code = code[:struct.unpack_from("<I", ctl, 0x38)[0]]
    code_path = work / f"{key}.bin"
    code_path.write_bytes(code)
    job = {"key": key, "stage": stage, "code": str(code_path), "arch": "sm86", "tex": {}, "img": {}, "attr": {},
           "fout": {}, "c1": c1.hex(), "debug": True, "reduced": True}   # the translator's defaults
    if sbuf_align:
        job["sbuf_align"] = sbuf_align
    if mvprev:
        job["mvprev"] = True   # the vertex shader also writes last frame's position (patches/0015)
    if stage == "cs":
        # compute's own c[0] layout: uniform buffer, storage buffer, texture counts, its size, push constant bytes,
        # separate texture and sampler counts (InstEmitAmpere.cs AMapC0)
        job["nvn2_cs"] = [ctl[0xB1], ctl[0xB2], ctl[0xB3], struct.unpack_from("<H", ctl, 0xBA)[0], ctl[0xA9] * 4,
                          ctl[0xB4], ctl[0xB5]]
    if stage in ("hs", "ds"):
        # TESS_MODE, the GPU state the NVN driver sets from the program (Program.cs QueryGraphicsState): control +0x5c in
        # this game's hull shaders and in one of its two domain shaders, 0x202 = quads, equal spacing, counter-clockwise,
        # connected; the other domain shader holds something else there (0x8e7c), so a value with bits outside the mode's
        # takes the one every hull shader of the game declares
        mode = struct.unpack_from("<I", ctl, 0x5C)[0]
        job["tess_mode"] = mode if mode and not (mode & ~0x333) else TESS_MODE_DEFAULT
    fact = dict(facts.get(key, {}))
    for kind in ("attr", "fout"):   # the runtime's answers (load_answers) on top of the profile's facts
        if kind in fact:
            job[kind].update(fact.pop(kind))
    job.update(fact)
    return job


def run_batch(batch: list, src: Path, out: Path, facts: dict, xlate: Path, sbuf_align: int = SBUF_ALIGN, mvprev: bool = False) -> list:
    with tempfile.TemporaryDirectory(prefix="mtf-xlate-", dir=out.parent) as temp:
        work = Path(temp)
        jobs = [job_for(key, stage, src, work, facts, sbuf_align, mvprev) for key, stage in batch]
        (work / "jobs.json").write_text(json.dumps(jobs), encoding="utf-8")
        # the release ships a self-contained translator (an .exe or wrapper); a developer's build is a .dll for dotnet
        host = [str(DOTNET), str(xlate)] if xlate.suffix.lower() == ".dll" else [str(xlate)]
        result = subprocess.run([*host, str(work / "jobs.json"), str(work / "out")],
                                capture_output=True, text=True)
        failed = []
        for key, _ in batch:
            spv, res = work / "out" / f"{key}.spv", work / "out" / f"{key}.res.json"
            if spv.is_file() and res.is_file():
                suffix = ".mvprev" if mvprev else ""   # <key>.mvprev.spv: the variant beside the shader
                shutil.move(str(spv), out / f"{key}{suffix}.spv")
                shutil.move(str(res), out / f"{key}{suffix}.res.json")
                reader = work / "out" / f"{key}.reader.spv"
                if reader.is_file():
                    shutil.move(str(reader), out / reader.name)
            else:
                failed.append(key)
        if failed and result.returncode:
            (out / "failures.log").open("a", encoding="utf-8").write(
                f"batch of {len(batch)} (exit {result.returncode}): {', '.join(failed[:8])}\n{result.stderr[-4000:]}\n")
        return failed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--src", type=Path, default=DEFAULT_SRC)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--facts", type=Path, default=DEFAULT_FACTS)
    parser.add_argument("--xlate", type=Path, default=DEFAULT_XLATE)
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 2))
    parser.add_argument("--batch", type=int, default=200)
    parser.add_argument("--limit", type=int, default=0, help="translate only the first N shaders (a smoke run)")
    parser.add_argument("--answers", type=Path, default=None,
                        help="the runtime's answers.tsv: translate the shaders it names again with those answers")
    parser.add_argument("--sbuf-align", type=int, default=SBUF_ALIGN,
                        help="storage buffers addressed from their address rounded down to this (0: exact)")
    parser.add_argument("--force", action="store_true", help="translate the given keys again even if they are on disk")
    parser.add_argument("--keys-file", type=Path, default=None, help="only the keys listed in this file, one a line")
    parser.add_argument("--mvprev", action="store_true",
                        help="the vertex shaders' last-frame variants for DLAA's per-object motion: <key>.mvprev.spv")
    parser.add_argument("keys", nargs="*", help="only these keys")
    args = parser.parse_args()
    if not args.xlate.is_file():
        sys.exit(f"{args.xlate} is missing: build the translator first (README.md)")

    rows = [line.split("\t")[:2] for line in (args.src / "shaders.tsv").read_text(encoding="utf-8").splitlines()
            if line and not line.startswith("#")]
    if args.keys_file:
        args.keys += [k.strip() for k in args.keys_file.read_text(encoding="utf-8").split() if k.strip()]
    if args.keys:
        rows = [r for r in rows if r[0] in set(args.keys)]
    rows = [r for r in rows if r[1] not in ("ts", "ms")]
    args.out.mkdir(parents=True, exist_ok=True)
    facts = load_facts(args.facts)
    # The profile's answers (types the pipeline gives) go into every translation; --answers names shaders to redo.
    profile_answers = load_answers(DEFAULT_ANSWERS)
    answers = load_answers(args.answers) if args.answers else {}
    for key, ans in list(profile_answers.items()) + list(answers.items()):
        fact = facts.setdefault(key, {})
        for kind in ("attr", "fout"):
            if ans[kind]:
                fact[kind] = {**fact.get(kind, {}), **ans[kind]}
    if args.mvprev:
        rows = [r for r in rows if r[1] == "vs"]
        if not args.force:
            rows = [r for r in rows if not (args.out / f"{r[0]}.mvprev.spv").is_file()]
    elif args.force:
        pass   # the given keys again, whatever is on disk
    elif args.answers:
        rows = [r for r in rows if r[0] in answers]   # these again, whatever is on disk
    else:
        rows = [r for r in rows if not (args.out / f"{r[0]}.spv").is_file()]   # done in an earlier run
    if args.limit:
        rows = rows[:args.limit]
    batches = [rows[i:i + args.batch] for i in range(0, len(rows), args.batch)]
    failed: list = []
    with cf.ThreadPoolExecutor(args.jobs) as pool:
        futures = [pool.submit(run_batch, b, args.src, args.out, facts, args.xlate, args.sbuf_align, args.mvprev) for b in batches]
        for done, fut in enumerate(cf.as_completed(futures), 1):
            failed += fut.result()
            print(f"\r{done}/{len(batches)} batches, {len(failed)} failed", end="", flush=True)
    print()
    total = len(list(args.out.glob("*.spv"))) - len(list(args.out.glob("*.reader.spv")))
    print(f"translated {len(rows) - len(failed)} of {len(rows)} this run; {total} SPIR-V shaders in {args.out}")
    if failed:
        (args.out / "failed.txt").write_text("\n".join(failed) + "\n", encoding="utf-8")
        print(f"{len(failed)} failed: {args.out / 'failed.txt'}, details in failures.log")
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
