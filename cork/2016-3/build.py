#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = [
#   "pyyaml>=6.0",
# ]
# ///
"""
build.py — Runner build script for cork/2016-3/boolean

Responsibilities:
  1. Bootstrap vcpkg from extern/vcpkg/ (skipped if already bootstrapped)
  2. Clone cork at commit 5987de5 into download/cork/ (skipped if already present)
  3. Configure and build the adapter via CMake (vcpkg toolchain installs
     nlohmann-json and gmp automatically during configure; cork sources are
     compiled directly via CMakeLists.txt — cork uses SCons, not CMake)
  4. Write bin/build-info.json

Prerequisites:
  - git (for cork clone and vcpkg submodule)
  - cmake >= 3.20
  - A C++17 compiler (gcc >= 9, clang >= 9, or MSVC 2019+)
  - extern/vcpkg submodule initialised:
      git submodule update --init --recursive

Usage:
  ./build.py --mode release    # default: builds optimised artifacts
  ./build.py --mode debug      # debug build
  ./build.py --clean           # remove bin/ and build/ (not download/)
  ./build.py --force-download  # re-clone cork even if download/cork/ exists
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

DOWNLOAD_DIR = RUNNER_DIR / "download"
BUILD_DIR    = RUNNER_DIR / "build"
BIN_DIR      = RUNNER_DIR / "bin"
CORK_CLONE   = DOWNLOAD_DIR / "cork"

CORK_GIT_URL = "https://github.com/gilbo/cork.git"
CORK_COMMIT  = "5987de5"



# ---------------------------------------------------------------------------
# Acquisition
# ---------------------------------------------------------------------------

def clone_cork(force: bool = False) -> None:
    if CORK_CLONE.exists():
        if force:
            print(f"[force] Removing existing cork clone at {CORK_CLONE}")
            shutil.rmtree(CORK_CLONE)
        else:
            print(f"[skip] cork already present at {CORK_CLONE}")
            return

    DOWNLOAD_DIR.mkdir(parents=True, exist_ok=True)
    print(f"Cloning cork from {CORK_GIT_URL} ...")
    # Cork has no release tags — clone default branch then checkout the commit.
    bh.run([
        "git", "clone",
        CORK_GIT_URL,
        str(CORK_CLONE),
    ])
    print(f"Checking out cork commit {CORK_COMMIT} ...")
    bh.run(["git", "checkout", CORK_COMMIT], cwd=CORK_CLONE)


# ---------------------------------------------------------------------------
# Patches
# ---------------------------------------------------------------------------

def patch_cork_sources() -> None:
    """
    Fix cork's Windows-specific code that does not compile with modern MSVC.

    mesh.isct.tpp uses bare `mesh` inside a template lambda, which MSVC
    rejects (dependent name lookup requires explicit `TopoCache::mesh`).
    The #ifdef _WIN32 branch was meant to help but introduced this bug.
    Collapse both branches to the correct qualified form.
    """
    target = CORK_CLONE / "src" / "mesh" / "mesh.isct.tpp"
    if not target.exists():
        return

    old = (
        "#ifdef _WIN32\n"
        "            Vec3d raw = mesh->verts[v->ref].pos;\n"
        "#else\n"
        "            Vec3d raw = TopoCache::mesh->verts[v->ref].pos;\n"
        "#endif"
    )
    new = "            Vec3d raw = TopoCache::mesh->verts[v->ref].pos;"

    text = target.read_text(encoding="utf-8")
    if new in text and old not in text:
        print("[skip] cork mesh.isct.tpp already patched")
        return
    if old not in text:
        print("[warn] cork mesh.isct.tpp: expected pattern not found — skipping patch")
        return
    target.write_text(text.replace(old, new), encoding="utf-8")
    print("[patch] Fixed mesh.isct.tpp: TopoCache::mesh qualified name")


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
        f"-DCORK_SOURCE_DIR={CORK_CLONE}",
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
    exe = BIN_DIR / bh.executable_name("cork_runner")
    if not exe.exists():
        raise RuntimeError(f"Build finished but expected binary not found: {exe}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description="Build cork_runner for cork 2016-3 boolean benchmark.")
    parser.add_argument("--mode", default="release", choices=["release", "debug"],
                        help="Build mode (default: release)")
    parser.add_argument("--clean", action="store_true",
                        help="Remove bin/ and build/ directories and exit")
    parser.add_argument("--force-download", action="store_true",
                        help="Re-clone cork even if download/cork/ already exists")
    parser.add_argument("-y", "--accept-licenses", action="store_true",
                        help="Accept all licenses non-interactively")
    args = parser.parse_args()

    if args.clean:
        print("Cleaning bin/ and build/ ...")
        shutil.rmtree(BIN_DIR, ignore_errors=True)
        shutil.rmtree(BUILD_DIR, ignore_errors=True)
        print("Done. Run without --clean to rebuild. Delete download/ manually to re-clone cork.")
        return 0

    # Step 1: ensure vcpkg is bootstrapped
    bh.ensure_vcpkg_bootstrapped(project_root=PROJECT_ROOT, runner_dir=RUNNER_DIR)

    # Step 2: acquire upstream
    clone_cork(force=args.force_download)
    patch_cork_sources()
    cork_commit = bh.git_commit(CORK_CLONE)
    print(f"cork commit: {cork_commit}")

    # Step 3: build (vcpkg installs nlohmann-json and gmp during cmake configure;
    #              cork sources are compiled directly via CMakeLists.txt)
    cmake_build(args.mode)

    # Step 4: toolchain info (reads CMakeCache written in step 3)
    toolchain = bh.gather_toolchain(BUILD_DIR)

    # Step 5: build-info.json
    bh.write_build_info(
        runner_dir=RUNNER_DIR,
        bin_dir=BIN_DIR,
        mode=args.mode,
        exe_stem="cork_runner",
        upstream={
            "source":           "git",
            "url":              CORK_GIT_URL,
            "requested_ref":    CORK_COMMIT,
            "resolved_commit":  cork_commit,
            "resolved_version": "2016-3",
        },
        toolchain=toolchain,
    )

    exe = BIN_DIR / bh.executable_name("cork_runner")
    print(f"\nBuild complete.")
    print(f"  Executable : {exe}")
    print(f"  Build info : {BIN_DIR / 'build-info.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
