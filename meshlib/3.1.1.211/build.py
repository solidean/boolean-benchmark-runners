#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = [
#   "pyyaml>=6.0",
# ]
# ///
"""
build.py — Runner build script for meshlib/3.1.1.211/boolean

Two-step build:
  1. Build MeshLib from source as a standalone cmake project (produces libMRMesh.so).
     All MeshLib dependencies (Boost, Eigen3, fmt, spdlog, TBB, etc.) are provided
     via vcpkg.
  2. Build the runner adapter (meshlib_runner) linking against the MeshLib build.

Prerequisites:
  - git, cmake >= 3.20, C++20 compiler
  - extern/vcpkg submodule initialised

Usage:
  ./build.py --mode release    # default
  ./build.py --mode debug
  ./build.py --clean
  ./build.py --force-download
"""

from __future__ import annotations

import argparse
import shutil
import sys
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

DOWNLOAD_DIR       = RUNNER_DIR / "download"
BUILD_DIR          = RUNNER_DIR / "build"
BIN_DIR            = RUNNER_DIR / "bin"
MESHLIB_CLONE      = DOWNLOAD_DIR / "meshlib"
MESHLIB_BUILD_DIR  = BUILD_DIR / "meshlib"
RUNNER_BUILD_DIR   = BUILD_DIR / "runner"

MESHLIB_GIT_URL = "https://github.com/MeshInspector/MeshLib.git"
MESHLIB_TAG     = "v3.1.1.211"


# ---------------------------------------------------------------------------
# Acquisition
# ---------------------------------------------------------------------------

def clone_meshlib(force: bool = False) -> None:
    if MESHLIB_CLONE.exists():
        if force:
            print(f"[force] Removing existing MeshLib clone at {MESHLIB_CLONE}")
            shutil.rmtree(MESHLIB_CLONE)
        else:
            print(f"[skip] MeshLib already present at {MESHLIB_CLONE}")
            return

    DOWNLOAD_DIR.mkdir(parents=True, exist_ok=True)
    print(f"Cloning MeshLib {MESHLIB_TAG} from {MESHLIB_GIT_URL} ...")
    bh.run([
        "git", "clone",
        "--depth", "1",
        "--branch", MESHLIB_TAG,
        MESHLIB_GIT_URL,
        str(MESHLIB_CLONE),
    ])


# ---------------------------------------------------------------------------
# Step 1: Build MeshLib as a standalone project
# ---------------------------------------------------------------------------

def build_meshlib(mode: str) -> None:
    cmake_build_type = "Release" if mode == "release" else "Debug"
    MESHLIB_BUILD_DIR.mkdir(parents=True, exist_ok=True)

    vcpkg_toolchain = bh.vcpkg_toolchain(PROJECT_ROOT)

    # Configure MeshLib — point cmake at the MeshLib source tree directly.
    # Provide all deps via vcpkg and disable everything except MRMesh.
    bh.run([
        "cmake",
        "-S", str(MESHLIB_CLONE),
        "-B", str(MESHLIB_BUILD_DIR),
        f"-DCMAKE_TOOLCHAIN_FILE={vcpkg_toolchain}",
        f"-DCMAKE_BUILD_TYPE={cmake_build_type}",
        # Use vcpkg for dependencies (affects how libzip is found, etc.)
        "-DMESHLIB_USE_VCPKG=ON",
        # Override MeshLib's default triplet (it defaults to x64-windows-meshlib)
        "-DVCPKG_TARGET_TRIPLET=x64-linux",
        # Tell vcpkg where our manifest is (so it finds our vcpkg.json with all deps)
        f"-DVCPKG_MANIFEST_DIR={RUNNER_DIR}",
        # MeshLib's CMake doesn't include GNUInstallDirs in the vcpkg branch
        "-DCMAKE_INSTALL_INCLUDEDIR=include",
        "-DCMAKE_INSTALL_LIBDIR=lib",
        # Disable precompiled headers
        "-DMR_PCH=OFF",
        # Disable all optional components
        "-DMESHLIB_BUILD_MRVIEWER=OFF",
        "-DMESHLIB_BUILD_MESHVIEWER=OFF",
        "-DMESHLIB_PYTHON_SUPPORT=OFF",
        "-DMESHLIB_BUILD_PYTHON_MODULES=OFF",
        "-DMESHLIB_BUILD_MRCUDA=OFF",
        "-DMESHLIB_BUILD_VOXELS=OFF",
        "-DMESHLIB_BUILD_SYMBOLMESH=OFF",
        "-DMESHLIB_BUILD_MESHCONV=OFF",
        "-DMESHLIB_BUILD_EXTRA_IO_FORMATS=OFF",
        "-DMESHLIB_BUILD_GENERATED_C_BINDINGS=OFF",
        "-DMESHLIB_EXPERIMENTAL_BUILD_C_BINDING=OFF",
        "-DBUILD_TESTING=OFF",
        "-DMRMESH_NO_TIFF=ON",
        "-DMRMESH_NO_GTEST=ON",
    ], cwd=MESHLIB_BUILD_DIR)

    # Build only the MRMesh target (and its dependency MRPch if PCH is on)
    bh.run([
        "cmake", "--build", str(MESHLIB_BUILD_DIR),
        "--target", "MRMesh",
        "--config", cmake_build_type,
        "--parallel",
    ], cwd=MESHLIB_BUILD_DIR)

    # Verify MRMesh library was built
    lib_candidates = list(MESHLIB_BUILD_DIR.rglob("libMRMesh.so*"))
    if not lib_candidates:
        lib_candidates = list(MESHLIB_BUILD_DIR.rglob("MRMesh.dll"))
    if not lib_candidates:
        raise RuntimeError("MeshLib build finished but libMRMesh not found")
    print(f"  MRMesh library: {lib_candidates[0]}")


# ---------------------------------------------------------------------------
# Step 2: Build the runner adapter
# ---------------------------------------------------------------------------

def build_runner(mode: str) -> None:
    cmake_build_type = "Release" if mode == "release" else "Debug"
    RUNNER_BUILD_DIR.mkdir(parents=True, exist_ok=True)
    BIN_DIR.mkdir(exist_ok=True)

    vcpkg_toolchain = bh.vcpkg_toolchain(PROJECT_ROOT)

    # Find the MRMesh library and headers from the MeshLib build
    meshlib_source_dir = str(MESHLIB_CLONE)
    meshlib_build_dir = str(MESHLIB_BUILD_DIR)

    bh.run([
        "cmake",
        "-S", str(RUNNER_DIR),
        "-B", str(RUNNER_BUILD_DIR),
        f"-DCMAKE_TOOLCHAIN_FILE={vcpkg_toolchain}",
        f"-DCMAKE_BUILD_TYPE={cmake_build_type}",
        f"-DMESHLIB_SOURCE_DIR={meshlib_source_dir}",
        f"-DMESHLIB_BUILD_DIR={meshlib_build_dir}",
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
                        help="Re-clone MeshLib even if download/meshlib/ already exists")
    parser.add_argument("-y", "--accept-licenses", action="store_true",
                        help="Accept all licenses non-interactively")
    args = parser.parse_args()

    if args.clean:
        print("Cleaning bin/ and build/ ...")
        if BIN_DIR.exists():
            shutil.rmtree(BIN_DIR)
        if BUILD_DIR.exists():
            shutil.rmtree(BUILD_DIR)
        print("Done. Run without --clean to rebuild. Delete download/ manually to re-clone MeshLib.")
        return 0

    # Step 0: ensure vcpkg is bootstrapped
    bh.ensure_vcpkg_bootstrapped(project_root=PROJECT_ROOT, runner_dir=RUNNER_DIR)

    # Step 1: acquire upstream
    clone_meshlib(force=args.force_download)
    meshlib_commit = bh.git_commit(MESHLIB_CLONE)
    print(f"MeshLib commit: {meshlib_commit}")

    # Step 2: build MeshLib as a standalone project
    print("\n=== Step 1/2: Building MeshLib ===")
    build_meshlib(args.mode)

    # Step 3: build the runner adapter
    print("\n=== Step 2/2: Building runner ===")
    build_runner(args.mode)

    # Step 4: toolchain info
    toolchain = bh.gather_toolchain(RUNNER_BUILD_DIR)

    # Step 5: build-info.json
    bh.write_build_info(
        runner_dir=RUNNER_DIR,
        bin_dir=BIN_DIR,
        mode=args.mode,
        exe_stem="meshlib_runner",
        upstream={
            "source":           "git",
            "url":              MESHLIB_GIT_URL,
            "requested_ref":    MESHLIB_TAG,
            "resolved_commit":  meshlib_commit,
            "resolved_version": "3.1.1.211",
        },
        toolchain=toolchain,
    )

    exe = BIN_DIR / bh.executable_name("meshlib_runner")
    print(f"\nBuild complete.")
    print(f"  Executable : {exe}")
    print(f"  Build info : {BIN_DIR / 'build-info.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
