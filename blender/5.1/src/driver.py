#!/usr/bin/env -S uv run
"""
driver.py — Blender 5.1 Boolean runner entry point.

Locates the downloaded Blender binary in download/blender/ (relative to the
runner root, one level above this script's src/ directory) and invokes it as a subprocess:

    blender --background --factory-startup --python <runner.py> -- <args...>

All arguments passed to this script are forwarded verbatim after the '--'
separator so that runner.py receives them.

Exit code mirrors Blender's exit code.
"""

from __future__ import annotations

import platform
import subprocess
import sys
from pathlib import Path


def find_blender(runner_root: Path) -> Path:
    """Return the path to the Blender executable inside download/blender/."""
    system = platform.system()
    if system == "Windows":
        exe = runner_root / "download" / "blender" / "blender.exe"
    else:
        # Linux (and other Unix-like systems if ever added)
        exe = runner_root / "download" / "blender" / "blender"

    if not exe.exists():
        raise FileNotFoundError(
            f"Blender executable not found at {exe}.\n"
            "Run build.py to download and extract Blender first."
        )
    return exe


def main() -> int:
    # src/ is the parent of this script; runner root is one level up
    src_dir = Path(__file__).resolve().parent
    runner_root = src_dir.parent
    runner_py = src_dir / "runner.py"

    if not runner_py.exists():
        print(f"[error] runner.py not found at {runner_py}", file=sys.stderr)
        return 1

    blender_exe = find_blender(runner_root)

    # Forward all arguments passed to driver.py after the '--' separator.
    # The meta-runner calls: python src/driver.py --request <req> --result <res> [variant args]
    # We pass them through as-is after '--' so runner.py can parse them.
    forwarded_args = sys.argv[1:]

    cmd = [
        str(blender_exe),
        "--background",
        "--factory-startup",
        "--python", str(runner_py),
        "--",
        *forwarded_args,
    ]

    result = subprocess.run(cmd)
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
