#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = [
#   "pyyaml>=6.0",
# ]
# ///
"""
build.py — Runner build script for interactive-and-robust-mesh-booleans/2024-6/boolean

Responsibilities:
  1. Bootstrap vcpkg from extern/vcpkg/ (skipped if already bootstrapped)
  2. Clone the repo at commit 7bd6c26 into download/iarmb/ (skipped if already present)
     All external dependencies (oneTBB, Cinolib, Indirect_Predicates, abseil-cpp) are
     vendored in-tree; no submodule init required.
  3. Configure and build the adapter via CMake (vcpkg toolchain installs nlohmann-json;
     TBB and cinolib are built from the vendored in-tree sources)
  4. Write bin/build-info.json

Prerequisites:
  - git
  - cmake >= 3.21
  - A C++20 compiler (gcc >= 10, clang >= 11, or MSVC 2019+)
  - extern/vcpkg submodule initialised:
      git submodule update --init --recursive

Usage:
  ./build.py --mode release    # default: builds optimised artifacts
  ./build.py --mode debug      # debug build
  ./build.py --clean           # remove bin/ and build/ (not download/)
  ./build.py --force-download  # re-clone repo even if download/iarmb/ exists
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

DOWNLOAD_DIR = RUNNER_DIR / "download"
BUILD_DIR    = RUNNER_DIR / "build"
BIN_DIR      = RUNNER_DIR / "bin"
IARMB_CLONE  = DOWNLOAD_DIR / "iarmb"

IARMB_GIT_URL = "https://github.com/gcherchi/InteractiveAndRobustMeshBooleans.git"
IARMB_COMMIT  = "7bd6c26"

VCPKG_ROOT      = PROJECT_ROOT / "extern" / "vcpkg"
VCPKG_TOOLCHAIN = VCPKG_ROOT / "scripts" / "buildsystems" / "vcpkg.cmake"


# ---------------------------------------------------------------------------
# Acquisition
# ---------------------------------------------------------------------------

def clone_iarmb(force: bool = False) -> None:
    if IARMB_CLONE.exists():
        if force:
            print(f"[force] Removing existing clone at {IARMB_CLONE}")
            shutil.rmtree(IARMB_CLONE)
        else:
            print(f"[skip] repo already present at {IARMB_CLONE}")
            return

    DOWNLOAD_DIR.mkdir(parents=True, exist_ok=True)
    print(f"Cloning {IARMB_GIT_URL} ...")
    bh.run(["git", "clone", IARMB_GIT_URL, str(IARMB_CLONE)])
    print(f"Checking out commit {IARMB_COMMIT} ...")
    bh.run(["git", "checkout", IARMB_COMMIT], cwd=IARMB_CLONE)
    # All external deps (oneTBB, Cinolib, Indirect_Predicates, abseil-cpp)
    # are vendored in-tree — no submodule init required.


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
        "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",  # oneTBB vendored copy uses cmake_minimum_required < 3.5
        f"-DIARMB_SOURCE_DIR={IARMB_CLONE}",
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
    exe = BIN_DIR / bh.executable_name("iarmb_runner")
    if not exe.exists():
        raise RuntimeError(f"Build finished but expected binary not found: {exe}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build iarmb_runner for interactive-and-robust-mesh-booleans 2024-6 benchmark."
    )
    parser.add_argument("--mode", default="release", choices=["release", "debug"],
                        help="Build mode (default: release)")
    parser.add_argument("--clean", action="store_true",
                        help="Remove bin/ and build/ directories and exit")
    parser.add_argument("--force-download", action="store_true",
                        help="Re-clone repo even if download/iarmb/ already exists")
    args = parser.parse_args()

    if args.clean:
        print("Cleaning bin/ and build/ ...")
        shutil.rmtree(BIN_DIR, ignore_errors=True)
        shutil.rmtree(BUILD_DIR, ignore_errors=True)
        print("Done. Run without --clean to rebuild. Delete download/ manually to re-clone.")
        return 0

    # Step 1: ensure vcpkg is bootstrapped
    bh.ensure_vcpkg_bootstrapped(VCPKG_ROOT)

    # Step 2: acquire upstream
    clone_iarmb(force=args.force_download)
    iarmb_commit = bh.git_commit(IARMB_CLONE)
    print(f"iarmb commit: {iarmb_commit}")

    # Step 3: build
    cmake_build(args.mode)

    # Step 4: toolchain info
    toolchain = bh.gather_toolchain(BUILD_DIR)

    # Step 5: build-info.json
    bh.write_build_info(
        runner_dir=RUNNER_DIR,
        bin_dir=BIN_DIR,
        mode=args.mode,
        exe_stem="iarmb_runner",
        upstream={
            "source":           "git",
            "url":              IARMB_GIT_URL,
            "requested_ref":    IARMB_COMMIT,
            "resolved_commit":  iarmb_commit,
            "resolved_version": "2024-6",
        },
        toolchain=toolchain,
    )

    exe = BIN_DIR / bh.executable_name("iarmb_runner")
    print(f"\nBuild complete.")
    print(f"  Executable : {exe}")
    print(f"  Build info : {BIN_DIR / 'build-info.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
