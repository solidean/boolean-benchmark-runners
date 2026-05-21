# Adding a New Runner

Practical guide. For concepts see [Runner Overview](Runner%20Overview.md); for schemas see [Runner Manifest Spec](Runner%20Manifest%20Spec.md), [Build Script Spec](Build%20Script%20Spec.md), and [Runner I/O Contract](Runner%20IO%20Contract.md).

## First: new folder or new variant?

| You need | Do this |
|---|---|
| Different upstream library, version family, algorithm family, or executable | Create a **new folder** |
| Different code path that would require awkward branching inside one runner | Create a **new folder** |
| Same artifact with a runtime flag (threads, repair on/off, precision toggle, …) | Add a **runtime variant** to an existing `runner.yaml` |

Folder explosion makes the tree noisy. Don't create a folder for every flag.

## Procedure

1. **Pick the folder identity.**
   `runners/<family>/<version>/<algorithm>/[material-variant]/`. Omit `<algorithm>` if there's only one. See [Runner Overview](Runner%20Overview.md#folder-hierarchy).

2. **Create `runner.yaml`.**
   See the starter at the bottom of this doc. Declare a stable `runner_id` and at least one public variant with capabilities.

3. **Declare exact upstream provenance.**
   Pinned git commit or versioned archive with checksum. Mutable branches are not enough.

4. **Write `build.py`.**
   Use the `uv` shebang. Find the runners-repo root via the `bootstrap-vcpkg.py` sentinel walk (do not use hardcoded `parents[N]`). Import shared helpers from `_common/build_helpers/python/builder_helpers.py`. For C++ runners, pass `-DPROJECT_ROOT=<runners-repo-root>` to cmake so `CMakeLists.txt` can find `_common/cpp/include` without `../../../` paths.

5. **Implement the I/O contract.**
   The runner reads `--request` and writes `--result`. Handle `load-mesh`, `boolean-union`, `boolean-intersection`, `boolean-difference`. Emit `total_ms` on every op; `operation_ms` on boolean ops. See [Runner I/O Contract](Runner%20IO%20Contract.md).

6. **Declare capabilities honestly.**
   If your variant assumes non-self-intersecting input, set `accepts_self_intersections: false`. The meta-runner uses capabilities to decide applicability.

7. **Verify locally.**
   Run `./build.py` and check every path in `build.outputs` exists. Invoke the runner against a small request/result pair from a recent session and confirm the output JSON parses against [Runner I/O Contract](Runner%20IO%20Contract.md).

## Timing fields

Mandatory:

* `total_ms` on every op.
* `operation_ms` on every boolean op — algorithmic core time on already-native inputs (excludes io / import / export / preprocessing).

Recommended for `load-mesh`:

* `io_ms` — file → declared raw mesh format.
* `import_ms` — raw mesh format → native internal structure.

Recommended for boolean ops:

* `export_ms` — native structure → indexed-tris-f64. Disk write excluded.

`preprocessing_ms` only if the work is reusable across ops on the same handle (BVH, spatial index). Never pair-specific work.

Document your raw mesh format and what each `*_ms` field covers in `runner.yaml` notes. See [solidean/2026.1/runner.yaml](../solidean/2026.1/runner.yaml) for an example.

## Checklist

- [ ] Folder path follows `runners/<family>/<version>/[<algorithm>/][<material-variant>/]`
- [ ] `runner.yaml` has `runner`, `entry`, `variants` (required) and `upstream`, `build`, `notes` (recommended)
- [ ] `runner_id` is unique and stable
- [ ] At least one public runtime variant declares capabilities
- [ ] Capabilities accurately describe what the variant is designed to handle
- [ ] `build.py` uses the `uv` shebang
- [ ] `build.py` discovers the repo root via the `bootstrap-vcpkg.py` sentinel walk (no hardcoded `parents[N]`)
- [ ] `build.py` imports `builder_helpers` from `_common/build_helpers/python/`
- [ ] For C++ runners: `build.py` passes `-DPROJECT_ROOT=<path>` and `CMakeLists.txt` derives include paths from it
- [ ] For C++ runners: `CMAKE_CXX_STANDARD 20` (shared headers use `std::span` etc.)
- [ ] `build.py` produces every path listed in `build.outputs`
- [ ] `bin/build-info.json` is written by the build
- [ ] `entry.command` points at the artifact the build produces
- [ ] Runner accepts `--request <path> --result <path>` and writes a valid result JSON
- [ ] Every per-op result has `total_ms`; every boolean op also has `operation_ms`
- [ ] `runner.yaml` notes document the raw mesh format and what each timing field covers
- [ ] `.gitignore` excludes `bin/` and `download/`

## Common pitfalls

**A folder for every flag.** Thread counts, repair on/off, heuristic toggles are runtime variants. Splitting them across folders obscures what's actually different.

**Collapsing different algorithms into one runner.** CGAL corefinement and CGAL Nef belong in separate folders. Don't paper over a structural difference with opaque flags.

**Vague provenance.** A folder named `5.6/` doesn't tell you which `5.6.x` you built. Declare an exact ref/commit or checksum.

**Treating capabilities as comments.** They are execution-planning data. Lying about them causes incorrect case selection. If a variant narrows preconditions, reflect that.

**Missing `build-info.json`.** The meta-runner cannot reconstruct provenance after the fact. Emit `build-info.json` with resolved facts (commit hash, compiler version, applied patches), not just intentions.

**Hardcoded depth-based path discovery.** Don't use `Path(__file__).parents[N]` — depth changes when a runner is moved. Walk upward from `__file__` until `bootstrap-vcpkg.py` is found.

**Relative paths into shared headers.** Don't write `../../../_common/cpp/include` in `CMakeLists.txt`. Receive `-DPROJECT_ROOT=<path>` from `build.py` and derive paths from it.

## Starter `runner.yaml`

Replace `<...>` placeholders.

```yaml
runner:
  family: <family>
  version: "<version>"
  algorithm: <algorithm>            # optional; omit if the runner has only one
  runner_id: <family>-<version>-<algorithm>
  display_name: "<Human Readable Name>"

upstream:
  source: git
  url: https://github.com/<org>/<repo>.git
  ref: <tag>
  # resolved_commit recorded in bin/build-info.json

build:
  script: ./build.py
  outputs:
    - bin/<runner_executable>
    - bin/build-info.json
  default_mode: release

entry:
  kind: executable
  command: ./bin/<runner_executable>
  args:
    - --request
    - "{request}"
    - --result
    - "{result}"

variants:
  - id: base
    internal: true
    args: []
    capabilities:
      operations: [union, intersection, difference]
      input_formats: [off, obj, stl]
      requires_closed_meshes: false
      accepts_self_intersections: true
      accepts_non_manifold_inputs: false
      supports_components: true
      supports_variadic_booleans: false

  - id: default
    extends: base
    label: "Default"
    args: []

notes:
  - "Raw mesh format: indexed-tris-f64."
  - "<Anything else worth recording — license, native data structure, timing details.>"
```

## Starter `build.py`

`_find_repo_root` must live inline in each `build.py` — it has to run *before* `builder_helpers` is imported.

```python
#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = [
#   "pyyaml>=6.0",
# ]
# ///

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path


# Repo-root discovery (must precede helper import)
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


DOWNLOAD_DIR = RUNNER_DIR / "download"
BUILD_DIR    = RUNNER_DIR / "build"
BIN_DIR      = RUNNER_DIR / "bin"

UPSTREAM_GIT_URL = "https://github.com/<org>/<repo>.git"
UPSTREAM_TAG     = "v<version>"
UPSTREAM_CLONE   = DOWNLOAD_DIR / "<library>"


def clone_upstream(force: bool = False) -> None:
    if UPSTREAM_CLONE.exists():
        if force:
            shutil.rmtree(UPSTREAM_CLONE)
        else:
            print(f"[skip] upstream already present at {UPSTREAM_CLONE}")
            return
    DOWNLOAD_DIR.mkdir(parents=True, exist_ok=True)
    bh.run(["git", "clone", "--depth", "1", "--branch", UPSTREAM_TAG,
            UPSTREAM_GIT_URL, str(UPSTREAM_CLONE)])


def cmake_build(mode: str) -> None:
    cmake_build_type = "Release" if mode == "release" else "Debug"
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    BIN_DIR.mkdir(exist_ok=True)

    bh.run([
        "cmake",
        f"-DCMAKE_TOOLCHAIN_FILE={bh.vcpkg_toolchain(project_root=PROJECT_ROOT)}",
        f"-DCMAKE_BUILD_TYPE={cmake_build_type}",
        f"-DPROJECT_ROOT={PROJECT_ROOT}",
        f"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY={BIN_DIR}",
        f"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE={BIN_DIR}",
        f"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG={BIN_DIR}",
        str(RUNNER_DIR),
    ], cwd=BUILD_DIR)

    bh.run(["cmake", "--build", str(BUILD_DIR),
            "--config", cmake_build_type, "--parallel"], cwd=BUILD_DIR)

    exe = BIN_DIR / bh.executable_name("<runner_executable>")
    if not exe.exists():
        raise RuntimeError(f"Build finished but expected binary not found: {exe}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", default="release", choices=["release", "debug"])
    parser.add_argument("--clean", action="store_true")
    parser.add_argument("--force-download", action="store_true")
    args = parser.parse_args()

    if args.clean:
        shutil.rmtree(BIN_DIR, ignore_errors=True)
        shutil.rmtree(BUILD_DIR, ignore_errors=True)
        return 0

    bh.ensure_vcpkg_bootstrapped(project_root=PROJECT_ROOT)

    clone_upstream(force=args.force_download)
    upstream_commit = bh.git_commit(UPSTREAM_CLONE)

    cmake_build(args.mode)

    toolchain = bh.gather_toolchain(BUILD_DIR)

    bh.write_build_info(
        runner_dir=RUNNER_DIR,
        bin_dir=BIN_DIR,
        mode=args.mode,
        exe_stem="<runner_executable>",
        upstream={
            "source":           "git",
            "url":              UPSTREAM_GIT_URL,
            "requested_ref":    UPSTREAM_TAG,
            "resolved_commit":  upstream_commit,
            "resolved_version": "<version>",
        },
        toolchain=toolchain,
    )

    exe = BIN_DIR / bh.executable_name("<runner_executable>")
    print(f"\nBuild complete.\n  Executable : {exe}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```
