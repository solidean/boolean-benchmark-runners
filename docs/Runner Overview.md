# Runner Overview

A runner is a standalone, buildable benchmark target for one Boolean implementation. Runners are subprocesses invoked by the meta-runner. Each lives in its own directory, builds independently, and conforms to a shared CLI + JSON I/O contract.

This is the entry point for the runner docs. Schema-level detail lives in the sibling docs at the bottom of this page.

## Core concepts

**Runner** — one buildable unit in one directory, containing `runner.yaml`, `build.py`, source/scripts, and (after build) a `bin/` directory with the executable and `bin/build-info.json`.

**Runtime variant** — a named configuration declared in `runner.yaml` that reuses the same built artifact and differs only via CLI args / env. Examples: `default`, `threads_1`, `assume_non_self_intersecting`.

**Folder variant** — a separate runner folder for a materially different implementation, algorithm family, or build product. Examples: CGAL `corefine/` vs `nef/`; CGAL 5.6 vs 6.1.1.

**Material variant** — an additional path segment below `<algorithm>/` for distinct sub-implementations (e.g. exact vs approximate backends with substantially different code paths). Use sparingly.

## Folder hierarchy

```text
runners/<family>/<version>/<algorithm>/[material-variant]/
```

`<algorithm>` is optional — omit it when the runner has only one. Examples:

* `runners/solidean/2026.1/` — single-algorithm runner
* `runners/cgal/6.1.1/corefine/epec/` — algorithm + material variant
* `runners/cgal/6.1.1/nef/exact_homogeneous_integer/` — algorithm + material variant

## Required files

```text
<runner-dir>/
  runner.yaml          # identity, build outputs, entry, variants — see Runner Manifest Spec
  build.py             # build script — see Build Script Spec
  .gitignore           # ignores bin/ and download/
  src/                 # optional; adapter code (C++/Rust/…)
  script/              # optional; Python/shell entrypoints
  patches/             # optional; upstream patches applied during build
  bin/                 # produced by build.py
    <runner_executable>
    build-info.json
```

Mandatory:

* `runner.yaml`
* `build.py`
* After build: `bin/build-info.json` and at least the entry executable declared in `runner.yaml`

Runners must not assume:

* a shared repo-level cache contract
* Docker or container execution
* that upstream sources are already checked out somewhere
* that the meta-runner can infer build provenance on its own

Each runner manages its own download/build/cache. `download/` is transient; deleting it should be a clean way to force a rebuild.

## Folder variant vs runtime variant

This is the central design rule. Get it right.

**Folder variant** when the difference is structural:

* separate source tree, executable, or build logic
* different algorithm family
* different dependency graph
* large semantic difference that deserves explicit physical identity

**Runtime variant** when the difference is parameterization of the same built runner:

* thread count
* repair on/off
* assume manifold / non-self-intersecting input
* heuristic toggle
* tolerance mode exposed through CLI flags

If you'd end up writing awkward branching inside one runner folder, split into folders. If it's naturally a runtime flag on the same artifact, keep it in `variants`.

## Execution model

1. The meta-runner discovers runners by locating `runner.yaml` files under [the runners root](../).
2. It selects applicable variants by matching each variant's capabilities against the benchmark case requirements.
3. For each selected variant, it assembles a command line from `entry.command` + `entry.args` (with `{request}` / `{result}` substituted) + the variant's `args`.
4. It writes a batch `request.json` and invokes the runner.
5. The runner executes every case in the request and writes a `result.json` to the requested path.
6. The meta-runner reads the result, appends system parameters, and aggregates.

A runner may be a native executable or a script-based wrapper (Blender, Python tool, etc.). The contract is the same.

## Runner identity

A runner has two identities:

* **Path identity** — `<family>/<version>/<algorithm>/[material-variant]`. For humans.
* **Manifest identity** — `runner_id` declared in `runner.yaml`. Globally unique and stable. Used by the meta-runner.

A full method identity for benchmarking purposes is `runner_id + variant_id`, e.g. `cgal-6.1.1-corefine-epec + default`.

## Doc map

| Doc | Contents |
|---|---|
| **Runner Overview** *(this doc)* | Concepts, folder hierarchy, execution model, identity |
| [Runner Manifest Spec](Runner%20Manifest%20Spec.md) | `runner.yaml` fields, variant inheritance, capabilities |
| [Build Script Spec](Build%20Script%20Spec.md) | `build.py` role, `uv` pattern, `build-info.json` |
| [Runner I/O Contract](Runner%20IO%20Contract.md) | Request/result JSON, timing fields, status values, multi-run protocol |
| [Adding a New Runner Guide](Adding%20a%20New%20Runner%20Guide.md) | Step-by-step, checklist, common pitfalls, starter templates |
