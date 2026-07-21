#!/usr/bin/env python3
"""Verify CMNY release checksums, archive layout, and binary formats."""

from __future__ import annotations

import hashlib
import platform
import shutil
import struct
import subprocess
import tarfile
import tempfile
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DIST = ROOT / "dist"
VERSION = "0.3.0"
PACKAGES = {
    f"cmny-v{VERSION}-macos-universal.tar.gz": "macos",
    f"cmny-v{VERSION}-linux-x86_64.tar.gz": "linux",
    f"cmny-v{VERSION}-windows-x86_64.zip": "windows",
}


def verify_magic(path: Path, kind: str) -> None:
    data = path.read_bytes()
    if kind == "macos":
        if data[:4] != b"\xca\xfe\xba\xbe":
            raise AssertionError("macOS executable is not a universal Mach-O binary")
    elif kind == "linux":
        if data[:4] != b"\x7fELF" or struct.unpack_from("<H", data, 18)[0] != 62:
            raise AssertionError("Linux executable is not x86-64 ELF")
    else:
        pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
        if data[:2] != b"MZ" or data[pe_offset:pe_offset + 4] != b"PE\0\0":
            raise AssertionError("Windows executable is not PE")
        if struct.unpack_from("<H", data, pe_offset + 4)[0] != 0x8664:
            raise AssertionError("Windows executable is not x86-64")
        if struct.unpack_from("<H", data, pe_offset + 24 + 68)[0] != 3:
            raise AssertionError("Windows executable is not a console application")
    if VERSION.encode("ascii") not in data or b"cmny" not in data.lower():
        raise AssertionError(f"{kind} executable does not contain the expected version")


def extract(archive: Path, destination: Path) -> None:
    if archive.suffix == ".zip":
        with zipfile.ZipFile(archive) as bundle:
            bundle.extractall(destination)
    else:
        with tarfile.open(archive) as bundle:
            try:
                bundle.extractall(destination, filter="data")
            except TypeError:
                bundle.extractall(destination)


def main() -> None:
    expected = {}
    for line in (DIST / "SHA256SUMS").read_text(encoding="ascii").splitlines():
        digest, name = line.split("  ", 1)
        expected[name] = digest
    if set(expected) != set(PACKAGES):
        raise AssertionError("SHA256SUMS does not list exactly the expected packages")

    with tempfile.TemporaryDirectory(prefix="cmny-package-check-") as temporary:
        base = Path(temporary)
        for name, kind in PACKAGES.items():
            archive = DIST / name
            actual = hashlib.sha256(archive.read_bytes()).hexdigest()
            if actual != expected[name]:
                raise AssertionError(f"checksum mismatch for {name}")
            destination = base / kind
            destination.mkdir()
            extract(archive, destination)
            roots = list(destination.iterdir())
            if len(roots) != 1 or not roots[0].is_dir():
                raise AssertionError(f"unexpected archive layout for {name}")
            root = roots[0]
            binary = root / ("cmny.exe" if kind == "windows" else "cmny")
            for required in (binary, root / "README.md", root / "LICENSE",
                             root / "assets" / "screenshots" / "overview.svg",
                             root / "assets" / "screenshots" / "reports.svg",
                             root / "assets" / "screenshots" / "settings.svg",
                             root / "THIRD_PARTY_NOTICES.txt", root / "INSTALL.txt"):
                if not required.is_file():
                    raise AssertionError(f"missing {required.name} in {name}")
            verify_magic(binary, kind)
            if kind == "macos" and platform.system() == "Darwin":
                result = subprocess.run([binary, "--version"], text=True, capture_output=True, check=True)
                if result.stdout != f"cmny {VERSION}\n":
                    raise AssertionError("macOS package version output is wrong")
            print(f"ok - {name}")
    if shutil.which("file"):
        subprocess.run(["file", *(DIST / name for name in PACKAGES)], check=True)


if __name__ == "__main__":
    main()
