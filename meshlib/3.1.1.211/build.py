#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = [
#   "pyyaml>=6.0",
# ]
# ///
"""
build.py — Runner build script for meshlib/3.1.1.211/boolean

Steps:
  1. Download + extract the prebuilt MeshLib archive from the MeshLib GitHub
     release page into download/meshlib/ (vcpkg-style layout).
  2. Bootstrap vcpkg (near-no-op: no external deps — all are bundled).
  3. Configure + build the runner against the prebuilt layout.
  4. Copy libMRMesh.so* + bundled transitive .so files into bin/ so the runner
     is self-contained when launched by the benchmark orchestrator.
  5. Write bin/build-info.json.

Prerequisites:
  - python 3.11+
  - cmake >= 3.20
  - A C++20 compiler

Usage:
  ./build.py --mode release    # default
  ./build.py --mode debug
  ./build.py --clean
  ./build.py --force-download
"""

from __future__ import annotations

import argparse
import hashlib
import os
import platform
import shutil
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path


# ---------------------------------------------------------------------------
# Repo-root discovery
# ---------------------------------------------------------------------------

def _find_repo_root(start: Path) -> Path:
    for p in [start, *start.parents]:
        if (p / "bootstrap-vcpkg.py").exists():
            return p
    raise RuntimeError(
        f"Cannot find repo root (bootstrap-vcpkg.py not found) starting from {start}"
    )


RUNNER_DIR   = Path(__file__).resolve().parent
PROJECT_ROOT = _find_repo_root(RUNNER_DIR)
sys.path.insert(0, str(PROJECT_ROOT / "_common" / "build_helpers" / "python"))
import builder_helpers as bh  # noqa: E402


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

MESHLIB_VERSION = "3.1.1.211"
RELEASE_BASE    = f"https://github.com/MeshInspector/Meshlib/releases/download/v{MESHLIB_VERSION}"

DOWNLOAD_DIR       = RUNNER_DIR / "download"
BUILD_DIR          = RUNNER_DIR / "build"
BIN_DIR            = RUNNER_DIR / "bin"
MESHLIB_DIR        = DOWNLOAD_DIR / "meshlib"
RUNNER_BUILD_DIR   = BUILD_DIR / "runner"


def _meshlib_prefix() -> Path:
    """Effective prefix passed to CMake as MESHLIB_DIR.

    Linux archive lays out include/, lib/ at the top level of MESHLIB_DIR.
    Windows archive wraps them in an extra install/ subdir and uses a
    different include/ layout (no MeshLib/ intermediate) — see CMakeLists.txt.
    """
    if platform.system() == "Windows":
        return MESHLIB_DIR / "install"
    return MESHLIB_DIR

# Maps (system, machine) → archive metadata.
# machine values from platform.machine(): x86_64, AMD64
_PLATFORM_ARCHIVES: dict[tuple[str, str], dict[str, str]] = {
    ("Linux",   "x86_64"): {
        "archive_name": f"meshlib_v{MESHLIB_VERSION}_linux-vcpkg-x64.tar.xz",
        "url":          f"{RELEASE_BASE}/meshlib_v{MESHLIB_VERSION}_linux-vcpkg-x64.tar.xz",
    },
    ("Windows", "AMD64"): {
        "archive_name": f"MeshLibDist_v{MESHLIB_VERSION}.zip",
        "url":          f"{RELEASE_BASE}/MeshLibDist_v{MESHLIB_VERSION}.zip",
    },
    ("Windows", "x86_64"): {
        "archive_name": f"MeshLibDist_v{MESHLIB_VERSION}.zip",
        "url":          f"{RELEASE_BASE}/MeshLibDist_v{MESHLIB_VERSION}.zip",
    },
}


def _detect_archive() -> dict[str, str]:
    system  = platform.system()
    machine = platform.machine()
    info = _PLATFORM_ARCHIVES.get((system, machine))
    if info is None:
        print(
            f"[error] Unsupported platform: system={system!r} machine={machine!r}.\n"
            "Supported: Linux x86_64, Windows AMD64/x86_64. "
            "Other platforms TODO.",
            file=sys.stderr,
        )
        sys.exit(1)
    return info


# ---------------------------------------------------------------------------
# Download + extract helpers (ported from runners/blender/5.1/build.py)
# ---------------------------------------------------------------------------

def _sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def _download(url: str, dest: Path) -> None:
    print(f"Downloading {url} ...", flush=True)
    dest.parent.mkdir(parents=True, exist_ok=True)

    req = urllib.request.Request(
        url,
        headers={
            "User-Agent": (
                "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:148.0) "
                "Gecko/20100101 Firefox/148.0"
            ),
            "Accept": "*/*",
        },
    )

    with urllib.request.urlopen(req) as resp, open(dest, "wb") as f:
        shutil.copyfileobj(resp, f)

    print(f"  -> saved to {dest}", flush=True)


def _common_prefix(names: list[str]) -> str | None:
    """Return the single top-level directory shared by all entries, or None."""
    tops = {n.split("/")[0] for n in names if n.strip()}
    if len(tops) == 1:
        return tops.pop()
    return None


def _extract_zip_stripped(zf: "zipfile.ZipFile", dest: Path) -> None:
    names  = zf.namelist()
    prefix = _common_prefix(names)
    strip  = (prefix + "/") if prefix else ""
    for member in zf.infolist():
        rel = member.filename
        if strip and rel.startswith(strip):
            rel = rel[len(strip):]
        if not rel:
            continue
        target = dest / rel
        if member.is_dir():
            target.mkdir(parents=True, exist_ok=True)
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            with zf.open(member) as src, open(target, "wb") as dst:
                shutil.copyfileobj(src, dst)


def _extract_tar_stripped(tf: "tarfile.TarFile", dest: Path) -> None:
    names  = [m.name for m in tf.getmembers()]
    prefix = _common_prefix(names)
    strip  = (prefix + "/") if prefix else ""
    for member in tf.getmembers():
        rel = member.name
        if strip and rel.startswith(strip):
            rel = rel[len(strip):]
        if not rel:
            continue
        target = dest / rel
        if member.isdir():
            target.mkdir(parents=True, exist_ok=True)
        elif member.isfile():
            target.parent.mkdir(parents=True, exist_ok=True)
            with tf.extractfile(member) as src, open(target, "wb") as dst:  # type: ignore[union-attr]
                shutil.copyfileobj(src, dst)
            if platform.system() != "Windows":
                target.chmod(member.mode)
        elif member.issym():
            target.parent.mkdir(parents=True, exist_ok=True)
            if target.exists() or target.is_symlink():
                target.unlink()
            target.symlink_to(member.linkname)


def _extract_archive(archive: Path, dest: Path) -> None:
    """Extract archive to dest, stripping the single top-level directory."""
    name = archive.name.lower()
    if name.endswith(".zip"):
        with zipfile.ZipFile(archive, "r") as zf:
            _extract_zip_stripped(zf, dest)
    elif name.endswith(".tar.xz") or name.endswith(".tar.gz") or name.endswith(".tar.bz2"):
        with tarfile.open(archive, "r:*") as tf:
            _extract_tar_stripped(tf, dest)
    else:
        raise RuntimeError(f"Unknown archive format: {archive}")


# ---------------------------------------------------------------------------
# Acquisition
# ---------------------------------------------------------------------------

def acquire_meshlib(force: bool = False) -> tuple[str, str]:
    """
    Ensure the prebuilt MeshLib archive is downloaded and extracted to
    download/meshlib/. Returns (archive_url, sha256_hex).
    """
    info = _detect_archive()
    archive_url  = info["url"]
    archive_name = info["archive_name"]

    DOWNLOAD_DIR.mkdir(parents=True, exist_ok=True)
    archive_local = DOWNLOAD_DIR / archive_name

    if MESHLIB_DIR.exists() and not force:
        print(f"[skip] MeshLib already extracted at {MESHLIB_DIR}", flush=True)
        sha = _sha256_file(archive_local) if archive_local.exists() else "<unknown>"
        return archive_url, sha

    _download(archive_url, archive_local)

    sha256 = _sha256_file(archive_local)
    print(f"  SHA-256: {sha256}", flush=True)

    if MESHLIB_DIR.exists():
        print(f"[replace] Removing existing extraction at {MESHLIB_DIR}", flush=True)
        shutil.rmtree(MESHLIB_DIR)
    MESHLIB_DIR.mkdir(parents=True, exist_ok=True)

    print(f"Extracting {archive_local} -> {MESHLIB_DIR} ...", flush=True)
    _extract_archive(archive_local, MESHLIB_DIR)
    print(f"[ok] MeshLib extracted to {MESHLIB_DIR}", flush=True)

    return archive_url, sha256


# ---------------------------------------------------------------------------
# Step 2: Build the runner adapter against the prebuilt MeshLib layout
# ---------------------------------------------------------------------------

def build_runner(mode: str) -> None:
    cmake_build_type = "Release" if mode == "release" else "Debug"
    RUNNER_BUILD_DIR.mkdir(parents=True, exist_ok=True)
    BIN_DIR.mkdir(exist_ok=True)

    vcpkg_toolchain = bh.vcpkg_toolchain(PROJECT_ROOT)

    bh.run([
        "cmake",
        "-S", str(RUNNER_DIR),
        "-B", str(RUNNER_BUILD_DIR),
        f"-DCMAKE_TOOLCHAIN_FILE={vcpkg_toolchain}",
        f"-DCMAKE_BUILD_TYPE={cmake_build_type}",
        f"-DMESHLIB_DIR={_meshlib_prefix()}",
        f"-DPROJECT_ROOT={PROJECT_ROOT}",
        f"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY={BIN_DIR}",
        f"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE={BIN_DIR}",
        f"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG={BIN_DIR}",
    ], cwd=RUNNER_BUILD_DIR)

    bh.run([
        "cmake", "--build", str(RUNNER_BUILD_DIR),
        "--config", cmake_build_type,
        "--parallel",
    ], cwd=RUNNER_BUILD_DIR)

    exe = BIN_DIR / bh.executable_name("meshlib_runner")
    if not exe.exists():
        raise RuntimeError(f"Build finished but expected binary not found: {exe}")


def _copy_lib(src: Path, dst_dir: Path) -> None:
    """Copy a file into dst_dir, preserving symlinks (so libfoo.so -> libfoo.so.1 stays a link)."""
    dst = dst_dir / src.name
    if dst.exists() or dst.is_symlink():
        dst.unlink()
    if src.is_symlink():
        target = os.readlink(src)
        os.symlink(target, dst)
    else:
        shutil.copy2(src, dst)


def copy_runtime_libs(mode: str) -> None:
    """Copy libMRMesh.so* + bundled transitive .so deps into bin/ so the runner is self-contained."""
    BIN_DIR.mkdir(parents=True, exist_ok=True)
    system = platform.system()

    if system == "Windows":
        config = "Release" if mode == "release" else "Debug"
        dll_src = _meshlib_prefix() / "app" / config
        if not dll_src.is_dir():
            print(f"[warn] No Windows DLL dir at {dll_src}")
            return
        count = 0
        for dll in dll_src.glob("*.dll"):
            _copy_lib(dll, BIN_DIR)
            count += 1
        print(f"Copied {count} DLL(s) from {dll_src} -> {BIN_DIR}")
        return

    # Linux (and macOS, though .dylib support is untested here)
    mrmesh_dir = MESHLIB_DIR / "lib" / "MeshLib"
    transitive_dir = MESHLIB_DIR / "lib"

    mrmesh_libs = sorted(mrmesh_dir.glob("libMRMesh.so*"))
    if not mrmesh_libs:
        print(f"[warn] libMRMesh.so* not found under {mrmesh_dir}")
    for lib in mrmesh_libs:
        _copy_lib(lib, BIN_DIR)

    transitive = sorted(p for p in transitive_dir.glob("lib*.so*") if p.is_file() or p.is_symlink())
    for lib in transitive:
        _copy_lib(lib, BIN_DIR)

    print(f"Copied {len(mrmesh_libs)} MRMesh lib(s) + {len(transitive)} transitive .so file(s) -> {BIN_DIR}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description="Build meshlib_runner for MeshLib 3.1.1.211 boolean benchmark.")
    parser.add_argument("--mode", default="release", choices=["release", "debug"],
                        help="Build mode (default: release)")
    parser.add_argument("--clean", action="store_true",
                        help="Remove bin/ and build/ directories and exit")
    parser.add_argument("--force-download", action="store_true",
                        help="Re-download MeshLib even if download/meshlib/ already exists")
    parser.add_argument("-y", "--accept-licenses", action="store_true",
                        help="Accept all licenses non-interactively")
    args = parser.parse_args()

    if args.clean:
        print("Cleaning bin/ and build/ ...")
        if BIN_DIR.exists():
            shutil.rmtree(BIN_DIR)
        if BUILD_DIR.exists():
            shutil.rmtree(BUILD_DIR)
        print("Done. Run without --clean to rebuild. Delete download/ manually to re-download MeshLib.")
        return 0

    print("=== Step 1: Downloading and extracting prebuilt MeshLib ===")
    archive_url, sha256 = acquire_meshlib(force=args.force_download)

    print()
    print("=== Step 2: Bootstrapping vcpkg ===")
    bh.ensure_vcpkg_bootstrapped(project_root=PROJECT_ROOT, runner_dir=RUNNER_DIR)

    print()
    print(f"=== Step 3: Building meshlib_runner ({args.mode}) ===")
    build_runner(args.mode)

    print()
    print("=== Step 4: Copying runtime libraries into bin/ ===")
    copy_runtime_libs(args.mode)

    print()
    print("=== Step 5: Writing build-info.json ===")
    toolchain = bh.gather_toolchain(RUNNER_BUILD_DIR)
    bh.write_build_info(
        runner_dir=RUNNER_DIR,
        bin_dir=BIN_DIR,
        mode=args.mode,
        exe_stem="meshlib_runner",
        upstream={
            "source":           "url",
            "url":              archive_url,
            "sha256":           sha256,
            "resolved_version": MESHLIB_VERSION,
            "platform":         platform.system(),
        },
        toolchain=toolchain,
    )

    exe = BIN_DIR / bh.executable_name("meshlib_runner")
    print()
    print("Build complete.")
    print(f"  Executable : {exe}")
    print(f"  Build info : {BIN_DIR / 'build-info.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
