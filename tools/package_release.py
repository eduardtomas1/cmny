#!/usr/bin/env python3
"""Build CMNY macOS, Linux, and Windows release archives on macOS."""

from __future__ import annotations

import hashlib
import os
import platform
import shutil
import subprocess
import tarfile
import urllib.request
import zipfile
from pathlib import Path


ZIG_VERSION = "0.15.2"
SQLITE_NUMBER = "3530300"
SQLITE_VERSION = "3.53.3"
PDCURSES_VERSION = "4.5.3"

ROOT = Path(__file__).resolve().parents[1]
VERSION = (ROOT / "VERSION").read_text(encoding="ascii").strip()
BUILD = ROOT / "build" / "release"
CACHE = ROOT / "build" / "release-cache"
STAGE = ROOT / "build" / "release-stage"
DIST = ROOT / "dist"

APP_SOURCES = sorted((ROOT / "src").rglob("*.c"))
PDC_COMMON = """
addch addchstr addstr attr beep bkgd border clear color debug delch deleteln
getch getstr getyx inch inchstr initscr inopts insch insstr instr kernel
keyname mouse move outopts overlay pad panel printw refresh scanw scr_dump
scroll slk termattr terminfo touch util window
""".split()
PDC_PLATFORM = "pdcclip pdcdisp pdcgetsc pdckbd pdcscrn pdcsetsc pdcutil".split()

APP_FLAGS = [
    "-O2", "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra",
    "-Wpedantic", "-Wshadow", "-Wconversion", "-Wstrict-prototypes",
    "-Wmissing-prototypes", "-Wformat=2", f'-DCMNY_VERSION="{VERSION}"',
]
SQLITE_FLAGS = [
    "-O2", "-DSQLITE_THREADSAFE=0", "-DSQLITE_OMIT_LOAD_EXTENSION", "-DSQLITE_DQS=0",
]


def run(*command: str | Path, cwd: Path | None = None) -> None:
    printable = " ".join(map(str, command))
    print(f"+ {printable}")
    subprocess.run([str(part) for part in command], cwd=cwd or ROOT, check=True)


def download(url: str, destination: Path, digest: str, algorithm: str = "sha256") -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.exists():
        print(f"+ download {url}")
        with urllib.request.urlopen(url) as response, destination.open("wb") as output:
            shutil.copyfileobj(response, output)
    actual = hashlib.new(algorithm, destination.read_bytes()).hexdigest()
    if actual != digest:
        raise RuntimeError(f"checksum mismatch for {destination.name}: {actual}")


def safe_target(base: Path, member: str) -> Path:
    target = (base / member).resolve()
    if target != base.resolve() and base.resolve() not in target.parents:
        raise RuntimeError(f"unsafe archive member: {member}")
    return target


def extract_tar(archive: Path, destination: Path) -> None:
    if destination.exists():
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive) as bundle:
        for member in bundle.getmembers():
            safe_target(destination.parent, member.name)
        try:
            bundle.extractall(destination.parent, filter="data")
        except TypeError:
            # Python before 3.12 has no extraction filter; paths were checked above.
            bundle.extractall(destination.parent)


def extract_zip(archive: Path, destination: Path) -> None:
    if destination.exists():
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as bundle:
        for member in bundle.namelist():
            safe_target(destination.parent, member)
        bundle.extractall(destination.parent)


def prepare_sources() -> tuple[Path, Path, Path]:
    machine = platform.machine().lower()
    if platform.system() != "Darwin" or machine not in {"arm64", "x86_64"}:
        raise RuntimeError("the all-platform release builder currently requires macOS")
    zig_arch = "aarch64" if machine == "arm64" else "x86_64"
    zig_hash = {
        "aarch64": "3cc2bab367e185cdfb27501c4b30b1b0653c28d9f73df8dc91488e66ece5fa6b",
        "x86_64": "375b6909fc1495d16fc2c7db9538f707456bfc3373b14ee83fdd3e22b3d43f7f",
    }[zig_arch]
    zig_archive = CACHE / f"zig-{zig_arch}-macos-{ZIG_VERSION}.tar.xz"
    zig_dir = CACHE / f"zig-{zig_arch}-macos-{ZIG_VERSION}"
    download(
        f"https://ziglang.org/download/{ZIG_VERSION}/zig-{zig_arch}-macos-{ZIG_VERSION}.tar.xz",
        zig_archive,
        zig_hash,
    )
    extract_tar(zig_archive, zig_dir)

    sqlite_archive = CACHE / f"sqlite-amalgamation-{SQLITE_NUMBER}.zip"
    sqlite_dir = CACHE / f"sqlite-amalgamation-{SQLITE_NUMBER}"
    download(
        f"https://www.sqlite.org/2026/sqlite-amalgamation-{SQLITE_NUMBER}.zip",
        sqlite_archive,
        "d45c688a8cb23f68611a894a756a12d7eb6ab6e9e2468ca70adbeab3808b5ab9",
        "sha3_256",
    )
    extract_zip(sqlite_archive, sqlite_dir)

    pdc_archive = CACHE / f"pdcursesmod-v{PDCURSES_VERSION}.tar.gz"
    pdc_dir = CACHE / f"PDCursesMod-{PDCURSES_VERSION}"
    download(
        f"https://github.com/Bill-Gray/PDCursesMod/archive/refs/tags/v{PDCURSES_VERSION}.tar.gz",
        pdc_archive,
        "5be1c4a1ba42c958deb219e6fe45fd3315444bc47cfe0c89f5ac0d8c00cc5930",
    )
    extract_tar(pdc_archive, pdc_dir)
    return zig_dir / "zig", sqlite_dir, pdc_dir


def compile_pdc(zig: Path, source: Path, target: str, backend: str, output: Path) -> Path:
    object_dir = output / "pdc-objects"
    object_dir.mkdir(parents=True, exist_ok=True)
    objects: list[Path] = []
    flags = [
        "cc", "-target", target, "-O2", "-DPDC_WIDE", "-DPDC_FORCE_UTF8",
        "-DHAVE_VSNPRINTF", "-DHAVE_VSSCANF", f"-I{source}",
    ]
    for name in PDC_COMMON:
        obj = object_dir / f"{name}.o"
        run(zig, *flags, "-c", source / "pdcurses" / f"{name}.c", "-o", obj)
        objects.append(obj)
    for name in PDC_PLATFORM:
        obj = object_dir / f"{name}.o"
        run(zig, *flags, "-c", source / backend / f"{name}.c", "-o", obj)
        objects.append(obj)
    library = output / "libpdcurses.a"
    run(zig, "ar", "rcs", library, *objects)
    return library


def compile_cross(zig: Path, sqlite: Path, pdc: Path, target: str, backend: str,
                  executable: Path) -> None:
    output = executable.parent
    output.mkdir(parents=True, exist_ok=True)
    library = compile_pdc(zig, pdc, target, backend, output)
    objects: list[Path] = []
    includes = [f"-I{ROOT / 'include'}", f"-I{sqlite}", f"-I{pdc}"]
    pdc_flags = ["-DPDC_WIDE", "-DPDC_FORCE_UTF8", "-DPDC_NCMOUSE"]
    for source in APP_SOURCES:
        relative = source.relative_to(ROOT / "src").with_suffix("")
        obj = output / ("_".join(relative.parts) + ".o")
        run(zig, "cc", "-target", target, *APP_FLAGS, *pdc_flags, *includes,
            "-c", source, "-o", obj)
        objects.append(obj)
    sqlite_object = output / "sqlite3.o"
    run(zig, "cc", "-target", target, *SQLITE_FLAGS, "-c", sqlite / "sqlite3.c",
        "-o", sqlite_object)
    link = [zig, "cc", "-target", target, "-s"]
    if "linux" in target:
        link.append("-static")
    link.extend([*objects, sqlite_object, library])
    if "windows" in target:
        link.append("-lwinmm")
    run(*link, "-o", executable)


def compile_macos(sqlite: Path, executable: Path) -> None:
    architecture_binaries: list[Path] = []
    for arch in ("arm64", "x86_64"):
        output = BUILD / f"macos-{arch}"
        output.mkdir(parents=True, exist_ok=True)
        objects: list[Path] = []
        arch_flags = ["-arch", arch, "-mmacosx-version-min=11.0"]
        includes = [f"-I{ROOT / 'include'}", f"-I{sqlite}"]
        for source in APP_SOURCES:
            relative = source.relative_to(ROOT / "src").with_suffix("")
            obj = output / ("_".join(relative.parts) + ".o")
            run("clang", *arch_flags, *APP_FLAGS, *includes, "-c", source, "-o", obj)
            objects.append(obj)
        sqlite_object = output / "sqlite3.o"
        run("clang", *arch_flags, *SQLITE_FLAGS, "-c", sqlite / "sqlite3.c",
            "-o", sqlite_object)
        binary = output / "cmny"
        run("clang", *arch_flags, *objects, sqlite_object, "-lncurses", "-o", binary)
        architecture_binaries.append(binary)
    executable.parent.mkdir(parents=True, exist_ok=True)
    run("lipo", "-create", *architecture_binaries, "-output", executable)
    run("strip", "-x", executable)
    run("codesign", "--force", "--sign", "-", executable)
    run("codesign", "--verify", "--strict", executable)


def normalized_tar_info(info: tarfile.TarInfo) -> tarfile.TarInfo:
    info.uid = info.gid = 0
    info.uname = info.gname = "root"
    info.mtime = 0
    return info


def stage_package(platform_name: str, binary: Path) -> Path:
    package_name = f"cmny-v{VERSION}-{platform_name}"
    directory = STAGE / package_name
    directory.mkdir(parents=True, exist_ok=True)
    binary_name = "cmny.exe" if platform_name.startswith("windows") else "cmny"
    shutil.copy2(binary, directory / binary_name)
    os.chmod(directory / binary_name, 0o755)
    for name in ("README.md", "LICENSE", "VERSION"):
        shutil.copy2(ROOT / name, directory / name)
    shutil.copytree(ROOT / "assets", directory / "assets", dirs_exist_ok=True)
    (directory / "THIRD_PARTY_NOTICES.txt").write_text(
        "CMNY THIRD-PARTY NOTICES\n\n"
        f"SQLite {SQLITE_VERSION} (https://www.sqlite.org/) - public domain; embedded in all packages.\n"
        f"PDCursesMod {PDCURSES_VERSION} (https://github.com/Bill-Gray/PDCursesMod) - "
        "public domain; used by Linux and Windows builds.\n"
        "Zig is used to cross-compile artifacts but is not included in the packages.\n",
        encoding="utf-8",
    )
    command = ".\\cmny.exe --demo" if platform_name.startswith("windows") else "./cmny --demo"
    (directory / "INSTALL.txt").write_text(
        f"CMNY {VERSION}\n\nExtract this directory, open a terminal here, and run:\n\n  {command}\n",
        encoding="utf-8",
    )
    return directory


def archive_package(directory: Path, windows: bool = False) -> Path:
    DIST.mkdir(parents=True, exist_ok=True)
    if windows:
        archive = DIST / f"{directory.name}.zip"
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as bundle:
            for path in sorted(directory.rglob("*")):
                if path.is_file():
                    bundle.write(path, Path(directory.name) / path.relative_to(directory))
    else:
        archive = DIST / f"{directory.name}.tar.gz"
        with tarfile.open(archive, "w:gz", compresslevel=9) as bundle:
            bundle.add(directory, arcname=directory.name, filter=normalized_tar_info)
    return archive


def main() -> None:
    zig, sqlite, pdc = prepare_sources()
    for directory in (BUILD, STAGE):
        if directory.exists():
            shutil.rmtree(directory)
        directory.mkdir(parents=True)
    DIST.mkdir(parents=True, exist_ok=True)
    for pattern in ("cmny-v*-*.tar.gz", "cmny-v*-*.zip"):
        for archive in DIST.glob(pattern):
            archive.unlink()
    (DIST / "SHA256SUMS").unlink(missing_ok=True)

    macos = BUILD / "macos" / "cmny"
    linux = BUILD / "linux-x86_64" / "cmny"
    windows = BUILD / "windows-x86_64" / "cmny.exe"
    compile_macos(sqlite, macos)
    compile_cross(zig, sqlite, pdc, "x86_64-linux-musl", "vt", linux)
    compile_cross(zig, sqlite, pdc, "x86_64-windows-gnu", "wincon", windows)

    expected = {
        "macos-universal": macos,
        "linux-x86_64": linux,
        "windows-x86_64": windows,
    }
    archives = [
        archive_package(stage_package(name, binary), name.startswith("windows"))
        for name, binary in expected.items()
    ]
    checksums = DIST / "SHA256SUMS"
    checksums.write_text(
        "".join(f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n" for path in archives),
        encoding="ascii",
    )
    run("python3", ROOT / "tools" / "verify_packages.py")
    print(f"CMNY {VERSION} packages are ready in {DIST}")


if __name__ == "__main__":
    main()
