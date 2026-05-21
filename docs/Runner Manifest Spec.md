# Runner Manifest Spec

`runner.yaml` is the declarative description of a runner. One file per runner directory.

Companion docs: [Runner Overview](Runner%20Overview.md), [Build Script Spec](Build%20Script%20Spec.md), [Runner I/O Contract](Runner%20IO%20Contract.md).

## Sections

| Section | Required | Purpose |
|---|---|---|
| `runner` | yes | Folder-level identity |
| `entry` | yes | How the built artifact is launched |
| `variants` | yes | Public runtime variants and their capabilities |
| `upstream` | recommended | Where the source comes from (provenance) |
| `build` | recommended | How the runner is built (referenced by build-verification tooling) |
| `notes` | optional | Free-form list of human-readable notes (license, raw mesh format, timing semantics, …) |

The meta-runner currently parses only `runner.runner_id`, `runner.display_name`, the `entry` block, and the `variants` block. The other sections are conventions used by tooling and humans; keep them honest because that's what reviewers read.

## `runner`

```yaml
runner:
  family: cgal
  version: "6.1.1"
  algorithm: corefine          # optional; omit when the runner has only one algorithm
  material_variant: epec       # optional sub-path segment
  runner_id: cgal-6.1.1-corefine-epec
  display_name: "CGAL 6.1.1 Corefine (EPEC)"
```

| Field | Required | Notes |
|---|---|---|
| `family` | yes (convention) | Upstream project name |
| `version` | yes (convention) | Human-meaningful version grouping |
| `algorithm` | optional | Algorithm family inside the project. Omit if the runner has only one |
| `material_variant` | optional | Used when an additional path segment under `<algorithm>/` is justified |
| `runner_id` | **yes (parsed)** | Stable globally-unique slug. Should remain stable over time |
| `display_name` | optional | Human-friendly label used in plots and reports |

`runner_id` does not need to encode every path segment — it just needs to be unique and stable. `solidean-2026.1` and `cgal-6.1.1-corefine-epec` are both valid styles.

## `upstream`

Declare the exact source so a build can be reproduced. Pick a form that fits the source:

```yaml
upstream:
  source: git
  url: https://github.com/CGAL/cgal.git
  ref: v6.1.1
  # resolved_commit is recorded in bin/build-info.json by build.py
```

```yaml
upstream:
  source: archive
  url: https://example.org/foo-1.2.3.tar.gz
  version: 1.2.3
  sha256: deadbeef...
```

```yaml
upstream:
  source: url       # platform-specific downloads handled by build.py
  # resolved_sha256 recorded in bin/build-info.json
```

The `build-info.json` produced by `build.py` is the authoritative record of what was actually resolved — see [Build Script Spec](Build%20Script%20Spec.md).

## `build`

```yaml
build:
  script: ./build.py
  outputs:
    - bin/corefine_runner
    - bin/build-info.json
  default_mode: release
```

`outputs` should list at least the entry binary and `bin/build-info.json`. Build verification tooling checks these exist after `build.py` runs.

## `entry`

```yaml
entry:
  kind: executable           # or "python-uv"
  command: ./bin/corefine_runner
  args:
    - --request
    - "{request}"
    - --result
    - "{result}"
```

| Field | Notes |
|---|---|
| `kind` | `executable` or `python-uv`. Defaults to `executable` |
| `command` | Path (relative to the runner dir) to the launchable entrypoint |
| `args` | Base args. `{request}` and `{result}` are substituted by the meta-runner |

The full invocation is `<command> <entry.args (substituted)> <variant.args>`.

## `variants`

At least one public variant is required. Variants share the same built artifact and differ only in CLI args / env.

```yaml
variants:
  - id: base
    internal: true
    args: []
    capabilities:
      operations: [union, intersection, difference]
      input_formats: [off, obj, stl]
      requires_closed_meshes: true
      accepts_self_intersections: true
      accepts_non_manifold_inputs: false
      supports_components: true
      supports_variadic_booleans: false

  - id: regularized
    extends: base
    label: "Regularized (interior().closure() after each op)"
    display_name: "CGAL 6.1.1 Nef (EH-EI, Reg)"
    color: "#e6e600"
    args: ["--regularize"]
```

| Field | Required | Notes |
|---|---|---|
| `id` | yes | Stable identifier, unique within the runner |
| `internal` | optional | `true` hides the variant from benchmarking; useful for `base` variants used via `extends` |
| `extends` | optional | Parent variant `id`. Scalar fields inherit; `args` and `capabilities` are replaced as documented below |
| `label` | optional | Short trailing label appended to the runner's `display_name` (e.g. `"Runner — label"`) |
| `display_name` | optional | If set, replaces the entire assembled display name. Use this for short legend labels |
| `color` | optional | Hex color for plots (`"#rrggbb"`) |
| `args` | yes | CLI args appended after `entry.args` for this variant |
| `env` | optional | Per-variant environment variable map |
| `capabilities` | required (public variants) | What the variant is designed to handle. See below |

### Inheritance

* Scalar fields inherit unless overridden.
* `args` — child fully replaces parent (not concatenated). If you need to extend, restate the parent args.
* `capabilities` — shallow key-by-key override; child keys win.

## Capabilities

Capabilities describe what the variant is **designed and allowed** to handle, not what might accidentally work. The meta-runner uses them to decide whether the variant is applicable to a case.

```yaml
capabilities:
  operations: [union, intersection, difference]
  input_formats: [off, obj, stl]
  requires_closed_meshes: true
  accepts_self_intersections: true
  accepts_non_manifold_inputs: false
  supports_components: true
  supports_variadic_booleans: false
```

### Fields parsed by the meta-runner

| Field | Type | Meaning |
|---|---|---|
| `operations` | list of `union` / `intersection` / `difference` | Supported boolean operations |
| `requires_closed_meshes` | bool | If true, open meshes are rejected by contract |
| `accepts_self_intersections` | bool | If false, self-intersecting input is out of contract |
| `accepts_non_manifold_inputs` | bool | If false, non-manifold input is out of contract |

### Fields used as documentation hints

These appear in real runner.yamls but are not (yet) consumed by the meta-runner. Keep them honest — they are the per-variant truth a reviewer reads.

| Field | Meaning |
|---|---|
| `input_formats` | File formats the loader accepts (`off`, `obj`, `stl`, …) |
| `supports_components` | Disconnected components are accepted/preserved |
| `supports_variadic_booleans` | Variadic boolean ops (3+ args) are supported natively |

### Honesty rule

If a variant narrows its preconditions, the capability fields must reflect that. Speedups from narrower preconditions are legitimate **only** if the benchmark planner knows about the precondition.

## Full example

See [cgal/6.1.1/nef/exact_homogeneous_integer/runner.yaml](../cgal/6.1.1/nef/exact_homogeneous_integer/runner.yaml) for a complete manifest with two public variants (`regularized`, `non_regularized`) extending a single internal `base`.
