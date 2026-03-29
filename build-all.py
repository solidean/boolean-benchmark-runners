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


def build_runner(runner_yaml: Path) -> None:
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

    run(["uv", "run", "--script", str(script_path)], cwd=runner_dir)


def main() -> int:
    repo_root = find_repo_root()

    runner_yamls = find_runner_yamls(repo_root)
    if not runner_yamls:
        print("No runner.yaml files found. Nothing to do.")
        return 0

    print(f"Found {len(runner_yamls)} runner(s).\n")

    failed: list[tuple[Path, Exception]] = []
    for runner_yaml in runner_yamls:
        runner_id = runner_yaml.parent.relative_to(repo_root)
        print(f"=== {runner_id} ===")
        try:
            build_runner(runner_yaml)
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
