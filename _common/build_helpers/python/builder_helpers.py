"""
builder_helpers.py — Shared build helpers for boolean-benchmark runners.

Import this module AFTER locating the repo root (it cannot locate the root
itself, since the root must be known to find this file).

Typical usage in a build.py:

    def _find_repo_root(start: Path) -> Path:
        for p in [start, *start.parents]:
            if (p / "bootstrap-vcpkg.py").exists():
                return p
        raise RuntimeError("Cannot find repo root (bootstrap-vcpkg.py not found)")

    RUNNER_DIR   = Path(__file__).resolve().parent
    PROJECT_ROOT = _find_repo_root(RUNNER_DIR)
    sys.path.insert(0, str(PROJECT_ROOT / "runners" / "_common" / "build_helpers" / "python"))
    import builder_helpers as bh
"""

from __future__ import annotations

import json
import platform
import subprocess
from datetime import datetime, timezone
from pathlib import Path

import yaml


# ---------------------------------------------------------------------------
# Subprocess helper
# ---------------------------------------------------------------------------

def run(cmd: list, cwd: Path | None = None, capture: bool = False) -> subprocess.CompletedProcess:
    printable = " ".join(str(c) for c in cmd)
    print(f"+ {printable}", flush=True)
    return subprocess.run(
        [str(c) for c in cmd],
        cwd=str(cwd) if cwd else None,
        check=True,
        capture_output=capture,
        text=capture,
    )


# ---------------------------------------------------------------------------
# Git helpers
# ---------------------------------------------------------------------------

def git_commit(repo: Path) -> str | None:
    try:
        r = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=str(repo), capture_output=True, text=True, check=True,
        )
        return r.stdout.strip()
    except Exception:
        return None


def working_tree_dirty(repo: Path) -> bool | None:
    try:
        r = subprocess.run(
            ["git", "status", "--porcelain"],
            cwd=str(repo), capture_output=True, text=True, check=True,
        )
        return bool(r.stdout.strip())
    except Exception:
        return None


# ---------------------------------------------------------------------------
# Platform helper
# ---------------------------------------------------------------------------

def executable_name(stem: str) -> str:
    return stem + ".exe" if platform.system() == "Windows" else stem


# ---------------------------------------------------------------------------
# vcpkg bootstrap
# ---------------------------------------------------------------------------

def ensure_vcpkg_bootstrapped(vcpkg_root: Path) -> None:
    if not vcpkg_root.exists():
        raise RuntimeError(
            f"vcpkg submodule not found at {vcpkg_root}.\n"
            "Run: git submodule update --init --recursive"
        )

    vcpkg_exe = vcpkg_root / ("vcpkg.exe" if platform.system() == "Windows" else "vcpkg")
    if vcpkg_exe.exists():
        print(f"[skip] vcpkg already bootstrapped at {vcpkg_exe}")
        return

    print(f"Bootstrapping vcpkg at {vcpkg_root} ...")
    if platform.system() == "Windows":
        bootstrap = vcpkg_root / "bootstrap-vcpkg.bat"
        run([str(bootstrap)], cwd=vcpkg_root)
    else:
        bootstrap = vcpkg_root / "bootstrap-vcpkg.sh"
        run(["bash", str(bootstrap)], cwd=vcpkg_root)

    if not vcpkg_exe.exists():
        raise RuntimeError(f"Bootstrap completed but vcpkg executable not found: {vcpkg_exe}")


# ---------------------------------------------------------------------------
# CMake cache introspection
# ---------------------------------------------------------------------------

def cmake_cache_value(cache_path: Path, key: str) -> str | None:
    if not cache_path.exists():
        return None
    for line in cache_path.read_text(encoding="utf-8").splitlines():
        # Lines look like: KEY:TYPE=VALUE
        if "=" in line and not line.startswith("#"):
            k, _, v = line.partition("=")
            k = k.split(":")[0].strip()
            if k == key:
                return v.strip() or None
    return None


def gather_toolchain(build_dir: Path) -> dict:
    toolchain: dict = {}

    # cmake version
    try:
        r = subprocess.run(["cmake", "--version"], capture_output=True, text=True)
        if r.returncode == 0:
            first_line = r.stdout.splitlines()[0]
            toolchain["cmake_version"] = first_line.replace("cmake version", "").strip()
    except Exception:
        pass

    # compiler from CMakeCache
    cache = build_dir / "CMakeCache.txt"
    compiler_path = cmake_cache_value(cache, "CMAKE_CXX_COMPILER")
    build_type    = cmake_cache_value(cache, "CMAKE_BUILD_TYPE")
    if compiler_path:
        toolchain["compiler"] = compiler_path
        try:
            r = subprocess.run([compiler_path, "--version"], capture_output=True, text=True)
            if r.returncode == 0:
                toolchain["compiler_version"] = r.stdout.splitlines()[0].strip()
        except Exception:
            pass
    if build_type:
        toolchain["build_type"] = build_type

    return toolchain


# ---------------------------------------------------------------------------
# build-info.json
# ---------------------------------------------------------------------------

def write_build_info(
    *,
    runner_dir: Path,
    bin_dir: Path,
    mode: str,
    exe_stem: str,
    upstream: dict,
    toolchain: dict,
) -> None:
    manifest = yaml.safe_load((runner_dir / "runner.yaml").read_text(encoding="utf-8"))
    runner_info = manifest["runner"]

    patches: list[str] = []
    patches_dir = runner_dir / "patches"
    if patches_dir.exists():
        patches = [
            str(p.relative_to(runner_dir))
            for p in sorted(patches_dir.glob("*.patch"))
        ]

    adapter_commit = git_commit(runner_dir)
    adapter_dirty  = working_tree_dirty(runner_dir)

    exe_name = executable_name(exe_stem)

    info = {
        "runner_id":     runner_info["runner_id"],
        "build_mode":    mode,
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "upstream":      upstream,
        "patches":       patches,
        "adapter": {
            "path":       "src",
            "vcs_commit": adapter_commit,
            "dirty":      adapter_dirty,
        },
        "toolchain": toolchain,
        "artifacts": [f"bin/{exe_name}"],
    }

    out = bin_dir / "build-info.json"
    out.write_text(json.dumps(info, indent=2), encoding="utf-8")
    print(f"Wrote {out}")
