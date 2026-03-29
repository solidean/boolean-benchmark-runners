#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = [
#   "pyyaml>=6.0",
# ]
# ///
"""
build.py — Runner build script for carve/2014-9/boolean

Responsibilities:
  1. Bootstrap vcpkg from extern/vcpkg/ (skipped if already bootstrapped)
  2. Clone Carve at commit 78f466b into download/carve/ (skipped if already present)
  3. Configure and build the adapter via CMake (vcpkg toolchain installs
     nlohmann-json automatically during configure; Carve itself is built via
     add_subdirectory inside CMakeLists.txt)
  4. Write bin/build-info.json

Prerequisites:
  - git (for Carve clone and vcpkg submodule)
  - cmake >= 3.20
  - A C++17 compiler (gcc >= 9, clang >= 9, or MSVC 2019+)
  - extern/vcpkg submodule initialised:
      git submodule update --init --recursive

Usage:
  ./build.py --mode release    # default: builds optimised artifacts
  ./build.py --mode debug      # debug build
  ./build.py --clean           # remove bin/ and build/ (not download/)
  ./build.py --force-download  # re-clone Carve even if download/carve/ exists
"""

from __future__ import annotations

import argparse
import os
import shutil
import stat
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
CARVE_CLONE  = DOWNLOAD_DIR / "carve"

CARVE_GIT_URL = "https://github.com/VTREEM/Carve.git"
CARVE_COMMIT  = "78f466b"

VCPKG_ROOT      = PROJECT_ROOT / "extern" / "vcpkg"
VCPKG_TOOLCHAIN = VCPKG_ROOT / "scripts" / "buildsystems" / "vcpkg.cmake"


# ---------------------------------------------------------------------------
# Acquisition
# ---------------------------------------------------------------------------

def _rmtree_force(path: Path) -> None:
    """Remove a directory tree, clearing read-only bits first (needed for .git on Windows)."""
    def _on_error(func, p, exc_info):
        os.chmod(p, stat.S_IWRITE)
        func(p)
    shutil.rmtree(path, onexc=_on_error)


def clone_carve(force: bool = False) -> None:
    if CARVE_CLONE.exists():
        if force:
            print(f"[force] Removing existing Carve clone at {CARVE_CLONE}")
            _rmtree_force(CARVE_CLONE)
        else:
            print(f"[skip] Carve already present at {CARVE_CLONE}")
            return

    DOWNLOAD_DIR.mkdir(parents=True, exist_ok=True)
    print(f"Cloning Carve from {CARVE_GIT_URL} ...")
    # Cannot use --depth 1 --branch with a raw commit hash; use blobless clone
    # then check out the target commit.
    bh.run([
        "git", "clone",
        "--filter=blob:none",
        CARVE_GIT_URL,
        str(CARVE_CLONE),
    ])
    print(f"Checking out commit {CARVE_COMMIT} ...")
    bh.run(["git", "-C", str(CARVE_CLONE), "checkout", CARVE_COMMIT])


# ---------------------------------------------------------------------------
# Patching
# ---------------------------------------------------------------------------

def _patch(path: Path, old: str, new: str) -> None:
    """Apply a text-substitution patch (replaces all occurrences). Idempotent."""
    text = path.read_text(encoding="utf-8")
    # Check for 'already patched' FIRST — old may be a substring of new.
    if new in text:
        return  # already patched — skip
    if old in text:
        path.write_text(text.replace(old, new), encoding="utf-8")
    else:
        raise RuntimeError(
            f"Patch failed — expected text not found in {path.relative_to(RUNNER_DIR)}"
        )


def patch_carve() -> None:
    """Apply all C++17/20 compatibility patches to the Carve source tree.

    These patches fix compilation with MSVC 2019+ / C++20 and are idempotent
    (safe to re-apply on an already-patched tree).  They correspond exactly to
    the git diff in download/carve/ after a fresh clone of commit 78f466b.
    """
    src = CARVE_CLONE
    inc = src / "include" / "carve"
    lib = src / "lib"

    print("Patching Carve source for C++17/20 / MSVC compatibility ...")

    # carve.hpp — inject std::hash<pair<T,U>> specialization (pairs used as
    # unordered_map/set keys throughout; std doesn't provide this by default).
    _patch(inc / "carve.hpp",
        "#include <carve/collection.hpp>\n",
        "#include <carve/collection.hpp>\n"
        "\n"
        "// std::hash<std::pair<T,U>> is not provided by the standard but Carve uses\n"
        "// pairs as unordered_map/set keys throughout.  Inject a general specialization.\n"
        "namespace std {\n"
        "  template<typename T, typename U>\n"
        "  struct hash<pair<T, U>> {\n"
        "    size_t operator()(const pair<T, U> &p) const {\n"
        "      size_t h = hash<T>()(p.first);\n"
        "      h ^= hash<U>()(p.second) + 0x9e3779b9u + (h << 6) + (h >> 2);\n"
        "      return h;\n"
        "    }\n"
        "  };\n"
        "}\n"
        "\n"
    )

    # collection_types.hpp — add V2Hash (std::hash<pair<...>> not specialized);
    # change V2Set to use it.
    _patch(inc / "collection_types.hpp",
        "      return V2(v.second, v.first);\n"
        "    }\n"
        "\n"
        "    // include/carve/csg.hpp",
        "      return V2(v.second, v.first);\n"
        "    }\n"
        "\n"
        "    // Hash for V2 (pair of vertex pointers). std::hash<pair<...>> is not specialized\n"
        "    // in the standard; provide one by combining the two pointer hashes.\n"
        "    struct V2Hash {\n"
        "      std::size_t operator()(const V2 &v) const {\n"
        "        std::size_t h = std::hash<const void *>()(v.first);\n"
        "        h ^= std::hash<const void *>()(v.second) + 0x9e3779b9u + (h << 6) + (h >> 2);\n"
        "        return h;\n"
        "      }\n"
        "    };\n"
        "\n"
        "    // include/carve/csg.hpp"
    )
    _patch(inc / "collection_types.hpp",
        "    typedef std::unordered_set<V2> V2Set;",
        "    typedef std::unordered_set<V2, V2Hash> V2Set;",
    )

    # intersection.hpp — data_type → mapped_type (boost extension not in std::).
    _patch(inc / "intersection.hpp",
        "for (super::data_type::const_iterator",
        "for (super::mapped_type::const_iterator",
    )

    # mesh_simplify.hpp — same data_type → mapped_type fix.
    _patch(inc / "mesh_simplify.hpp",
        "for (interaction_graph_t::data_type::iterator",
        "for (interaction_graph_t::mapped_type::iterator",
    )

    # polyhedron_impl.hpp — missing <algorithm> include.
    _patch(inc / "polyhedron_impl.hpp",
        "#include <assert.h>\n",
        "#include <assert.h>\n"
        "#include <algorithm>\n",
    )

    # polyline_iter.hpp — ssize_t is POSIX; not available on MSVC.
    _patch(inc / "polyline_iter.hpp", "ssize_t", "ptrdiff_t")

    # vector.hpp — missing <algorithm> for std::sort.
    _patch(inc / "vector.hpp",
        "#include <sstream>\n",
        "#include <sstream>\n"
        "#include <algorithm>\n",
    )

    # win32.h — cbrt is available natively from VS2013+; guard the fallback.
    _patch(inc / "win32.h",
        "#if defined(_MSC_VER)\n"
        "#  include <carve/cbrt.h>\n",
        "#if defined(_MSC_VER)\n"
        "// cbrt is natively available since VS2013 (_MSC_VER >= 1800); skip the inline fallback.\n"
        "#  if _MSC_VER < 1800\n"
        "#    include <carve/cbrt.h>\n"
        "#  endif\n",
    )

    # iobj.hpp — intptr_t is POSIX/stdint; not pulled in implicitly on Linux.
    _patch(inc / "iobj.hpp",
        "#include <carve/carve.hpp>\n",
        "#include <carve/carve.hpp>\n"
        "#include <cstdint>  // intptr_t\n",
    )

    # rtree.hpp — uint32_t requires <cstdint> on Linux (not implicitly included).
    _patch(lib.parent / "include" / "carve" / "rtree.hpp",
        "#include <limits>\n",
        "#include <limits>\n"
        "#include <cstdint>  // uint32_t\n",
    )

    # geom2d.hpp — max_element/min_element require <algorithm> on Linux.
    _patch(inc / "geom2d.hpp",
        "#include <math.h>\n",
        "#include <math.h>\n"
        "#include <algorithm>  // max_element, min_element\n",
    )

    # rtree.hpp — ssize_t → ptrdiff_t (two array-index casts).
    _patch(lib.parent / "include" / "carve" / "rtree.hpp",
        "part_num[begin[(ssize_t)j]]",
        "part_num[begin[(ptrdiff_t)j]]",
    )

    # external/boost/random/uniform_on_sphere.hpp — std::bind2nd removed in C++17.
    _patch(
        src / "include" / "carve" / "external" / "boost" / "random" / "uniform_on_sphere.hpp",
        "    std::transform(_container.begin(), _container.end(), _container.begin(),\n"
        "                   std::bind2nd(std::divides<RealType>(), sqrt(sqsum)));",
        "    const RealType norm = sqrt(sqsum);\n"
        "    std::transform(_container.begin(), _container.end(), _container.begin(),\n"
        "                   [norm](RealType v) { return v / norm; });",
    )

    # intersect.cpp — two data_type → mapped_type; std::auto_ptr → std::unique_ptr.
    _patch(lib / "intersect.cpp",
        "for (carve::csg::detail::VEVecMap::data_type::const_iterator",
        "for (carve::csg::detail::VEVecMap::mapped_type::const_iterator",
    )
    _patch(lib / "intersect.cpp",
        "for (VertexIntersections::data_type::const_iterator",
        "for (VertexIntersections::mapped_type::const_iterator",
    )
    _patch(lib / "intersect.cpp", "std::auto_ptr", "std::unique_ptr")

    # mesh.cpp — missing <algorithm>; std::bind2nd removed in C++17.
    _patch(lib / "mesh.cpp",
        "#include <carve/poly.hpp>\n",
        "#include <carve/poly.hpp>\n"
        "\n"
        "#include <algorithm>\n",
    )
    _patch(lib / "mesh.cpp",
        "std::bind2nd(std::not_equal_to<const vertex_t *>(), next)",
        "[next](const vertex_t *v) { return v != next; }",
    )
    _patch(lib / "mesh.cpp",
        "std::bind2nd(std::not_equal_to<const vertex_t *>(), prev)",
        "[prev](const vertex_t *v) { return v != prev; }",
    )

    # pointset.cpp — missing <algorithm> for std::sort.
    _patch(lib / "pointset.cpp",
        "#include <carve/pointset.hpp>\n",
        "#include <carve/pointset.hpp>\n"
        "#include <algorithm>\n",
    )

    # polyline.cpp — missing <algorithm> for std::sort.
    _patch(lib / "polyline.cpp",
        "#include <carve/polyline.hpp>\n",
        "#include <carve/polyline.hpp>\n"
        "#include <algorithm>\n",
    )

    # shewchuk_predicates.cpp — sys/time.h is POSIX; random() is POSIX.
    _patch(lib / "shewchuk_predicates.cpp",
        "#include <math.h>\n"
        "#include <sys/time.h>\n",
        "#include <math.h>\n"
        "#if !defined(WIN32) && !defined(_WIN32)\n"
        "#include <sys/time.h>\n"
        "#else\n"
        "#define random rand\n"
        "#endif\n",
    )

    # timing.cpp — WIN32 not defined by MSVC on x64; use _WIN32 as well.
    _patch(lib / "timing.cpp",
        "#ifdef WIN32\n"
        "#include <windows.h>\n"
        "#else \n",
        "#if defined(WIN32) || defined(_WIN32)\n"
        "#include <windows.h>\n"
        "#else\n",
    )

    print("Patching complete.")


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
        f"-DCARVE_SOURCE_DIR={CARVE_CLONE}",
        f"-DPROJECT_ROOT={PROJECT_ROOT}",
        f"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY={BIN_DIR}",
        # On multi-config generators (MSVC), also set per-config output dir
        f"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE={BIN_DIR}",
        f"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG={BIN_DIR}",
        # Carve's CMakeLists.txt predates CMake 3.5; suppress compatibility error.
        "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
        str(RUNNER_DIR),
    ], cwd=BUILD_DIR)

    # Build
    bh.run([
        "cmake", "--build", str(BUILD_DIR),
        "--config", cmake_build_type,
        "--parallel",
    ], cwd=BUILD_DIR)

    # Verify executable
    exe = BIN_DIR / bh.executable_name("carve_runner")
    if not exe.exists():
        raise RuntimeError(f"Build finished but expected binary not found: {exe}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description="Build carve_runner for Carve 2014-9 boolean benchmark.")
    parser.add_argument("--mode", default="release", choices=["release", "debug"],
                        help="Build mode (default: release)")
    parser.add_argument("--clean", action="store_true",
                        help="Remove bin/ and build/ directories and exit")
    parser.add_argument("--force-download", action="store_true",
                        help="Re-clone Carve even if download/carve/ already exists")
    args = parser.parse_args()

    if args.clean:
        print("Cleaning bin/ and build/ ...")
        shutil.rmtree(BIN_DIR, ignore_errors=True)
        shutil.rmtree(BUILD_DIR, ignore_errors=True)
        print("Done. Run without --clean to rebuild. Delete download/ manually to re-clone Carve.")
        return 0

    # Step 1: ensure vcpkg is bootstrapped
    bh.ensure_vcpkg_bootstrapped(VCPKG_ROOT)

    # Step 2: acquire upstream
    clone_carve(force=args.force_download)
    carve_commit = bh.git_commit(CARVE_CLONE)
    print(f"Carve commit: {carve_commit}")

    # Step 2b: apply source patches for C++17/20 / MSVC compatibility.
    patch_carve()

    # Step 3: build (vcpkg installs nlohmann-json during cmake configure;
    #              Carve itself is built via add_subdirectory in CMakeLists.txt)
    cmake_build(args.mode)

    # Step 4: toolchain info (reads CMakeCache written in step 3)
    toolchain = bh.gather_toolchain(BUILD_DIR)

    # Step 5: build-info.json
    bh.write_build_info(
        runner_dir=RUNNER_DIR,
        bin_dir=BIN_DIR,
        mode=args.mode,
        exe_stem="carve_runner",
        upstream={
            "source":           "git",
            "url":              CARVE_GIT_URL,
            "requested_ref":    CARVE_COMMIT,
            "resolved_commit":  carve_commit,
            "resolved_version": "2014-9",
        },
        toolchain=toolchain,
    )

    exe = BIN_DIR / bh.executable_name("carve_runner")
    print(f"\nBuild complete.")
    print(f"  Executable : {exe}")
    print(f"  Build info : {BIN_DIR / 'build-info.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
