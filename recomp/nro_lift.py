"""Verify a captured runtime NRO against its NRR, then lift it as a static module."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
GENERATOR = Path(__file__).resolve().parent / "gen.py"
PROFILE = PROJECT_ROOT / "profiles" / "0400D4201C6EA000" / "profile.json"
sys.path.insert(0, str(Path(__file__).resolve().parent))

from layout import layout  # noqa: E402
from nso import Nso  # noqa: E402
from nro_capture import verify_capture  # noqa: E402


def _inside_build_generated(path: Path) -> bool:
    generated = Path(os.environ.get("MTF_GENERATED_ROOT") or PROJECT_ROOT / "build" / "generated").resolve()
    resolved = path.resolve()
    return resolved == generated or generated in resolved.parents


def _link_or_copy(source: Path, destination: Path) -> None:
    try:
        os.link(source, destination)
    except OSError:
        shutil.copyfile(source, destination)


def recompile_nro(exefs: Path, nro_path: Path, output_dir: Path, jobs: int = 1) -> dict[str, object]:
    exefs = Path(exefs).resolve()
    nro_path = Path(nro_path).resolve()
    output_dir = Path(output_dir).resolve()
    if not _inside_build_generated(output_dir):
        raise ValueError("NRO generated code must stay under build/generated/")
    if not exefs.is_dir() or not nro_path.is_file():
        raise FileNotFoundError("provide the matching update ExeFS and captured NRO")
    profile = json.loads(PROFILE.read_text(encoding="utf-8"))
    for name, expected in profile["modules"].items():
        path = exefs / name
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected["sha256"]:
            raise ValueError(f"{name} does not match the pinned v1.1.1 module hash")

    output_dir.mkdir(parents=True, exist_ok=True)
    verified_dir = output_dir / "verified-capture"
    capture = verify_capture(exefs.parent / "romfs" / ".nrr" / "nrr.nrr", verified_dir, nro_path, PROFILE)
    verified_nro = Path(capture["nro_path"])
    generated_root = Path(os.environ.get("MTF_GENERATED_ROOT") or PROJECT_ROOT / "build" / "generated").resolve()
    with tempfile.TemporaryDirectory(prefix="mtf-nro-stage-", dir=generated_root) as temporary:
        stage_root = Path(temporary).resolve()
        if not _inside_build_generated(stage_root):
            raise ValueError("temporary NRO staging directory escaped build/generated/")
        stage_exefs = stage_root / "exefs"
        stage_exefs.mkdir()
        for name in ("main", "sdk"):
            _link_or_copy(exefs / name, stage_exefs / name)
        _link_or_copy(verified_nro, stage_exefs / "nro")
        modules = {name: (base, size) for name, base, size in layout(stage_exefs)}
        if "nro" not in modules:
            raise ValueError("the captured image was not recognized as an NRO module")
        nro_base, nro_span = modules["nro"]
        expected_nro = profile["runtime_loaded_nro"]
        if nro_base != int(expected_nro["guest_base"], 16):
            raise ValueError(f"NRO base {nro_base:#x} differs from pinned base {expected_nro['guest_base']}")
        if nro_span > int(expected_nro["maximum_guest_span"], 16):
            raise ValueError("NRO plus BSS exceeds the reserved guest address range")
        command = [sys.executable, str(GENERATOR), "--exefs", str(stage_exefs), "--out", str(output_dir),
                   "--modules", "nro", "--jobs", str(jobs)]
        patches_file = PROJECT_ROOT / "profiles" / "0400D4201C6EA000" / "patches.txt"
        if patches_file.is_file():
            command.extend(["--patches", str(patches_file)])
        subprocess.run(command, cwd=PROJECT_ROOT, check=True)
        nso = Nso(str(stage_exefs / "nro"))

    manifest_path = output_dir / "nro" / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    stats = manifest["stats"]
    report = {
        "title_id": profile["title_id"],
        "profile_version": profile["profile_version"],
        "nro_sha256": capture["candidate_sha256"],
        "build_id": nso.build_id,
        "guest_base": f"{nro_base:#x}",
        "image_size": f"{nro_span:#x}",
        "text_size": f"{nso.text[1]:#x}",
        "function_count": manifest["function_count"],
        "emitted_functions": stats["emitted_functions"],
        "instructions": stats["instructions"],
        "traps": stats["traps"],
        "unsupported": stats["unsupported"],
        "unsupported_census": manifest["unsupported_census"],
        "generated_c_bytes": stats["c_bytes"],
        "chunks": manifest["chunks"],
    }
    (output_dir / "nro-recompile-report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exefs", type=Path, required=True)
    parser.add_argument("--nro", type=Path, required=True)
    parser.add_argument("--out", type=Path, default=Path("build/generated/nro/0400D4201C6EA000/1.1.1/recompiled"))
    parser.add_argument("--jobs", type=int, default=1)
    args = parser.parse_args()
    output = args.out if args.out.is_absolute() else PROJECT_ROOT / args.out
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    try:
        report = recompile_nro(args.exefs, args.nro, output, args.jobs)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"NRO recompile failed: {exc}\n")
    print(f"NRO {report['nro_sha256']}: {report['emitted_functions']}/{report['function_count']} functions, "
          f"{report['unsupported']} unsupported")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
