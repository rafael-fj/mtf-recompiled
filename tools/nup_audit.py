"""Read-only inventory and identity checks for the supplied MTF NUP."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

_VENDORED_NUP = Path(__file__).resolve().parent / "vendor" / "nup"
sys.path.insert(0, str(_VENDORED_NUP))

from nup import format as F  # noqa: E402
from nup.access import iter_children, meta_records  # noqa: E402
from nup.read import NupReader, PackageInfo  # noqa: E402
from nup.verify import verify  # noqa: E402

from tools.nup_types import (  # noqa: E402
    PROFILE_VERSIONS,
    TARGET_TITLE_ID,
    NupAudit,
    NupFileRecord,
    NupPackageRecord,
)


class NupAuditError(RuntimeError):
    """The NUP is malformed or does not match the pinned game profile."""


def _decode(payload: bytes | None) -> str | None:
    return payload.decode("utf-8", "replace") if payload is not None else None


def _package_files(reader: NupReader, package: PackageInfo) -> tuple[NupFileRecord, ...]:
    found: list[NupFileRecord] = []
    stack: list[tuple[int, str]] = [(package.root_dir, "")]
    while stack:
        directory_index, prefix = stack.pop()
        for child in reversed(iter_children(reader, directory_index)):
            name = child.name.decode("utf-8", "surrogateescape")
            relpath = f"{prefix}/{name}" if prefix else name
            if child.is_dir:
                stack.append((child.index, relpath))
                continue
            file_name, size, digest, _offset, _extent_size, codec = reader.file_info(child.index)
            if file_name != child.name:
                raise NupAuditError(f"inconsistent NUP file node for {relpath!r}")
            found.append(NupFileRecord(
                path=relpath,
                size=size,
                sha256=digest.hex(),
                codec="zstd" if codec == F.CODEC_ZSTD else "raw",
            ))
    return tuple(found)


def _module_records(packages: tuple[NupPackageRecord, ...]) -> tuple[dict[str, Any], ...]:
    records: list[dict[str, Any]] = []
    for package in packages:
        for file in package.files:
            name = Path(file.path).name.lower()
            if file.path.startswith("exefs/") or name in {"42", "nrr.nrr"}:
                records.append({
                    "package_index": package.index,
                    "package_kind": package.kind,
                    "path": file.path,
                    "size": file.size,
                    "sha256": file.sha256,
                    "package_build_id": package.build_id,
                })
    return tuple(records)


def audit_nup(path: Path) -> NupAudit:
    source = path.expanduser().resolve(strict=True)
    source_stat = source.stat()
    source_size = source_stat.st_size
    packages: list[NupPackageRecord] = []
    with NupReader(str(source)) as reader:
        for package in reader.packages:
            meta: dict[int, bytes] = {}
            for tlv_type, payload in meta_records(reader, package):
                meta.setdefault(tlv_type, payload)
            title_id = f"{package.title_id:016X}"
            raw_version = package.version
            packages.append(NupPackageRecord(
                index=package.index,
                kind=package.kind_name,
                title_id=title_id,
                version=raw_version,
                version_hex=f"0x{raw_version:X}",
                flags=package.flags,
                display_version=_decode(meta.get(F.TLV_DISPLAY_VERSION)),
                profile_version=PROFILE_VERSIONS.get((title_id, raw_version)),
                source_dump_label=_decode(meta.get(F.TLV_SOURCE_DUMP)),
                build_id=meta.get(F.TLV_BUILD_ID, b"").rstrip(bytes(1)).hex().upper() or None,
                files=_package_files(reader, package),
            ))

    matching = [p for p in packages if p.title_id == TARGET_TITLE_ID]
    if not matching:
        raise NupAuditError(
            f"the NUP does not hold the expected title {TARGET_TITLE_ID}; found: "
            f"{', '.join(sorted({p.title_id for p in packages})) or 'nenhum'}"
        )

    ok, problems = verify(str(source), deep=False)
    if not ok:
        raise NupAuditError("structural check failed: " + "; ".join(problems[:8]))

    updates = sorted({p.version for p in matching if p.kind == "update"})
    base_present = any(p.kind == "base" for p in matching)
    update_profile_versions = {
        p.profile_version for p in matching if p.kind == "update" and p.profile_version
    }
    display_versions = {p.display_version for p in matching if p.display_version}
    source_labels = {p.source_dump_label for p in matching if p.source_dump_label}
    profile_version = next(iter(update_profile_versions)) if len(update_profile_versions) == 1 else None
    display_version = next(iter(display_versions)) if len(display_versions) == 1 else None
    source_dump_label = next(iter(source_labels)) if len(source_labels) == 1 else None
    package_tuple = tuple(packages)

    return NupAudit(
        input_path=str(source),
        input_size=source_size,
        input_mtime_ns=source_stat.st_mtime_ns,
        title_id=TARGET_TITLE_ID,
        display_version=display_version,
        profile_version=profile_version,
        source_dump_label=source_dump_label,
        packages=package_tuple,
        base_present=base_present,
        update_versions=tuple(updates),
        module_records=_module_records(package_tuple),
        verification_status="fast_ok (header, TOC, tree structure)",
        payload_hash_status="pending extraction; each output will be SHA-256 checked",
    )


def _has_complete_update(audit: NupAudit) -> bool:
    for p in audit.packages:
        if p.title_id == audit.title_id and p.kind == "update" and p.version == 327680:
            has_main = any(f.path == "exefs/main" for f in p.files)
            has_romfs = any(f.path.startswith("romfs/") for f in p.files)
            if has_main and has_romfs:
                return True
    return False


def _summary(audit: NupAudit) -> str:
    complete_upd = _has_complete_update(audit)
    lines = [
        f"structurally valid NUP: {audit.input_path}",
        f"title: {audit.title_id}; base present: {'yes' if audit.base_present else 'no'}",
        f"profile version: {audit.profile_version or 'not recognised'}; "
        f"display-version TLV: {audit.display_version or 'absent'}",
        f"source label: {audit.source_dump_label or 'absent'}",
        "Pacotes: " + ", ".join(
            f"{p.kind} v{p.version} ({len(p.files)} arquivos)"
            for p in audit.packages if p.title_id == audit.title_id
        ),
    ]
    if not audit.base_present:
        if complete_upd:
            lines.append("base package absent, but update v327680 (1.1.1) is complete (standalone).")
        else:
            lines.append(
                f"the base package of title {audit.title_id} is missing; nothing extracted."
            )
    if 327680 not in audit.update_versions:
        lines.append("the expected update v327680 (1.1.1) is not there.")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="your .nup")
    parser.add_argument("--out", required=True, type=Path, help="JSON report (metadata only)")
    args = parser.parse_args(argv)
    try:
        audit = audit_nup(args.input)
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(audit.to_dict(), indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        print(_summary(audit))
        if (not audit.base_present and not _has_complete_update(audit)) or 327680 not in audit.update_versions:
            return 2
        return 0
    except (OSError, F.NupError, NupAuditError, ValueError) as exc:
        print(f"nup-audit: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
