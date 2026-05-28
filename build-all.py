#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = [
#   "pyyaml>=6.0",
# ]
# ///
#
# Invoke with:   uv run build-all.py
# On Windows:    python -m uv run build-all.py   (or: uv run build-all.py if uv is on PATH)
from __future__ import annotations

import argparse
import re
import subprocess
from pathlib import Path

import yaml


def run(cmd: list[str], cwd: Path) -> None:
    pretty = " ".join(f'"{x}"' if " " in x else x for x in cmd)
    print(f"> {pretty}  (in {cwd})")
    subprocess.run(cmd, cwd=cwd, check=True)


def find_repo_root() -> Path:
    return Path(__file__).resolve().parent


def find_runner_yamls(search_root: Path) -> list[Path]:
    return sorted(p for p in search_root.rglob("runner.yaml") if "_common" not in p.parts)


def _version_key(version: str) -> list[tuple[int, object]]:
    """Natural-order sort key so e.g. 1.10.0 sorts above 1.9.8."""
    key: list[tuple[int, object]] = []
    for tok in re.split(r"(\d+)", version):
        if tok.isdigit():
            key.append((1, int(tok)))
        elif tok:
            key.append((0, tok))
    return key


def select_latest(runner_yamls: list[Path], search_root: Path) -> tuple[list[Path], list[Path]]:
    """Keep only the highest-version runner per group, returning (selected, skipped).

    Groups follow the on-disk layout <family>/<version>/[<algo>/]runner.yaml: the
    version is the path component right after the family, so two versions of the
    same family+algorithm share a group. Runners without a version component are
    always selected.
    """
    groups: dict[tuple, list[tuple[list, Path]]] = {}
    selected: list[Path] = []
    for runner_yaml in runner_yamls:
        parts = runner_yaml.parent.relative_to(search_root).parts
        if len(parts) < 2:
            selected.append(runner_yaml)  # no version component — can't group
            continue
        family, version, algo = parts[0], parts[1], parts[2:]
        groups.setdefault((family, algo), []).append((_version_key(version), runner_yaml))

    skipped: list[Path] = []
    for members in groups.values():
        members.sort(key=lambda m: m[0])
        selected.append(members[-1][1])
        skipped.extend(m[1] for m in members[:-1])

    return sorted(selected), sorted(skipped)


def build_runner(runner_yaml: Path, accept_licenses: bool = False) -> None:
    runner_dir = runner_yaml.parent

    with runner_yaml.open() as f:
        config = yaml.safe_load(f)

    script = config.get("build", {}).get("script")
    if not script:
        print(f"  [skip] no build.script defined in {runner_yaml}")
        return

    # script is relative to the runner directory (e.g. "./build.py")
    script_path = (runner_dir / script).resolve()
    if not script_path.exists():
        print(f"  [skip] build script not found: {script_path}")
        return

    cmd = ["uv", "run", "--script", str(script_path)]
    if accept_licenses:
        cmd.append("-y")
    run(cmd, cwd=runner_dir)


def main() -> int:
    parser = argparse.ArgumentParser(description="Build boolean-benchmark runners (latest version of each by default).")
    parser.add_argument(
        "-y", "--accept-licenses", action="store_true",
        help="Accept all licenses non-interactively, including free non-commercial licenses",
    )
    parser.add_argument(
        "--all", action="store_true",
        help="Build every version of every runner (default: only the latest version of each)",
    )
    args = parser.parse_args()

    repo_root = find_repo_root()

    runner_yamls = find_runner_yamls(repo_root)
    if not runner_yamls:
        print("No runner.yaml files found. Nothing to do.")
        return 0

    if args.all:
        to_build, skipped = runner_yamls, []
    else:
        to_build, skipped = select_latest(runner_yamls, repo_root)

    print(f"Found {len(runner_yamls)} runner(s); building {len(to_build)}.")
    for path in skipped:
        print(f"  [skip older] {path.parent.relative_to(repo_root)}")
    print()

    failed: list[tuple[Path, Exception]] = []
    for runner_yaml in to_build:
        runner_id = runner_yaml.parent.relative_to(repo_root)
        print(f"=== {runner_id} ===")
        try:
            build_runner(runner_yaml, accept_licenses=args.accept_licenses)
        except subprocess.CalledProcessError as e:
            print(f"  [FAILED] exit code {e.returncode}")
            failed.append((runner_yaml, e))
        print()

    if failed:
        print(f"{len(failed)} runner(s) failed:")
        for path, _ in failed:
            print(f"  {path}")
        return 1

    print("Done.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
