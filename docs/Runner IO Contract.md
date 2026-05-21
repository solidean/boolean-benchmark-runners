# Runner I/O Contract

A runner is a subprocess: reads `request.json`, runs the cases inside, writes `result.json`, exits.

Companion docs: [Runner Overview](Runner%20Overview.md), [Runner Manifest Spec](Runner%20Manifest%20Spec.md).

## CLI

```
<runner-entry> --request <request.json> --result <result.json>
```

The base args come from `runner.yaml` `entry.args`; the chosen variant appends its own `args`. Runtime settings (thread count, regularize, precision mode) live in those variant args — **not** in the request JSON.

## Request

```json
{
  "kind": "boolean-benchmark",
  "version": 1,
  "id": "a3f8c2d1-7e4b-4a0f-9c6e-b1d2e3f4a5b6",
  "system_snapshot": { "...": "..." },
  "runs": [
    {
      "case_id": "milling_step_037",
      "out_dir": ".../out/milling_step_037",
      "bounding_box": { "min": [-50, -50, -50], "max": [50, 50, 50] },
      "operations": [
        { "op": "load-mesh", "path": ".../workpiece_037.obj", "name": "workpiece_037.obj" },
        { "op": "load-mesh", "path": ".../tool_037.obj",      "name": "tool_037.obj" },
        { "op": "boolean-difference", "args": [0, 1], "name": "result" }
      ]
    }
  ]
}
```

### Top-level fields

| Field | Description |
|---|---|
| `kind` | Schema family, currently `"boolean-benchmark"` |
| `version` | Schema version (integer) |
| `id` | Random identifier; echo verbatim in the result |
| `runs` | Array of per-run objects (processed in order) |
| `system_snapshot` | Opaque object describing the host environment. Do not depend on its shape |

Ignore any other top-level fields.

### Per-run fields

| Field | Description |
|---|---|
| `case_id` | Stable run identifier; echo on the matching result entry |
| `out_dir` | Directory for per-op output files. Already exists |
| `bounding_box` | `{ "min": [x,y,z], "max": [x,y,z] }` over all load-mesh inputs of the run. Use it to size exact-arithmetic / spatial structures; safe to ignore otherwise |
| `operations` | Ordered SSA-style operations |

### Operations

Operations execute in order. Result of op `i` lives at SSA slot `i`. Later ops reference earlier ones by index.

| `op` | Required | Optional | Notes |
|---|---|---|---|
| `load-mesh` | `path` | `name` | Load mesh from disk. `path` is the materialized (typically hashed) path; `name` is the original filename, useful for logging |
| `boolean-union` | `args` | `name` | Union over the referenced slots |
| `boolean-intersection` | `args` | `name` | Intersection |
| `boolean-difference` | `args` | `name` | First arg minus the rest |

`args` semantics:

* **1 index** — self-operation (self-union for regularization, etc.)
* **2 indices** — binary
* **3+ indices** — variadic. Binary-only runners must return `status: "unsupported"`; the meta-runner only emits variadic ops to runners that advertise variadic support

For C++ runners, [`runner_utils::validate_op_boolean_binary`](../_common/cpp/include/runner_utils/ops.hh) handles binary-only validation and throws `unsupported_op` on 3+ args — catch it and report `unsupported`.

### Output files

Write the result of **every** operation — including `load-mesh` — to `<out_dir>/op_<N>.<ext>` (`N` = zero-based op index). Writing loads enables load-integrity checks and standalone viewer playback. Report the path in the per-op `file` field. Disk write time is **excluded** from `io_ms` / `import_ms` / `operation_ms` / `export_ms` / `preprocessing_ms`; it lands in `debug_total_ms` along with the rest of the per-op wall-clock.

## Result

Mirrors the request envelope:

```json
{
  "kind": "boolean-benchmark",
  "version": 1,
  "id": "a3f8c2d1-7e4b-4a0f-9c6e-b1d2e3f4a5b6",
  "runs": [
    {
      "case_id": "milling_step_037",
      "status": "success",
      "duration_ms": 152.4,
      "ops": [
        { "status": "success", "debug_total_ms": 5.1,   "io_ms": 2.1, "import_ms": 3.0,    "file": ".../op_0.obj" },
        { "status": "success", "debug_total_ms": 4.8,   "io_ms": 1.9, "import_ms": 2.9,    "file": ".../op_1.obj" },
        { "status": "success", "debug_total_ms": 142.5, "operation_ms": 139.2, "export_ms": 3.1, "file": ".../op_2.obj" }
      ]
    }
  ]
}
```

### Top-level fields

| Field | Description |
|---|---|
| `kind`, `version` | Match the request |
| `id` | Echo verbatim from the request — guards against wrong-result associations |
| `runs` | Array of per-run result objects |

Runners may add extra top-level fields to record their resolved runtime settings (e.g. the Nef variant echoes `"regularize": true/false` so result files are self-describing). The C++ helper accepts these via `extra_res_fields` on `run_main_loop`.

### Per-run fields

| Field | Required | Description |
|---|---|---|
| `case_id` | yes | Echoed from the request |
| `status` | yes | Overall run outcome (see status values below) |
| `duration_ms` | yes | Wall time for the run. Informational |
| `ops` | yes | Per-op results, index-aligned with the request `operations` |

A runner may stop at the first non-success op and emit a shorter `ops` array; missing entries are treated as not executed.

### Per-op fields

| Field | Required | Description |
|---|---|---|
| `status` | yes | Op outcome |
| `debug_total_ms` | yes | Raw per-op wall-clock including the output-file disk write. Diagnostic only — see timing below |
| `operation_ms` | boolean ops | Algorithmic core time on already-native inputs. Excludes io/import/export/preprocessing |
| `io_ms` | optional | File → declared raw mesh format (typically indexed-tris-f64) |
| `import_ms` | optional | Raw mesh format → native internal structure |
| `export_ms` | optional | Native internal structure → indexed-tris-f64. Disk write excluded |
| `preprocessing_ms` | optional | Reusable per-handle prep (BVH, spatial index). Never pair-specific work |
| `file` | yes | Path the runner wrote |
| `skipped_facets` | optional | Input facets rejected by the loader (load-mesh only) |
| `error` | on failure | Human-readable error message |

### Status values

`success` · `timeout` · `crash` · `invalid_input` · `invalid_output` · `unsupported`

Failures must be reported via `status` + `error`, not only via the exit code. Exit non-zero on failure too.

## Timing field semantics

`debug_total_ms` is the raw per-op wall-clock, **including the disk write of the output file**. Treat it as diagnostic only — it does not compose across ops, because summing it double-counts intermediate exports and disk writes that a real consumer would not pay if the next op reused the in-memory handle. Downstream consumers compute a clean `total_ms = import_ms + operation_ms + export_ms` from the explicit sub-fields below; the runner itself emits only `debug_total_ms`.

The other fields are sub-measurements with defined inclusion policies. Small uncategorized glue between phases is acceptable — there is no `misc_ms`.

| Field | load-mesh | boolean ops |
|---|---|---|
| `debug_total_ms` | required (diagnostic) | required (diagnostic) |
| `operation_ms` | — | required |
| `io_ms` | recommended | normally — |
| `import_ms` | recommended | optional |
| `export_ms` | — | optional (common) |
| `preprocessing_ms` | optional | optional |

* **`debug_total_ms`** — wall-clock for the entire per-op step, including the disk write of the result file. Diagnostic only — see composition guidance below.
* **`operation_ms`** — boolean computation on already-native inputs. Excludes io / import / export / preprocessing. Some runners cannot separate the library's internal import/export from its boolean call — those should document what `operation_ms` actually covers in `runner.yaml` notes.
* **`io_ms`** — file read + parse, up to (not including) native conversion.
* **`import_ms`** — raw mesh format → native structure.
* **`export_ms`** — native structure → indexed-tris-f64. Disk write excluded.
* **`preprocessing_ms`** — reusable per-handle prep that persists across ops on the same handle (BVH, spatial index). Never pair-specific work. Runners must document what goes in here.

### Composing timings for benchmarking

Don't sum `debug_total_ms` across ops. The compositions that mean something:

* **End-to-end (mirrors "load → ops → save" user experience)**:
  `Σ load-mesh.import_ms + Σ boolean.operation_ms + last boolean.export_ms`
* **Composable pipeline cost (extends cleanly when more ops are appended)**:
  `Σ load-mesh.import_ms + Σ boolean.operation_ms`
  Drop export entirely — you don't pay for intermediate exports when the result is consumed in-memory by the next op.
* **Per-op core cost (small input set, many ops on top of it)**:
  `Σ boolean.operation_ms`
  Useful when you have a fixed set of inputs and run many booleans against them; per-input prep (BVH, spatial index) amortizes away. This composition is honest **only** because `preprocessing_ms` excludes pair-specific or cross-input work — if a runner snuck that into `operation_ms`, this comparison would be misleading.

Whether to include `io_ms` depends on the benchmark question: "given files on disk" → add it; "given meshes in memory" → don't.

### Soft invariants

Approximate relationships, not enforced numerically:

* load-mesh: `debug_total_ms ≈ io_ms + import_ms + <disk write of the load's output file>`
* boolean ops: `debug_total_ms ≈ operation_ms + export_ms + <disk write of the result file>`

The disk-write portion of `debug_total_ms` is normally small but can be substantial for large meshes or slow filesystems — another reason to use the explicit sub-fields, not `debug_total_ms`, for benchmarking.

Runners must document their raw mesh format and what each `*_ms` field covers — usually in `runner.yaml` notes. See [solidean/2026.1/runner.yaml](../solidean/2026.1/runner.yaml) for an example.

## Multi-run protocol

Process all entries in `runs` in order. Two reasons to flush partial results to disk during the loop:

1. The meta-runner enforces a memory/time watchdog and can SIGKILL the process. Without periodic flushing, completed cases would be lost.
2. Mid-flush kills must not leave half-written JSON.

The reference C++ loop ([`runner_utils/run_loop.hh`](../_common/cpp/include/runner_utils/run_loop.hh)) flushes every ~10s by writing `<result_path>.tmp` and renaming over `<result_path>`. New runners should match this.

A missing or malformed result file is treated by the meta-runner as a crash of the whole batch. Result files missing individual `case_id`s are treated as crashes of just those runs.
