# Build Script Spec

Each runner directory must contain a `build.py` next to its `runner.yaml`. The build script is responsible for producing the runner artifacts from scratch or from its own local caches.

Companion docs: [Runner Overview](Runner%20Overview.md), [Runner Manifest Spec](Runner%20Manifest%20Spec.md).

## What it must do

1. Acquire upstream sources deterministically (clone at a ref, download an archive with checksum, or use a locally-configured manual path).
2. Apply any patches under `patches/` if needed.
3. Build the upstream library and the local adapter.
4. Place the entry binary (declared in `runner.yaml` `entry.command`) and any auxiliary artifacts under `bin/`.
5. Write `bin/build-info.json` describing the resolved build (see below).

Cache layout, build directories, and download locations are runner-local — there is intentionally no shared cache contract. Local duplication is acceptable.

## `uv` shebang pattern

Use the `uv`-based shebang so each runner declares its own Python deps inline:

```python
#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = [
#   "pyyaml>=6.0",
# ]
# ///
```

This avoids a shared Python environment and keeps build deps local to the runner.

## Patches

If a runner needs upstream patches, place them in `patches/`. The build script applies them as part of the build, and records the applied set in `build-info.json`.

```text
runners/cgal/6.1.1/corefine/epec/
  patches/
    cgal-6.1.1-fix-whatever.patch
```

## Output contract

After a successful build:

* every path listed in `runner.yaml` `build.outputs` exists
* the file referenced by `entry.command` exists
* `bin/build-info.json` exists and is valid JSON

Auxiliary artifacts (DLLs, wrappers, config files) belong in `bin/` or a subdirectory.

## `build-info.json`

`build-info.json` is the authoritative record of **what was actually built**. The meta-runner does not infer this; the build script must emit it.

```json
{
  "runner_id": "cgal-6.1.1-corefine-epec",
  "build_mode": "release",
  "timestamp_utc": "2026-03-10T12:34:56Z",
  "upstream": {
    "source": "git",
    "url": "https://github.com/CGAL/cgal.git",
    "requested_ref": "v6.1.1",
    "resolved_commit": "0123456789abcdef0123456789abcdef01234567",
    "resolved_version": "6.1.1"
  },
  "patches": [
    "patches/fix-build-warning.patch"
  ],
  "adapter": {
    "path": "src",
    "vcs_commit": "abcdef1234567890",
    "dirty": false
  },
  "toolchain": {
    "compiler": "gcc",
    "compiler_version": "13.2.0",
    "cmake_version": "3.29.1",
    "build_type": "Release"
  },
  "artifacts": [
    "bin/corefine_runner"
  ]
}
```

Field names may vary, but record **resolved facts** (commit hash, checksum, compiler version) not just requested intentions. If something cannot be determined, omit or null it rather than invent it.

## Recommended CLI

Not strictly required, but supported by every existing runner:

```text
./build.py --mode release      # default
./build.py --mode debug
./build.py --clean             # remove generated outputs
./build.py --force-download    # re-download upstream
```

`--mode release` should be the default and must work.

## Shared helpers

C++ runners share helpers under [`_common/`](../_common/):

* [`_common/build_helpers/python/builder_helpers.py`](../_common/build_helpers/python/builder_helpers.py) — Python helpers (`run`, `vcpkg_toolchain`, `gather_toolchain`, `write_build_info`, …).
* [`_common/cpp/include/`](../_common/cpp/include/) — shared C++ headers (`runner_utils`, `runner_mesh_helpers`).
* `_common/vcpkg/` — vcpkg submodule for C++ deps.

The helper module can only be imported *after* the build script has located the repo root. Use an inline sentinel walk in each `build.py` (look for `bootstrap-vcpkg.py`) rather than hardcoded `parents[N]` indexing — the depth changes when a runner is moved.

See [Adding a New Runner Guide](Adding%20a%20New%20Runner%20Guide.md) for the starter template.
