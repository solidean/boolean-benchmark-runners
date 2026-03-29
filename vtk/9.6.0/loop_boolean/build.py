#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = [
#   "pyyaml>=6.0",
# ]
# ///
"""
build.py — Runner build script for vtk/9.6.0/loop_boolean

Responsibilities:
  1. Bootstrap vcpkg from extern/vcpkg/ (skipped if already bootstrapped)
  2. Clone VTK at v9.6.0 into ../download/vtk/ (shared with boolean_polydata variant;
     skipped if already present)
  3. Build and install a minimal VTK into ../vtk-install/ (skipped if already present)
  4. Configure and build the adapter via CMake (vcpkg toolchain installs
     nlohmann-json automatically during configure)
  5. Write bin/build-info.json

Prerequisites:
  - git (for VTK clone and vcpkg submodule)
  - cmake >= 3.20
  - A C++17 compiler (gcc >= 9, clang >= 10, or MSVC 2019+)
  - extern/vcpkg submodule initialised:
      git submodule update --init --recursive

Usage:
  ./build.py --mode release    # default: builds optimised artifacts
  ./build.py --mode debug      # debug build
  ./build.py --clean           # remove bin/ and build/ (not ../download/ or ../vtk-install/)
  ./build.py --force-download  # re-clone VTK even if ../download/vtk/ exists
  ./build.py --force-vtk       # rebuild VTK even if ../vtk-install/ exists
"""

from __future__ import annotations

import argparse
import platform
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

# Shared between boolean_polydata and loop_boolean variants.
VTK_VERSION_DIR = RUNNER_DIR.parent
DOWNLOAD_DIR    = VTK_VERSION_DIR / "download"
VTK_CLONE       = DOWNLOAD_DIR / "vtk"
VTK_BUILD_DIR   = VTK_VERSION_DIR / "vtk-build"
VTK_INSTALL_DIR = VTK_VERSION_DIR / "vtk-install"

# Runner-specific build artifacts.
BUILD_DIR = RUNNER_DIR / "build"
BIN_DIR   = RUNNER_DIR / "bin"

VTK_GIT_URL = "https://github.com/Kitware/VTK.git"
VTK_TAG     = "v9.6.0"



# ---------------------------------------------------------------------------
# Acquisition
# ---------------------------------------------------------------------------

def clone_vtk(force: bool = False) -> None:
    if VTK_CLONE.exists():
        if force:
            print(f"[force] Removing existing VTK clone at {VTK_CLONE}")
            shutil.rmtree(VTK_CLONE)
        else:
            print(f"[skip] VTK already present at {VTK_CLONE}")
            return

    DOWNLOAD_DIR.mkdir(parents=True, exist_ok=True)
    print(f"Cloning VTK {VTK_TAG} from {VTK_GIT_URL} ...")
    bh.run([
        "git", "clone",
        "--depth", "1",
        "--branch", VTK_TAG,
        VTK_GIT_URL,
        str(VTK_CLONE),
    ])


# ---------------------------------------------------------------------------
# Shared VTK build
# ---------------------------------------------------------------------------

def build_vtk(mode: str, force: bool = False) -> None:
    """Build and install a minimal VTK into vtk-install/.
    Skipped if vtk-install/ already exists (unless force=True).
    """
    if VTK_INSTALL_DIR.exists() and not force:
        print(f"[skip] VTK install already present at {VTK_INSTALL_DIR}")
        return

    cmake_build_type = "Release" if mode == "release" else "Debug"
    VTK_BUILD_DIR.mkdir(parents=True, exist_ok=True)

    print("Configuring VTK (minimal modules only) ...")
    bh.run([
        "cmake",
        f"-DCMAKE_BUILD_TYPE={cmake_build_type}",
        f"-DCMAKE_INSTALL_PREFIX={VTK_INSTALL_DIR}",
        "-DVTK_BUILD_TESTING=OFF",
        "-DVTK_BUILD_EXAMPLES=OFF",
        "-DVTK_GROUP_ENABLE_Qt=NO",
        "-DVTK_GROUP_ENABLE_Rendering=NO",
        "-DVTK_GROUP_ENABLE_MPI=NO",
        "-DVTK_GROUP_ENABLE_Imaging=NO",
        "-DVTK_GROUP_ENABLE_Views=NO",
        "-DVTK_GROUP_ENABLE_Web=NO",
        "-DVTK_MODULE_ENABLE_VTK_FiltersGeneral=YES",
        "-DVTK_MODULE_ENABLE_VTK_FiltersCore=YES",
        "-DVTK_MODULE_ENABLE_VTK_CommonCore=YES",
        "-DVTK_MODULE_ENABLE_VTK_CommonDataModel=YES",
        str(VTK_CLONE),
    ], cwd=VTK_BUILD_DIR)

    print("Building VTK ...")
    bh.run([
        "cmake", "--build", str(VTK_BUILD_DIR),
        "--config", cmake_build_type,
        "--parallel",
    ], cwd=VTK_BUILD_DIR)

    print("Installing VTK ...")
    bh.run([
        "cmake", "--install", str(VTK_BUILD_DIR),
        "--config", cmake_build_type,
    ], cwd=VTK_BUILD_DIR)


# ---------------------------------------------------------------------------
# Runner build
# ---------------------------------------------------------------------------

def cmake_build(mode: str) -> None:
    cmake_build_type = "Release" if mode == "release" else "Debug"
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    BIN_DIR.mkdir(exist_ok=True)

    vtk_cmake_dir = VTK_INSTALL_DIR / "lib" / "cmake" / f"vtk-{VTK_TAG.lstrip('v').rsplit('.', 1)[0]}"
    if not vtk_cmake_dir.exists():
        vtk_cmake_dir = VTK_INSTALL_DIR / "lib" / "cmake" / "vtk"

    bh.run([
        "cmake",
        f"-DCMAKE_TOOLCHAIN_FILE={bh.vcpkg_toolchain(PROJECT_ROOT)}",
        f"-DCMAKE_BUILD_TYPE={cmake_build_type}",
        f"-DVTK_DIR={vtk_cmake_dir}",
        f"-DPROJECT_ROOT={PROJECT_ROOT}",
        f"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY={BIN_DIR}",
        f"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE={BIN_DIR}",
        f"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG={BIN_DIR}",
        str(RUNNER_DIR),
    ], cwd=BUILD_DIR)

    bh.run([
        "cmake", "--build", str(BUILD_DIR),
        "--config", cmake_build_type,
        "--parallel",
    ], cwd=BUILD_DIR)

    exe = BIN_DIR / bh.executable_name("vtk_loop_runner")
    if not exe.exists():
        raise RuntimeError(f"Build finished but expected binary not found: {exe}")

    sys_name = platform.system()
    if sys_name == "Windows":
        libs = list((VTK_INSTALL_DIR / "bin").glob("*.dll"))
    elif sys_name == "Darwin":
        libs = list((VTK_INSTALL_DIR / "lib").glob("*.dylib"))
    else:
        libs = list((VTK_INSTALL_DIR / "lib").glob("*.so*"))
    if libs:
        print(f"Copying {len(libs)} VTK shared lib(s) to {BIN_DIR} ...")
        for lib in libs:
            shutil.copy2(lib, BIN_DIR / lib.name)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build vtk_loop_runner for VTK 9.6.0 loop_boolean benchmark."
    )
    parser.add_argument("--mode", default="release", choices=["release", "debug"],
                        help="Build mode (default: release)")
    parser.add_argument("--clean", action="store_true",
                        help="Remove bin/ and build/ directories and exit (not shared VTK)")
    parser.add_argument("--force-download", action="store_true",
                        help="Re-clone VTK even if ../download/vtk/ already exists")
    parser.add_argument("--force-vtk", action="store_true",
                        help="Rebuild VTK even if ../vtk-install/ already exists")
    args = parser.parse_args()

    if args.clean:
        print("Cleaning bin/ and build/ ...")
        if BIN_DIR.exists():
            shutil.rmtree(BIN_DIR)
        if BUILD_DIR.exists():
            shutil.rmtree(BUILD_DIR)
        print("Done. Run without --clean to rebuild.")
        print("To also remove the shared VTK build, delete ../download/ and ../vtk-install/ manually.")
        return 0

    # Step 1: ensure vcpkg is bootstrapped
    bh.ensure_vcpkg_bootstrapped(project_root=PROJECT_ROOT)

    # Step 2: acquire VTK source
    clone_vtk(force=args.force_download)
    vtk_commit = bh.git_commit(VTK_CLONE)
    print(f"VTK commit: {vtk_commit}")

    # Step 3: build shared VTK install (skipped if already present)
    build_vtk(args.mode, force=args.force_vtk)

    # Step 4: build the runner adapter
    cmake_build(args.mode)

    # Step 5: toolchain info (reads CMakeCache written in step 4)
    toolchain = bh.gather_toolchain(BUILD_DIR)

    # Step 6: build-info.json
    bh.write_build_info(
        runner_dir=RUNNER_DIR,
        bin_dir=BIN_DIR,
        mode=args.mode,
        exe_stem="vtk_loop_runner",
        upstream={
            "source":           "git",
            "url":              VTK_GIT_URL,
            "requested_ref":    VTK_TAG,
            "resolved_commit":  vtk_commit,
            "resolved_version": "9.6.0",
        },
        toolchain=toolchain,
    )

    exe = BIN_DIR / bh.executable_name("vtk_loop_runner")
    print(f"\nBuild complete.")
    print(f"  Executable : {exe}")
    print(f"  Build info : {BIN_DIR / 'build-info.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
