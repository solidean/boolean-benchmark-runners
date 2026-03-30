#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = [
#   "pyyaml>=6.0",
# ]
# ///
"""
build.py — Runner build script for quickcsg/2022-10/boolean

Responsibilities:
  1. Bootstrap vcpkg from extern/vcpkg/ (skipped if already bootstrapped)
  2. Clone QuickCSG and checkout commit caff91954daf864da9d738d1d3b9ff2e8902568e
     into download/quickcsg/ (skipped if already present)
  3. Configure and build the adapter via CMake (vcpkg toolchain installs
     nlohmann-json and tbb automatically during configure; QuickCSG sources
     are compiled directly via CMakeLists.txt — QuickCSG uses cmake but we
     bypass its build system to compile sources as a library)
  4. Write bin/build-info.json

Prerequisites:
  - git (for QuickCSG clone and vcpkg submodule)
  - cmake >= 3.20
  - A C++20 compiler (gcc >= 10, clang >= 11, or MSVC 2022)
  - extern/vcpkg submodule initialised:
      git submodule update --init --recursive

Usage:
  ./build.py --mode release    # default: builds optimised artifacts
  ./build.py --mode debug      # debug build
  ./build.py --clean           # remove bin/ and build/ (not download/)
  ./build.py --force-download  # re-clone QuickCSG even if download/quickcsg/ exists
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
QUICKCSG_CLONE  = DOWNLOAD_DIR / "quickcsg"

QUICKCSG_GIT_URL = "https://gitlab.inria.fr/quickcsg/quickcsg.git"
QUICKCSG_COMMIT  = "caff91954daf864da9d738d1d3b9ff2e8902568e"



# ---------------------------------------------------------------------------
# Acquisition
# ---------------------------------------------------------------------------

def clone_quickcsg(force: bool = False) -> None:
    if QUICKCSG_CLONE.exists():
        if force:
            print(f"[force] Removing existing QuickCSG clone at {QUICKCSG_CLONE}")
            shutil.rmtree(QUICKCSG_CLONE)
        else:
            print(f"[skip] QuickCSG already present at {QUICKCSG_CLONE}")
            return

    DOWNLOAD_DIR.mkdir(parents=True, exist_ok=True)
    print(f"Cloning QuickCSG from {QUICKCSG_GIT_URL} ...")
    # QuickCSG has no release tags — clone default branch, then checkout the commit.
    bh.run([
        "git", "clone",
        QUICKCSG_GIT_URL,
        str(QUICKCSG_CLONE),
    ])
    print(f"Checking out QuickCSG commit {QUICKCSG_COMMIT} ...")
    bh.run(["git", "checkout", QUICKCSG_COMMIT], cwd=QUICKCSG_CLONE)


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
        f"-DQUICKCSG_SOURCE_DIR={QUICKCSG_CLONE}",
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
    exe = BIN_DIR / bh.executable_name("quickcsg_runner")
    if not exe.exists():
        raise RuntimeError(f"Build finished but expected binary not found: {exe}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build quickcsg_runner for QuickCSG 2022-10 boolean benchmark."
    )
    parser.add_argument("--mode", default="release", choices=["release", "debug"],
                        help="Build mode (default: release)")
    parser.add_argument("--clean", action="store_true",
                        help="Remove bin/ and build/ directories and exit")
    parser.add_argument("--force-download", action="store_true",
                        help="Re-clone QuickCSG even if download/quickcsg/ already exists")
    parser.add_argument("-y", "--accept-licenses", action="store_true",
                        help="Accept all licenses non-interactively")
    args = parser.parse_args()

    if args.clean:
        print("Cleaning bin/ and build/ ...")
        shutil.rmtree(BIN_DIR, ignore_errors=True)
        shutil.rmtree(BUILD_DIR, ignore_errors=True)
        print("Done. Run without --clean to rebuild. Delete download/ manually to re-clone QuickCSG.")
        return 0

    # Step 1: ensure vcpkg is bootstrapped
    bh.ensure_vcpkg_bootstrapped(project_root=PROJECT_ROOT, runner_dir=RUNNER_DIR)

    # Step 2: acquire upstream
    clone_quickcsg(force=args.force_download)
    quickcsg_commit = bh.git_commit(QUICKCSG_CLONE)
    print(f"QuickCSG commit: {quickcsg_commit}")

    # Step 3: build (vcpkg installs nlohmann-json and tbb during cmake configure;
    #              QuickCSG sources are compiled directly via CMakeLists.txt)
    cmake_build(args.mode)

    # Step 4: toolchain info (reads CMakeCache written in step 3)
    toolchain = bh.gather_toolchain(BUILD_DIR)

    # Step 5: build-info.json
    bh.write_build_info(
        runner_dir=RUNNER_DIR,
        bin_dir=BIN_DIR,
        mode=args.mode,
        exe_stem="quickcsg_runner",
        upstream={
            "source":           "git",
            "url":              QUICKCSG_GIT_URL,
            "requested_ref":    QUICKCSG_COMMIT,
            "resolved_commit":  quickcsg_commit,
            "resolved_version": "2022-10",
        },
        toolchain=toolchain,
    )

    exe = BIN_DIR / bh.executable_name("quickcsg_runner")
    print(f"\nBuild complete.")
    print(f"  Executable : {exe}")
    print(f"  Build info : {BIN_DIR / 'build-info.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
