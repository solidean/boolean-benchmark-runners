#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = [
#   "pyyaml>=6.0",
# ]
# ///
"""
build.py — Runner build script for blender/5.1/boolean

Responsibilities:
  1. Detect the current platform (Linux x64, Windows x64, Windows arm64).
  2. Download the platform-specific Blender 5.1.0 archive from
       https://download.blender.org/release/Blender5.1/
  3. Validate the archive against the official blender-5.1.0.sha256 checksum file.
  4. Extract the archive into download/blender/ (top-level directory stripped).
  5. Write bin/build-info.json.

No CMake, no C++, no vcpkg — Blender ships its own Python runtime.

Platforms:
  - Linux x64   → blender-5.1.0-linux-x64.tar.xz
  - Windows x64 → blender-5.1.0-windows-x64.zip
  - Windows arm64 → blender-5.1.0-windows-arm64.zip
  - macOS       → not supported (DMG extraction requires hdiutil; add later)

Usage:
  ./build.py                     # default: release (no-op for Blender)
  ./build.py --mode debug        # accepted but identical to release
  ./build.py --clean             # remove bin/ (not download/)
  ./build.py --force-download    # re-download even if download/blender/ exists
"""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import shutil
import sys
import tarfile
import urllib.request
import zipfile
from datetime import datetime, timezone
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
# Constants
# ---------------------------------------------------------------------------

BLENDER_VERSION = "5.1.0"
BASE_URL        = "https://download.blender.org/release/Blender5.1"
SHA256_FILENAME = f"blender-{BLENDER_VERSION}.sha256"

DOWNLOAD_DIR = RUNNER_DIR / "download"
BIN_DIR      = RUNNER_DIR / "bin"
SRC_DIR      = RUNNER_DIR / "src"
BLENDER_DIR  = DOWNLOAD_DIR / "blender"

# Maps (system, machine) → archive filename.
# machine values from platform.machine(): x86_64, AMD64, arm64, aarch64
_PLATFORM_ARCHIVES: dict[tuple[str, str], str] = {
    ("Linux",   "x86_64"): f"blender-{BLENDER_VERSION}-linux-x64.tar.xz",
    ("Windows", "AMD64"):  f"blender-{BLENDER_VERSION}-windows-x64.zip",
    ("Windows", "x86_64"): f"blender-{BLENDER_VERSION}-windows-x64.zip",
    ("Windows", "ARM64"):  f"blender-{BLENDER_VERSION}-windows-arm64.zip",
    ("Windows", "arm64"):  f"blender-{BLENDER_VERSION}-windows-arm64.zip",
}


def _detect_archive_name() -> str:
    system  = platform.system()
    machine = platform.machine()
    key = (system, machine)
    archive = _PLATFORM_ARCHIVES.get(key)
    if archive is None:
        print(
            f"[error] Unsupported platform: system={system!r} machine={machine!r}.\n"
            "Supported: Linux x86_64, Windows AMD64/x86_64, Windows ARM64.",
            file=sys.stderr,
        )
        sys.exit(1)
    return archive


# ---------------------------------------------------------------------------
# Download and validate
# ---------------------------------------------------------------------------

def _sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def _download(url: str, dest: Path) -> None:
    print(f"Downloading {url} ...", flush=True)
    dest.parent.mkdir(parents=True, exist_ok=True)

    req = urllib.request.Request(
        url,
        headers={
            "User-Agent": (
                "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:148.0) "
                "Gecko/20100101 Firefox/148.0"
            ),
            "Accept": "*/*",
        },
    )

    with urllib.request.urlopen(req) as resp, open(dest, "wb") as f:
        shutil.copyfileobj(resp, f)

    print(f"  → saved to {dest}", flush=True)


def _parse_sha256_file(sha256_file: Path, filename: str) -> str:
    """Parse a multi-entry sha256 file and return the hash for the given filename."""
    for line in sha256_file.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) >= 2:
            digest, name = parts[0], parts[-1].lstrip("*")
            if name == filename:
                return digest.lower()
    raise RuntimeError(
        f"Could not find entry for {filename!r} in {sha256_file}"
    )


def acquire_blender(force: bool = False) -> tuple[str, str]:
    """
    Ensure Blender is downloaded, verified, and extracted.
    Returns (archive_url, sha256_hex).
    """
    archive_name = _detect_archive_name()
    archive_url  = f"{BASE_URL}/{archive_name}"
    sha256_url   = f"{BASE_URL}/{SHA256_FILENAME}"

    DOWNLOAD_DIR.mkdir(parents=True, exist_ok=True)

    sha256_local = DOWNLOAD_DIR / SHA256_FILENAME
    archive_local = DOWNLOAD_DIR / archive_name

    if BLENDER_DIR.exists() and not force:
        print(f"[skip] Blender already extracted at {BLENDER_DIR}", flush=True)
        # Try to recover expected sha256 from the cached checksum file.
        if sha256_local.exists():
            try:
                digest = _parse_sha256_file(sha256_local, archive_name)
                return archive_url, digest
            except Exception:
                pass
        return archive_url, "<unknown — re-run with --force-download to refresh>"

    # Download checksum file
    _download(sha256_url, sha256_local)
    expected_sha256 = _parse_sha256_file(sha256_local, archive_name)
    print(f"  Expected SHA-256: {expected_sha256}", flush=True)

    # Download archive
    _download(archive_url, archive_local)

    # Validate
    actual_sha256 = _sha256_file(archive_local)
    print(f"  Actual   SHA-256: {actual_sha256}", flush=True)
    if actual_sha256 != expected_sha256:
        raise RuntimeError(
            f"SHA-256 mismatch for {archive_local}!\n"
            f"  expected: {expected_sha256}\n"
            f"  actual:   {actual_sha256}"
        )
    print("[ok] Checksum verified.", flush=True)

    # Extract
    if BLENDER_DIR.exists():
        print(f"[replace] Removing existing extraction at {BLENDER_DIR}", flush=True)
        shutil.rmtree(BLENDER_DIR)
    BLENDER_DIR.mkdir(parents=True, exist_ok=True)

    print(f"Extracting {archive_local} → {BLENDER_DIR} ...", flush=True)
    _extract_archive(archive_local, BLENDER_DIR)
    print(f"[ok] Blender extracted to {BLENDER_DIR}", flush=True)

    return archive_url, actual_sha256


def _extract_archive(archive: Path, dest: Path) -> None:
    """Extract archive to dest, stripping the single top-level directory."""
    suffix = archive.suffix.lower()

    if suffix == ".zip" or archive.name.endswith(".zip"):
        with zipfile.ZipFile(archive, "r") as zf:
            _extract_zip_stripped(zf, dest)
    elif suffix in (".xz", ".gz", ".bz2") or archive.name.endswith(".tar.xz"):
        with tarfile.open(archive, "r:*") as tf:
            _extract_tar_stripped(tf, dest)
    else:
        raise RuntimeError(f"Unknown archive format: {archive}")


def _common_prefix(names: list[str]) -> str | None:
    """Return the single top-level directory shared by all entries, or None."""
    tops = {n.split("/")[0] for n in names if n.strip()}
    if len(tops) == 1:
        return tops.pop()
    return None


def _extract_zip_stripped(zf: "zipfile.ZipFile", dest: Path) -> None:
    names  = zf.namelist()
    prefix = _common_prefix(names)
    strip  = (prefix + "/") if prefix else ""
    for member in zf.infolist():
        rel = member.filename
        if strip and rel.startswith(strip):
            rel = rel[len(strip):]
        if not rel:
            continue
        target = dest / rel
        if member.is_dir():
            target.mkdir(parents=True, exist_ok=True)
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            with zf.open(member) as src, open(target, "wb") as dst:
                shutil.copyfileobj(src, dst)


def _extract_tar_stripped(tf: "tarfile.TarFile", dest: Path) -> None:
    names  = [m.name for m in tf.getmembers()]
    prefix = _common_prefix(names)
    strip  = (prefix + "/") if prefix else ""
    for member in tf.getmembers():
        rel = member.name
        if strip and rel.startswith(strip):
            rel = rel[len(strip):]
        if not rel:
            continue
        target = dest / rel
        if member.isdir():
            target.mkdir(parents=True, exist_ok=True)
        elif member.isfile():
            target.parent.mkdir(parents=True, exist_ok=True)
            with tf.extractfile(member) as src, open(target, "wb") as dst:  # type: ignore[union-attr]
                shutil.copyfileobj(src, dst)
            # Preserve executable bit on Unix
            if platform.system() != "Windows":
                target.chmod(member.mode)
        elif member.issym():
            target.parent.mkdir(parents=True, exist_ok=True)
            if target.exists() or target.is_symlink():
                target.unlink()
            target.symlink_to(member.linkname)


# ---------------------------------------------------------------------------
# build-info.json
# ---------------------------------------------------------------------------

def write_build_info(archive_url: str, sha256: str, mode: str) -> None:
    import yaml  # declared in script dependencies

    manifest = yaml.safe_load((RUNNER_DIR / "runner.yaml").read_text(encoding="utf-8"))
    runner_info = manifest["runner"]

    adapter_commit = bh.git_commit(RUNNER_DIR)
    adapter_dirty  = bh.working_tree_dirty(RUNNER_DIR)

    info = {
        "runner_id":     runner_info["runner_id"],
        "build_mode":    mode,
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "upstream": {
            "source":           "archive",
            "url":              archive_url,
            "sha256":           sha256,
            "resolved_version": BLENDER_VERSION,
            "platform":         platform.system(),
            "machine":          platform.machine(),
        },
        "patches": [],
        "adapter": {
            "path":       "src",
            "vcs_commit": adapter_commit,
            "dirty":      adapter_dirty,
        },
        "toolchain": {
            "python_version": platform.python_version(),
        },
        "artifacts": [],
    }

    out = BIN_DIR / "build-info.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(info, indent=2), encoding="utf-8")
    print(f"Wrote {out}", flush=True)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(
        description=f"Build blender-{BLENDER_VERSION} boolean runner."
    )
    parser.add_argument(
        "--mode", default="release", choices=["release", "debug"],
        help="Build mode (no-op for Blender; kept for convention). Default: release."
    )
    parser.add_argument(
        "--clean", action="store_true",
        help="Remove bin/ and exit. download/ is NOT removed."
    )
    parser.add_argument(
        "--force-download", action="store_true",
        help="Re-download and re-extract Blender even if download/blender/ exists."
    )
    args = parser.parse_args()

    if args.clean:
        print("Cleaning bin/ ...", flush=True)
        if BIN_DIR.exists():
            shutil.rmtree(BIN_DIR)
            print(f"  removed {BIN_DIR}", flush=True)
        else:
            print("  bin/ does not exist; nothing to clean.", flush=True)
        print("Done. Run without --clean to rebuild.")
        print("Delete download/ manually to re-download Blender.")
        return 0

    # Step 1: acquire Blender
    archive_url, sha256 = acquire_blender(force=args.force_download)

    # Step 2: build-info.json
    write_build_info(archive_url, sha256, args.mode)

    # Quick sanity check
    p = RUNNER_DIR / "bin/build-info.json"
    if not p.exists():
        raise RuntimeError(f"Expected artifact missing after build: {p}")

    blender_exe = (
        BLENDER_DIR / ("blender.exe" if platform.system() == "Windows" else "blender")
    )
    print(f"\nBuild complete.")
    print(f"  Blender    : {blender_exe}")
    print(f"  Build info : {BIN_DIR / 'build-info.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
