#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = [
#   "pyyaml>=6.0",
# ]
# ///
"""
build.py — Runner build script for trueform/0.7.0/boolean

Responsibilities:
  1. Bootstrap vcpkg from extern/vcpkg/ (skipped if already bootstrapped)
  2. Clone trueform at v0.7.0 into download/trueform/ (skipped if already present)
  3. Configure and build the adapter via CMake (vcpkg toolchain installs
     tbb and nlohmann-json automatically during configure; trueform itself is
     header-only and included directly via TRUEFORM_SOURCE_DIR)
  4. Write bin/build-info.json

Prerequisites:
  - git (for trueform clone and vcpkg submodule)
  - cmake >= 3.20
  - A C++20 compiler (gcc >= 10, clang >= 10, or MSVC 2019+)
  - extern/vcpkg submodule initialised:
      git submodule update --init --recursive

Usage:
  ./build.py --mode release    # default: builds optimised artifacts
  ./build.py --mode debug      # debug build
  ./build.py --clean           # remove bin/ and build/ (not download/)
  ./build.py --force-download  # re-clone trueform even if download/trueform/ exists
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
sys.path.insert(0, str(PROJECT_ROOT / "_common" / "build_helpers" / "python"))
import builder_helpers as bh  # noqa: E402


# ---------------------------------------------------------------------------
# Runner-specific constants
# ---------------------------------------------------------------------------

DOWNLOAD_DIR    = RUNNER_DIR / "download"
BUILD_DIR       = RUNNER_DIR / "build"
BIN_DIR         = RUNNER_DIR / "bin"
TRUEFORM_CLONE  = DOWNLOAD_DIR / "trueform"

TRUEFORM_GIT_URL = "https://github.com/polydera/trueform.git"
TRUEFORM_TAG     = "v0.7.0"



# ---------------------------------------------------------------------------
# Acquisition
# ---------------------------------------------------------------------------

def clone_trueform(force: bool = False) -> None:
    if TRUEFORM_CLONE.exists():
        if force:
            print(f"[force] Removing existing trueform clone at {TRUEFORM_CLONE}")
            shutil.rmtree(TRUEFORM_CLONE)
        else:
            print(f"[skip] trueform already present at {TRUEFORM_CLONE}")
            return

    DOWNLOAD_DIR.mkdir(parents=True, exist_ok=True)
    print(f"Cloning trueform {TRUEFORM_TAG} from {TRUEFORM_GIT_URL} ...")
    bh.run([
        "git", "clone",
        "--depth", "1",
        "--branch", TRUEFORM_TAG,
        TRUEFORM_GIT_URL,
        str(TRUEFORM_CLONE),
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
        f"-DCMAKE_TOOLCHAIN_FILE={bh.vcpkg_toolchain(PROJECT_ROOT)}",
        f"-DCMAKE_BUILD_TYPE={cmake_build_type}",
        f"-DTRUEFORM_SOURCE_DIR={TRUEFORM_CLONE}",
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
    exe = BIN_DIR / bh.executable_name("trueform_runner")
    if not exe.exists():
        raise RuntimeError(f"Build finished but expected binary not found: {exe}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description="Build trueform_runner for trueform 0.7.0 boolean benchmark.")
    parser.add_argument("--mode", default="release", choices=["release", "debug"],
                        help="Build mode (default: release)")
    parser.add_argument("--clean", action="store_true",
                        help="Remove bin/ and build/ directories and exit")
    parser.add_argument("--force-download", action="store_true",
                        help="Re-clone trueform even if download/trueform/ already exists")
    parser.add_argument("-y", "--accept-licenses", action="store_true",
                        help="Accept all licenses non-interactively")
    args = parser.parse_args()

    if args.clean:
        print("Cleaning bin/ and build/ ...")
        shutil.rmtree(BIN_DIR, ignore_errors=True)
        shutil.rmtree(BUILD_DIR, ignore_errors=True)
        print("Done. Run without --clean to rebuild. Delete download/ manually to re-clone trueform.")
        return 0

    # Step 1: ensure vcpkg is bootstrapped
    bh.ensure_vcpkg_bootstrapped(project_root=PROJECT_ROOT, runner_dir=RUNNER_DIR)

    # Step 2: acquire upstream
    clone_trueform(force=args.force_download)
    trueform_commit = bh.git_commit(TRUEFORM_CLONE)
    print(f"trueform commit: {trueform_commit}")

    # Step 3: build (vcpkg installs tbb + nlohmann-json during cmake configure;
    #              trueform itself is header-only, included via TRUEFORM_SOURCE_DIR)
    cmake_build(args.mode)

    # Step 4: toolchain info (reads CMakeCache written in step 3)
    toolchain = bh.gather_toolchain(BUILD_DIR)

    # Step 5: build-info.json
    bh.write_build_info(
        runner_dir=RUNNER_DIR,
        bin_dir=BIN_DIR,
        mode=args.mode,
        exe_stem="trueform_runner",
        upstream={
            "source":           "git",
            "url":              TRUEFORM_GIT_URL,
            "requested_ref":    TRUEFORM_TAG,
            "resolved_commit":  trueform_commit,
            "resolved_version": "0.7.0",
        },
        toolchain=toolchain,
    )

    exe = BIN_DIR / bh.executable_name("trueform_runner")
    print(f"\nBuild complete.")
    print(f"  Executable : {exe}")
    print(f"  Build info : {BIN_DIR / 'build-info.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
