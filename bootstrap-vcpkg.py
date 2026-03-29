#!/usr/bin/env -S uv run python3
from __future__ import annotations

import os
import platform
import subprocess
import sys
from pathlib import Path


def run(cmd: list[str], cwd: Path | None = None) -> None:
    pretty = " ".join(f'"{x}"' if " " in x else x for x in cmd)
    print(f"> {pretty}")
    subprocess.run(cmd, cwd=cwd, check=True)


def find_repo_root() -> Path:
    return Path(__file__).resolve().parent


def get_vcpkg_paths(repo_root: Path) -> tuple[Path, Path]:
    vcpkg_root = repo_root / "extern" / "vcpkg"

    if platform.system() == "Windows":
        vcpkg_exe = vcpkg_root / "vcpkg.exe"
    else:
        vcpkg_exe = vcpkg_root / "vcpkg"

    return vcpkg_root, vcpkg_exe


def bootstrap_vcpkg(vcpkg_root: Path, vcpkg_exe: Path) -> None:
    if vcpkg_exe.exists():
        print(f"vcpkg already bootstrapped: {vcpkg_exe}")
        return

    system = platform.system()
    if system == "Windows":
        bootstrap = vcpkg_root / "bootstrap-vcpkg.bat"
        if not bootstrap.exists():
            raise FileNotFoundError(f"Missing bootstrap script: {bootstrap}")
        run([str(bootstrap)], cwd=vcpkg_root)
    else:
        bootstrap = vcpkg_root / "bootstrap-vcpkg.sh"
        if not bootstrap.exists():
            raise FileNotFoundError(f"Missing bootstrap script: {bootstrap}")
        # Use bash explicitly to avoid executable-bit surprises.
        run(["bash", str(bootstrap)], cwd=vcpkg_root)

    if not vcpkg_exe.exists():
        raise RuntimeError(f"Bootstrap completed but vcpkg executable was not created: {vcpkg_exe}")


def find_manifest_roots(runners_root: Path) -> list[Path]:
    manifests = sorted(runners_root.rglob("vcpkg.json"))
    roots = [manifest.parent for manifest in manifests]
    return roots


def install_manifests(vcpkg_exe: Path, manifest_roots: list[Path]) -> None:
    if not manifest_roots:
        print("No vcpkg.json files found under runners/. Nothing to do.")
        return

    print(f"Found {len(manifest_roots)} manifest(s). Prewarming dependencies...")

    for manifest_root in manifest_roots:
        print(f"\n=== {manifest_root} ===")
        run(
            [
                str(vcpkg_exe),
                "install",
                "--x-manifest-root",
                str(manifest_root),
                "--x-install-root",
                str(manifest_root / "build" / "vcpkg_installed"),
            ]
        )


def main() -> int:
    repo_root = find_repo_root()
    runners_root = repo_root / "runners"
    vcpkg_root, vcpkg_exe = get_vcpkg_paths(repo_root)

    if not vcpkg_root.exists():
        print(f"error: missing vcpkg submodule directory: {vcpkg_root}", file=sys.stderr)
        print("Did you run: git submodule update --init --recursive ?", file=sys.stderr)
        return 1

    if not runners_root.exists():
        print(f"error: missing runners directory: {runners_root}", file=sys.stderr)
        return 1

    bootstrap_vcpkg(vcpkg_root, vcpkg_exe)
    manifest_roots = find_manifest_roots(runners_root)
    install_manifests(vcpkg_exe, manifest_roots)

    print("\nDone.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())