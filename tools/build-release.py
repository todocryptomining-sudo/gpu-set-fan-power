#!/usr/bin/env python3
"""Construye el tar.gz de una versión con permisos y metadatos correctos.

Uso:
    python tools/build-release.py v1.5 dist/gpu-fan-curve-v1.5.tar.gz

Los scripts quedan en 0755 y el resto en 0644, independientemente de cómo
los haya guardado Windows. La salida es reproducible si se fija
SOURCE_DATE_EPOCH.
"""
from __future__ import annotations

import gzip
import hashlib
import os
import sys
import tarfile
from pathlib import Path

EXECUTABLES = {
    "install.sh",
    "uninstall.sh",
    "editargpu",
    "restartgpu",
    "gpustatus",
    "gpusave",
    "setfan",
    "gpuhelp",
    "updategpu",
}

DEFAULT_EPOCH = 1793836800  # 2026-11-05 00:00:00 UTC


def add_entry(tar: tarfile.TarFile, path: Path, arcname: str, mode: int, epoch: int) -> None:
    info = tar.gettarinfo(str(path), arcname)
    info.uid = 0
    info.gid = 0
    info.uname = "root"
    info.gname = "root"
    info.mode = mode
    info.mtime = epoch
    if path.is_file():
        with path.open("rb") as handle:
            tar.addfile(info, handle)
    else:
        tar.addfile(info)


def main() -> int:
    if len(sys.argv) != 3:
        print("Uso: build-release.py <version> <salida.tar.gz>", file=sys.stderr)
        return 2

    version, output = sys.argv[1], sys.argv[2]
    repo_root = Path(__file__).resolve().parent.parent
    source = repo_root / "releases" / version
    if not source.is_dir():
        print(f"No existe la carpeta {source}", file=sys.stderr)
        return 1

    epoch = int(os.environ.get("SOURCE_DATE_EPOCH", str(DEFAULT_EPOCH)))
    output_path = Path(output)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    dirs = sorted((p for p in source.rglob("*") if p.is_dir()), key=lambda p: p.as_posix())
    files = sorted((p for p in source.rglob("*") if p.is_file()), key=lambda p: p.as_posix())

    with output_path.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=epoch, compresslevel=9) as gz:
            with tarfile.open(fileobj=gz, mode="w", format=tarfile.USTAR_FORMAT) as tar:
                add_entry(tar, source, version, 0o755, epoch)
                for directory in dirs:
                    arcname = f"{version}/{directory.relative_to(source).as_posix()}"
                    add_entry(tar, directory, arcname, 0o755, epoch)
                for file_path in files:
                    arcname = f"{version}/{file_path.relative_to(source).as_posix()}"
                    mode = 0o755 if (file_path.name in EXECUTABLES or file_path.suffix == ".sh") else 0o644
                    add_entry(tar, file_path, arcname, mode, epoch)

    digest = hashlib.sha256(output_path.read_bytes()).hexdigest()
    print(f"{digest}  {output_path.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
