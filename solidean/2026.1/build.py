#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = [
#   "pyyaml>=6.0",
# ]
# ///
"""
build.py — Runner build script for solidean/2026.1

Responsibilities:
  1. On first download: display the Solidean Community License URL and require
     explicit acceptance before downloading.
  2. Download the Solidean Community SDK zip for the current platform and
     validate its SHA-256 checksum.
  3. Extract the SDK into download/solidean/
  4. Bootstrap vcpkg from extern/vcpkg/ (skipped if already bootstrapped)
  5. Configure and build the adapter via CMake (vcpkg toolchain installs
     nlohmann-json automatically during configure)
  6. Write bin/build-info.json

Prerequisites:
  - cmake >= 3.20
  - A C++20 compiler (gcc >= 10, clang >= 10, MSVC 2019+)
  - extern/vcpkg submodule initialised:
      git submodule update --init --recursive

Platforms:
  - Linux  → Solidean-Community-Ubuntu24 zip
  - Windows → Solidean-Community-Win64 zip
  - Other  → error

Usage:
  ./build.py --mode release    # default: builds optimised artifacts
  ./build.py --mode debug      # debug build
  ./build.py --clean           # remove bin/ and build/ (not download/)
  ./build.py --force-download  # re-download even if download/solidean/ exists
"""

from __future__ import annotations

import argparse
import hashlib
import platform
import shutil
import sys
import urllib.request
import zipfile
from pathlib import Path


# ---------------------------------------------------------------------------
# Repo-root discovery (must precede helper import)
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
# Runner-specific constants
# ---------------------------------------------------------------------------

DOWNLOAD_DIR = RUNNER_DIR / "download"
BUILD_DIR    = RUNNER_DIR / "build"
BIN_DIR      = RUNNER_DIR / "bin"
SOLIDEAN_DIR = DOWNLOAD_DIR / "solidean"

LICENSE_URL = "https://solidean.com/legal/solidean-community-license/"

PLATFORM_INFO = {
    "Linux": {
        "url": "https://solidean.com/downloads/solidean-sdk/2026-03-12-1bf7dd-Solidean-Community-Ubuntu24.zip",
        "sha256_url": "https://solidean.com/downloads/solidean-sdk/2026-03-12-1bf7dd-Solidean-Community-Ubuntu24.zip.sha256",
        "zip_name": "solidean-ubuntu24.zip",
    },
    "Windows": {
        "url": "https://solidean.com/downloads/solidean-sdk/2026-02-08-4febd1-Solidean-Community-Win64.zip",
        "sha256_url": "https://solidean.com/downloads/solidean-sdk/2026-02-08-4febd1-Solidean-Community-Win64.zip.sha256",
        "zip_name": "solidean-win64.zip",
    },
}



# ---------------------------------------------------------------------------
# License acceptance
# ---------------------------------------------------------------------------

def prompt_license_acceptance() -> None:
    """Print the license URL and require the user to type 'Yes' to continue."""
    print()
    print("=" * 72)
    print("  Solidean Community License")
    print()
    print("  The Solidean Community SDK is subject to the Solidean Community")
    print("  License.  Benchmarking and evaluation are permitted, but")
    print("  redistribution and commercial use are NOT allowed.")
    print()
    print(f"  Please review the full license before downloading:")
    print(f"    {LICENSE_URL}")
    print()
    print("  Type 'Yes' (exactly) to accept and continue, or anything else to abort.")
    print("=" * 72)
    response = input("Accept license? ").strip().lower()
    if response != "yes":
        print("[abort] License not accepted. Exiting.")
        sys.exit(1)
    print("[ok] License accepted.")
    print()


# ---------------------------------------------------------------------------
# Download and validate
# ---------------------------------------------------------------------------

def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def download_solidean(info: dict, force: bool = False) -> str:
    """Download, validate, and extract the Solidean SDK. Returns the sha256 hex."""
    DOWNLOAD_DIR.mkdir(parents=True, exist_ok=True)
    zip_path = DOWNLOAD_DIR / info["zip_name"]

    # Download zip
    print(f"Downloading Solidean SDK from {info['url']} ...")
    urllib.request.urlretrieve(info["url"], zip_path)
    print(f"  → saved to {zip_path}")

    # Download expected sha256
    sha256_txt_path = DOWNLOAD_DIR / (info["zip_name"] + ".sha256")
    print(f"Downloading checksum from {info['sha256_url']} ...")
    urllib.request.urlretrieve(info["sha256_url"], sha256_txt_path)

    expected_sha256 = sha256_txt_path.read_text(encoding="utf-8").split()[0].lower()
    print(f"  Expected SHA-256: {expected_sha256}")

    # Validate
    actual_sha256 = sha256_file(zip_path)
    print(f"  Actual   SHA-256: {actual_sha256}")
    if actual_sha256 != expected_sha256:
        raise RuntimeError(
            f"SHA-256 mismatch for {zip_path}!\n"
            f"  expected: {expected_sha256}\n"
            f"  actual:   {actual_sha256}"
        )
    print("[ok] Checksum verified.")

    # Extract
    if SOLIDEAN_DIR.exists():
        print(f"[replace] Removing existing extraction at {SOLIDEAN_DIR}")
        shutil.rmtree(SOLIDEAN_DIR)
    SOLIDEAN_DIR.mkdir(parents=True, exist_ok=True)

    print(f"Extracting {zip_path} → {SOLIDEAN_DIR} ...")
    with zipfile.ZipFile(zip_path, "r") as zf:
        # Strip top-level directory if all entries share one common prefix
        names = zf.namelist()
        top_dirs = {n.split("/")[0] for n in names if n.strip()}
        if len(top_dirs) == 1:
            prefix = top_dirs.pop() + "/"
            for member in zf.infolist():
                rel = member.filename
                if rel.startswith(prefix):
                    rel = rel[len(prefix):]
                if not rel:
                    continue
                target = SOLIDEAN_DIR / rel
                if member.is_dir():
                    target.mkdir(parents=True, exist_ok=True)
                else:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with zf.open(member) as src, open(target, "wb") as dst:
                        shutil.copyfileobj(src, dst)
        else:
            zf.extractall(SOLIDEAN_DIR)

    print(f"[ok] Solidean SDK extracted to {SOLIDEAN_DIR}")
    return actual_sha256


def acquire_solidean(force: bool = False) -> str:
    """Ensure the Solidean SDK is present. Returns the sha256 hex."""
    system = platform.system()
    if system not in PLATFORM_INFO:
        print(f"[error] Unsupported platform: {system!r}. "
              "Only Linux and Windows are supported.", file=sys.stderr)
        sys.exit(1)

    info = PLATFORM_INFO[system]

    if SOLIDEAN_DIR.exists() and not force:
        print(f"[skip] Solidean SDK already present at {SOLIDEAN_DIR}")
        # Try to read cached sha256
        sha256_path = DOWNLOAD_DIR / (info["zip_name"] + ".sha256")
        if sha256_path.exists():
            return sha256_path.read_text(encoding="utf-8").split()[0].lower()
        return "<unknown — re-run with --force-download to refresh>"

    # First download: require license acceptance
    if not DOWNLOAD_DIR.exists():
        prompt_license_acceptance()

    return download_solidean(info, force=force)


# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------

def cmake_build(mode: str) -> None:
    cmake_build_type = "Release" if mode == "release" else "Debug"
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    BIN_DIR.mkdir(exist_ok=True)

    # Configure
    bh.run([
        "cmake",
        f"-DCMAKE_TOOLCHAIN_FILE={bh.vcpkg_toolchain(PROJECT_ROOT)}",
        f"-DCMAKE_BUILD_TYPE={cmake_build_type}",
        f"-DSOLIDEAN_DIR={SOLIDEAN_DIR}",
        f"-DPROJECT_ROOT={PROJECT_ROOT}",
        f"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY={BIN_DIR}",
        f"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE={BIN_DIR}",
        f"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG={BIN_DIR}",
        str(RUNNER_DIR),
    ], cwd=BUILD_DIR)

    # Build
    bh.run([
        "cmake", "--build", str(BUILD_DIR),
        "--config", cmake_build_type,
        "--parallel",
    ], cwd=BUILD_DIR)

    # Verify executable
    exe = BIN_DIR / bh.executable_name("solidean_runner")
    if not exe.exists():
        raise RuntimeError(f"Build finished but expected binary not found: {exe}")

    sys_name = platform.system()
    if sys_name == "Windows":
        sdk_lib = SOLIDEAN_DIR / "lib" / "windows" / "solidean.dll"
    elif sys_name == "Darwin":
        sdk_lib = SOLIDEAN_DIR / "lib" / "macos" / "libsolidean.dylib"
    else:
        sdk_lib = SOLIDEAN_DIR / "lib" / "linux" / "libsolidean.so"
    if sdk_lib.exists():
        print(f"Copying {sdk_lib.name} to {BIN_DIR} ...")
        shutil.copy2(sdk_lib, BIN_DIR / sdk_lib.name)
    else:
        print(f"[warn] Solidean shared lib not found at expected path: {sdk_lib}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description="Build solidean_runner for Solidean 2026.1 benchmark.")
    parser.add_argument("--mode", default="release", choices=["release", "debug"],
                        help="Build mode (default: release)")
    parser.add_argument("--clean", action="store_true",
                        help="Remove bin/ and build/ directories and exit")
    parser.add_argument("--force-download", action="store_true",
                        help="Re-download Solidean SDK even if download/solidean/ already exists")
    args = parser.parse_args()

    if args.clean:
        print("Cleaning bin/ and build/ ...")
        shutil.rmtree(BIN_DIR)
        shutil.rmtree(BUILD_DIR)
        print("Done. Run without --clean to rebuild. Delete download/ manually to re-download the SDK.")
        return 0

    # Step 1: acquire Solidean SDK (with license prompt on first download)
    sha256 = acquire_solidean(force=args.force_download)

    # Step 2: ensure vcpkg is bootstrapped
    bh.ensure_vcpkg_bootstrapped(project_root=PROJECT_ROOT)

    # Step 3: build
    cmake_build(args.mode)

    # Step 4: toolchain info
    toolchain = bh.gather_toolchain(BUILD_DIR)

    # Step 5: build-info.json
    system = platform.system()
    info_entry = PLATFORM_INFO.get(system, {})
    bh.write_build_info(
        runner_dir=RUNNER_DIR,
        bin_dir=BIN_DIR,
        mode=args.mode,
        exe_stem="solidean_runner",
        upstream={
            "source":           "url",
            "url":              info_entry.get("url", "<unknown>"),
            "sha256":           sha256,
            "resolved_version": "2026.1",
            "platform":         system,
        },
        toolchain=toolchain,
    )

    exe = BIN_DIR / bh.executable_name("solidean_runner")
    print(f"\nBuild complete.")
    print(f"  Executable : {exe}")
    print(f"  Build info : {BIN_DIR / 'build-info.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
