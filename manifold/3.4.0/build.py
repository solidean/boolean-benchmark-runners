#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = [
#   "pyyaml>=6.0",
# ]
# ///
"""
build.py — Runner build script for manifold/3.4.0/boolean

Responsibilities:
  1. Bootstrap vcpkg from extern/vcpkg/ (skipped if already bootstrapped)
  2. Clone manifold at v3.4.0 into download/manifold/ (skipped if already present)
  3. Configure and build the adapter via CMake (vcpkg toolchain installs
     nlohmann-json automatically during configure; manifold itself is built via
     add_subdirectory inside CMakeLists.txt)
  4. Write bin/build-info.json

Prerequisites:
  - git (for manifold clone and vcpkg submodule)
  - cmake >= 3.20
  - A C++17 compiler (gcc >= 9, clang >= 9, or MSVC 2019+)
  - extern/vcpkg submodule initialised:
      git submodule update --init --recursive

Usage:
  ./build.py --mode release    # default: builds optimised artifacts
  ./build.py --mode debug      # debug build
  ./build.py --clean           # remove bin/ and build/ (not download/)
  ./build.py --force-download  # re-clone manifold even if download/manifold/ exists
"""

from __future__ import annotations

import argparse
import shutil
import sys
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
sys.path.insert(0, str(PROJECT_ROOT / "runners" / "_common" / "build_helpers" / "python"))
import builder_helpers as bh  # noqa: E402


# ---------------------------------------------------------------------------
# Runner-specific constants
# ---------------------------------------------------------------------------

DOWNLOAD_DIR   = RUNNER_DIR / "download"
BUILD_DIR      = RUNNER_DIR / "build"
BIN_DIR        = RUNNER_DIR / "bin"
MANIFOLD_CLONE = DOWNLOAD_DIR / "manifold"

MANIFOLD_GIT_URL = "https://github.com/elalish/manifold.git"
MANIFOLD_TAG     = "v3.4.0"

VCPKG_ROOT      = PROJECT_ROOT / "extern" / "vcpkg"
VCPKG_TOOLCHAIN = VCPKG_ROOT / "scripts" / "buildsystems" / "vcpkg.cmake"


# ---------------------------------------------------------------------------
# Acquisition
# ---------------------------------------------------------------------------

def clone_manifold(force: bool = False) -> None:
    if MANIFOLD_CLONE.exists():
        if force:
            print(f"[force] Removing existing manifold clone at {MANIFOLD_CLONE}")
            shutil.rmtree(MANIFOLD_CLONE)
        else:
            print(f"[skip] manifold already present at {MANIFOLD_CLONE}")
            return

    DOWNLOAD_DIR.mkdir(parents=True, exist_ok=True)
    print(f"Cloning manifold {MANIFOLD_TAG} from {MANIFOLD_GIT_URL} ...")
    bh.run([
        "git", "clone",
        "--depth", "1",
        "--branch", MANIFOLD_TAG,
        MANIFOLD_GIT_URL,
        str(MANIFOLD_CLONE),
    ])


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
        f"-DCMAKE_TOOLCHAIN_FILE={VCPKG_TOOLCHAIN}",
        f"-DCMAKE_BUILD_TYPE={cmake_build_type}",
        f"-DMANIFOLD_SOURCE_DIR={MANIFOLD_CLONE}",
        f"-DPROJECT_ROOT={PROJECT_ROOT}",
        f"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY={BIN_DIR}",
        # On multi-config generators (MSVC), also set per-config output dir
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
    exe = BIN_DIR / bh.executable_name("manifold_runner")
    if not exe.exists():
        raise RuntimeError(f"Build finished but expected binary not found: {exe}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description="Build manifold_runner for manifold 3.4.0 boolean benchmark.")
    parser.add_argument("--mode", default="release", choices=["release", "debug"],
                        help="Build mode (default: release)")
    parser.add_argument("--clean", action="store_true",
                        help="Remove bin/ and build/ directories and exit")
    parser.add_argument("--force-download", action="store_true",
                        help="Re-clone manifold even if download/manifold/ already exists")
    args = parser.parse_args()

    if args.clean:
        print("Cleaning bin/ and build/ ...")
        shutil.rmtree(BIN_DIR)
        shutil.rmtree(BUILD_DIR)
        print("Done. Run without --clean to rebuild. Delete download/ manually to re-clone manifold.")
        return 0

    # Step 1: ensure vcpkg is bootstrapped
    bh.ensure_vcpkg_bootstrapped(VCPKG_ROOT)

    # Step 2: acquire upstream
    clone_manifold(force=args.force_download)
    manifold_commit = bh.git_commit(MANIFOLD_CLONE)
    print(f"manifold commit: {manifold_commit}")

    # Step 3: build (vcpkg installs nlohmann-json during cmake configure;
    #              manifold itself is built via add_subdirectory in CMakeLists.txt)
    cmake_build(args.mode)

    # Step 4: toolchain info (reads CMakeCache written in step 3)
    toolchain = bh.gather_toolchain(BUILD_DIR)

    # Step 5: build-info.json
    bh.write_build_info(
        runner_dir=RUNNER_DIR,
        bin_dir=BIN_DIR,
        mode=args.mode,
        exe_stem="manifold_runner",
        upstream={
            "source":           "git",
            "url":              MANIFOLD_GIT_URL,
            "requested_ref":    MANIFOLD_TAG,
            "resolved_commit":  manifold_commit,
            "resolved_version": "3.4.0",
        },
        toolchain=toolchain,
    )

    exe = BIN_DIR / bh.executable_name("manifold_runner")
    print(f"\nBuild complete.")
    print(f"  Executable : {exe}")
    print(f"  Build info : {BIN_DIR / 'build-info.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
