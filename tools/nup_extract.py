"""Extract matching base/update NUP packages to ignored build content."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from pathlib import Path
from typing import BinaryIO

_VENDORED_NUP = Path(__file__).resolve().parent / "vendor" / "nup"
sys.path.insert(0, str(_VENDORED_NUP))

from nup import format as F  # noqa: E402
from nup.access import iter_children  # noqa: E402
from nup.read import NupReader, PackageInfo  # noqa: E402

from tools.nup_audit import _has_complete_update, audit_nup  # noqa: E402
from tools.nup_types import TARGET_TITLE_ID  # noqa: E402


CHUNK_SIZE = 1024 * 1024
PROGRESS_BYTES = 256 * 1024 * 1024
PROJECT_ROOT = Path(__file__).resolve().parents[1]
_WINDOWS_RESERVED = {
    "CON", "PRN", "AUX", "NUL",
    *(f"COM{i}" for i in range(1, 10)),
    *(f"LPT{i}" for i in range(1, 10)),
}


class ExtentReader:
    """Limit a file-like reader to one NUP blob extent."""

    def __init__(self, source: BinaryIO, length: int) -> None:
        self.source = source
        self.remaining = length

    def read(self, size: int = -1) -> bytes:
        if self.remaining <= 0:
            return b""
        if size < 0 or size > self.remaining:
            size = self.remaining
        data = self.source.read(size)
        self.remaining -= len(data)
        return data

    def readable(self) -> bool:
        return True


def _safe_child(parent: Path, name: str) -> Path:
    if (not name or name in {".", ".."} or "/" in name or "\\" in name
            or any(character in name for character in '<>:"|?*')
            or name.endswith((".", " "))
            or name.split(".", 1)[0].upper() in _WINDOWS_RESERVED):
        raise ValueError(f"unsafe path component in NUP: {name!r}")
    target = parent / name
    if target.resolve(strict=False).parent != parent.resolve(strict=False):
        raise ValueError(f"NUP path escapes extraction directory: {name!r}")
    return target


def _write_chunk(output: BinaryIO, digest: Any, chunk: bytes, count: int) -> int:
    output.write(chunk)
    digest.update(chunk)
    return count + len(chunk)


def _extract_file(
    source_path: Path,
    reader: NupReader,
    file_index: int,
    target: Path,
    relative_path: str,
) -> dict[str, Any]:
    name, expected_size, expected_hash, extent_offset, extent_size, codec = reader.file_info(file_index)
    target.parent.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256()
    written = 0
    next_progress = PROGRESS_BYTES
    print(f"extract: {relative_path} ({expected_size / (1024 ** 3):.2f} GiB)", flush=True)

    try:
        with source_path.open("rb") as source, target.open("wb") as output:
            source.seek(extent_offset)
            if expected_size == 0:
                pass
            elif codec == F.CODEC_RAW:
                bounded = ExtentReader(source, extent_size)
                while chunk := bounded.read(CHUNK_SIZE):
                    written = _write_chunk(output, digest, chunk, written)
                    if written >= next_progress:
                        print(
                            f"  {written / (1024 ** 3):.2f}/{expected_size / (1024 ** 3):.2f} GiB",
                            flush=True,
                        )
                        next_progress = written + PROGRESS_BYTES
            elif codec == F.CODEC_ZSTD:
                import zstandard

                bounded = ExtentReader(source, extent_size)
                decompressor = zstandard.ZstdDecompressor()
                with decompressor.stream_reader(
                    bounded, read_size=CHUNK_SIZE, closefd=False
                ) as decoded:
                    while chunk := decoded.read(CHUNK_SIZE):
                        written = _write_chunk(output, digest, chunk, written)
                        if written >= next_progress:
                            print(
                                f"  {written / (1024 ** 3):.2f}/{expected_size / (1024 ** 3):.2f} GiB",
                                flush=True,
                            )
                            next_progress = written + PROGRESS_BYTES
            else:
                raise ValueError(f"unsupported NUP codec {codec} for {name!r}")
            output.flush()
            os.fsync(output.fileno())
    except BaseException:
        target.unlink(missing_ok=True)
        raise

    actual_hash = digest.digest()
    if written != expected_size or actual_hash != expected_hash:
        target.unlink(missing_ok=True)
        raise ValueError(
            f"integrity failure for {relative_path}: size {written}/{expected_size}, "
            f"sha256 {actual_hash.hex()} (expected {expected_hash.hex()})"
        )
    print(f"  ok sha256={actual_hash.hex()}", flush=True)
    return {"path": relative_path, "size": written, "sha256": actual_hash.hex()}


def _extract_package(
    source_path: Path,
    reader: NupReader,
    package: PackageInfo,
    destination: Path,
) -> list[dict[str, Any]]:
    files: list[dict[str, Any]] = []
    stack: list[tuple[int, Path, str]] = [(package.root_dir, destination, "")]
    while stack:
        directory_index, output_dir, relative_dir = stack.pop()
        output_dir.mkdir(parents=True, exist_ok=True)
        for child in reversed(iter_children(reader, directory_index)):
            name = child.name.decode("utf-8", "surrogateescape")
            target = _safe_child(output_dir, name)
            relative_path = f"{relative_dir}/{name}" if relative_dir else name
            if child.is_dir:
                stack.append((child.index, target, relative_path))
            else:
                files.append(_extract_file(source_path, reader, child.index, target, relative_path))
    return files


def extract_nup(report_path: Path, destination: Path) -> Path:
    report = json.loads(report_path.read_text(encoding="utf-8"))
    source_path = Path(report["input_path"]).expanduser().resolve(strict=True)
    audit = audit_nup(source_path)
    if report.get("title_id") != TARGET_TITLE_ID or audit.title_id != TARGET_TITLE_ID:
        raise ValueError(f"report and NUP must target {TARGET_TITLE_ID}")
    if report.get("input_size") != source_path.stat().st_size or audit.input_size != source_path.stat().st_size:
        raise ValueError("NUP file size differs from the audited report; run nup_audit again")
    if report.get("input_mtime_ns") != source_path.stat().st_mtime_ns:
        raise ValueError("NUP timestamp differs from the audited report; run nup_audit again")
    # MTF_CONTENT_ROOT: the install folder the installer chose
    allowed_root = Path(os.environ.get("MTF_CONTENT_ROOT") or PROJECT_ROOT / "build" / "content").resolve()
    destination = destination.expanduser().resolve()
    if not destination.is_relative_to(allowed_root):
        raise ValueError(f"extraction must stay under {allowed_root}")
    if not audit.base_present and not _has_complete_update(audit):
        raise ValueError(f"matching base package {TARGET_TITLE_ID} is absent")
    if 327680 not in audit.update_versions or audit.profile_version != "1.1.1":
        raise ValueError("matching update v327680 / profile 1.1.1 is absent")

    destination.mkdir(parents=True, exist_ok=True)
    manifest: dict[str, Any] = {
        "title_id": audit.title_id,
        "profile_version": audit.profile_version,
        "source_nup": str(source_path),
        "packages": [],
    }
    with NupReader(str(source_path)) as reader:
        for package in reader.packages:
            if package.title_id != int(TARGET_TITLE_ID, 16):
                continue
            if package.kind == F.KIND_BASE:
                package_dir = destination / "base"
            elif package.kind == F.KIND_UPDATE and package.version == 327680:
                package_dir = destination / "updates" / str(package.version)
            else:
                continue
            package_files = _extract_package(source_path, reader, package, package_dir)
            manifest["packages"].append({
                "kind": package.kind_name,
                "title_id": f"{package.title_id:016X}",
                "version": package.version,
                "path": str(package_dir),
                "files": package_files,
            })

    if audit.base_present and not any(item["kind"] == "base" for item in manifest["packages"]):
        raise ValueError("NUP audit and extraction package inventory disagree: base missing")
    if not any(item["kind"] == "update" and item["version"] == 327680 for item in manifest["packages"]):
        raise ValueError("NUP audit and extraction package inventory disagree: update v327680 missing")

    manifest_path = destination / "extraction-manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    final_stat = source_path.stat()
    if final_stat.st_size != audit.input_size or final_stat.st_mtime_ns != audit.input_mtime_ns:
        raise ValueError("source NUP size or timestamp changed during extraction")
    report["payload_hash_status"] = "verified; all extracted files matched NUP TOC SHA-256"
    report["extraction_manifest"] = str(manifest_path)
    report_path.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return manifest_path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path, help="the report tools.nup_audit wrote")
    parser.add_argument("--out", required=True, type=Path, help="output folder (git ignores build/content/<title-id>)")
    args = parser.parse_args(argv)
    try:
        manifest = extract_nup(args.report, args.out)
        print(f"extracted and verified; manifest: {manifest}")
        return 0
    except (OSError, ValueError, KeyError, F.NupError) as exc:
        print(f"nup-extract: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
