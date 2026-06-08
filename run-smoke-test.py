"""Run the smoke request against all built runners and report a markdown table.

Standalone developer utility: discovers every runner variant under ``runners/``,
runs each built one against ``tests/smoke/smoke-request.json`` (cwd = the smoke
folder, so the relative mesh paths resolve), collects ``op_2.obj`` per runner,
and computes a results table like ``suites/smoke/analyze.py`` — but sourced from
the collected result folder, computing geometry stats itself (the raw runner
``result.json`` carries timing only, no geometry).

Usage:
    python run-smoke-test.py                 # all built variants
    python run-smoke-test.py -r solidean     # only variants matching slug/name
    python run-smoke-test.py -r cgal -r blender
"""

import argparse
import json
import math
import os
import shutil
import subprocess
import sys
from pathlib import Path

import yaml

# ---- Layout ----------------------------------------------------------------

RUNNERS_DIR = Path(__file__).resolve().parent
SMOKE_DIR   = RUNNERS_DIR / "tests" / "smoke"
REQUEST     = SMOKE_DIR / "smoke-request.json"
OUTPUT_DIR  = SMOKE_DIR / "smoke-output"
RESULT_DIR  = SMOKE_DIR / "result"

# Expected geometry of A − B (from suites/smoke/cube-minus-cube.case).
EXPECT_VOLUME = 0.496
EXPECT_AREA   = 6.0
TOL           = 0.001

# The boolean-difference is op index 2 in smoke-request.json.
RESULT_OP_INDEX = 2

# Per-runner subprocess timeout.
RUN_TIMEOUT_S = 120

# Directories under runners/ that are not runners.
SKIP_DIRS = {"_common", "docs", "tests"}


# ---- Discovery -------------------------------------------------------------


def discover_variants():
    """Walk ``runners/**/runner.yaml`` and expand into one record per public variant.

    Returns a list of dicts with the slug, resolved display name, the entry
    command/args, the variant args, and whether the runner is built.
    """
    variants = []
    for manifest_path in sorted(RUNNERS_DIR.glob("**/runner.yaml")):
        rel = manifest_path.relative_to(RUNNERS_DIR)
        if rel.parts and rel.parts[0] in SKIP_DIRS:
            continue
        try:
            manifest = yaml.safe_load(manifest_path.read_text(encoding="utf-8"))
        except (OSError, yaml.YAMLError) as exc:
            print(f"warning: skipping {rel}: {exc}", file=sys.stderr)
            continue

        runner_dir = manifest_path.parent
        runner   = manifest.get("runner", {})
        runner_id   = runner.get("runner_id")
        runner_name = runner.get("display_name", runner_id)
        entry    = manifest.get("entry", {})
        entry_kind = entry.get("kind", "executable")
        command    = entry.get("command")
        entry_args = entry.get("args", [])
        if not runner_id or not command:
            continue

        built = is_built(runner_dir, entry_kind, command)
        # One row per non-internal ("base") variant; slug mirrors session naming.
        for variant in manifest.get("variants", []):
            if variant.get("internal"):
                continue
            variants.append({
                "slug": f"{runner_id}.{variant['id']}",
                "display_name": resolve_display_name(runner_name, variant),
                "runner_dir": runner_dir,
                "entry_kind": entry_kind,
                "command": command,
                "entry_args": entry_args,
                "variant_args": variant.get("args", []),
                "built": built,
            })
    return variants


def resolve_display_name(runner_name, variant):
    """Resolve the human-readable name: explicit variant name, else runner + label."""
    if variant.get("display_name"):
        return variant["display_name"]
    label = variant.get("label")
    return f"{runner_name} — {label}" if label else runner_name


def resolve_command(runner_dir, entry_kind, command):
    """Resolve the entry command to an absolute path, appending ``.exe`` on Windows."""
    path = (runner_dir / command).resolve()
    if entry_kind == "executable" and os.name == "nt" and path.suffix == "":
        exe = path.with_suffix(".exe")
        if exe.exists():
            return exe
    return path


def is_built(runner_dir, entry_kind, command):
    """A runner is built when build-info.json exists and (executables) the binary exists."""
    if not (runner_dir / "bin" / "build-info.json").exists():
        return False
    if entry_kind == "executable":
        return resolve_command(runner_dir, entry_kind, command).exists()
    return True  # python-uv: the driver ships in src/, build-info gates the download


def matches_patterns(variant, patterns):
    """True when no patterns are given, or any pattern substring-matches slug/name."""
    if not patterns:
        return True
    hay = f"{variant['slug']}\n{variant['display_name']}".lower()
    return any(p.lower() in hay for p in patterns)


# ---- Invocation ------------------------------------------------------------


def build_command(variant, request_path, result_path):
    """Assemble the argv: ``<command> <entry.args substituted> <variant.args>``."""
    command = resolve_command(variant["runner_dir"], variant["entry_kind"], variant["command"])
    subst = {"{request}": str(request_path), "{result}": str(result_path)}
    args = [subst.get(a, a) for a in variant["entry_args"]] + list(variant["variant_args"])
    if variant["entry_kind"] == "python-uv":
        return ["uv", "run", str(command), *args]
    return [str(command), *args]


def clear_dir(path):
    """Remove ``path`` and recreate it empty, so no stale outputs survive."""
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True, exist_ok=True)


def run_variant(variant):
    """Run one variant against the smoke request; return its parsed run result or None.

    Clears the output dir first, copies ``op_2.obj`` into the per-runner result
    folder, and persists the runner's ``result.json`` alongside it.
    """
    clear_dir(OUTPUT_DIR)
    dest = RESULT_DIR / variant["slug"]
    clear_dir(dest)
    result_path = dest / "result.json"

    cmd = build_command(variant, REQUEST, result_path)
    try:
        proc = subprocess.run(
            cmd, cwd=SMOKE_DIR, capture_output=True, text=True, timeout=RUN_TIMEOUT_S,
        )
    except subprocess.TimeoutExpired:
        print(f"  {variant['slug']}: TIMEOUT after {RUN_TIMEOUT_S}s", file=sys.stderr)
        return None
    except OSError as exc:
        print(f"  {variant['slug']}: failed to launch: {exc}", file=sys.stderr)
        return None

    if proc.returncode != 0:
        # Non-zero is tolerated — the result.json (if any) still tells the story.
        print(f"  {variant['slug']}: exit {proc.returncode}", file=sys.stderr)

    # Collect the boolean result obj.
    op_obj = OUTPUT_DIR / f"op_{RESULT_OP_INDEX}.obj"
    if op_obj.exists():
        shutil.copyfile(op_obj, dest / "result.obj")

    if not result_path.exists():
        return None
    try:
        data = json.loads(result_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    runs = data.get("runs", [])
    return runs[0] if runs else None


# ---- Geometry --------------------------------------------------------------


def _vertex_index(token, n_verts):
    """Parse an OBJ face token (``v``, ``v/vt``, ``v//vn``) into a 0-based index."""
    raw = int(token.split("/")[0])
    return raw - 1 if raw > 0 else n_verts + raw  # negative = relative to end


def mesh_stats(obj_path):
    """Parse an OBJ and compute triangle/vertex counts, surface area and volume.

    Faces are fan-triangulated; volume uses the divergence theorem over the
    triangulation. Returns None if the file is missing.
    """
    if not obj_path or not obj_path.exists():
        return None

    verts = []
    tris = 0
    area = 0.0
    vol6 = 0.0
    with open(obj_path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("v "):
                _, x, y, z = line.split()[:4]
                verts.append((float(x), float(y), float(z)))
            elif line.startswith("f "):
                idx = [_vertex_index(t, len(verts)) for t in line.split()[1:]]
                # Fan-triangulate the polygon: (0, i, i+1).
                for i in range(1, len(idx) - 1):
                    a, b, c = verts[idx[0]], verts[idx[i]], verts[idx[i + 1]]
                    ab = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
                    ac = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
                    cx = (ab[1] * ac[2] - ab[2] * ac[1],
                          ab[2] * ac[0] - ab[0] * ac[2],
                          ab[0] * ac[1] - ab[1] * ac[0])
                    area += 0.5 * math.sqrt(cx[0] ** 2 + cx[1] ** 2 + cx[2] ** 2)
                    # Signed volume of the tetrahedron (origin, a, b, c).
                    vol6 += a[0] * (b[1] * c[2] - b[2] * c[1]) \
                        + a[1] * (b[2] * c[0] - b[0] * c[2]) \
                        + a[2] * (b[0] * c[1] - b[1] * c[0])
                    tris += 1
    return {
        "tri_count": tris,
        "vertex_count": len(verts),
        "area": area,
        "volume": abs(vol6) / 6.0,
    }


# ---- Rendering -------------------------------------------------------------


def fmt_float(v, decimals=4):
    return "—" if v is None else f"{v:.{decimals}f}"


def fmt_int(v):
    return "—" if v is None else str(v)


def fmt_ms(v):
    # Treat exact 0 as "not emitted" — missing timing fields default to 0.0.
    if v is None or v <= 0.0:
        return "—"
    return f"{v:.1f}"


def is_success(run_result, stats):
    """Success = boolean op succeeded, result obj present, geometry within tolerance."""
    if run_result is None or stats is None:
        return False
    ops = run_result.get("ops", [])
    if len(ops) <= RESULT_OP_INDEX or ops[RESULT_OP_INDEX].get("status") != "success":
        return False
    return (abs(stats["volume"] - EXPECT_VOLUME) <= TOL
            and abs(stats["area"] - EXPECT_AREA) <= TOL)


def build_row(slug, run_result, stats):
    """Render one markdown table row for a runner's outcome (or *not run*)."""
    if run_result is None and stats is None:
        return f"| {slug} | *not run* | — | — | — | — | — | — | — |"

    ops = run_result.get("ops", []) if run_result else []
    import_ms    = sum(op.get("import_ms", 0.0)    for op in ops)
    operation_ms = sum(op.get("operation_ms", 0.0) for op in ops)
    export_ms    = sum(op.get("export_ms", 0.0)    for op in ops)

    check = "✅" if is_success(run_result, stats) else "❌"
    area   = fmt_float(stats["area"])         if stats else "—"
    volume = fmt_float(stats["volume"])       if stats else "—"
    tris   = fmt_int(stats["tri_count"])      if stats else "—"
    verts  = fmt_int(stats["vertex_count"])   if stats else "—"

    return (f"| {slug} | {fmt_ms(import_ms)} | {fmt_ms(operation_ms)} | {fmt_ms(export_ms)} "
            f"| {check} | {area} | {volume} | {tris} | {verts} |")


def render(rows_data):
    """Render the full markdown document. rows_data: list of (slug, run_result, stats)."""
    ran    = [r for r in rows_data if r[1] is not None]
    passed = sum(1 for slug, rr, st in ran if is_success(rr, st))
    lines = [
        "# Smoke Test Results",
        "",
        f"**Runners:** {len(rows_data)} total — {passed} passed, "
        f"{len(ran) - passed} failed, {len(rows_data) - len(ran)} not run",
        "",
        "| Runner | Import (ms) | Op (ms) | Export (ms) | ✓ | Area | Volume | Tris | Verts |",
        "|:---|---:|---:|---:|:---:|---:|---:|---:|---:|",
    ]
    lines += [build_row(slug, rr, st) for slug, rr, st in rows_data]
    lines.append("")
    return "\n".join(lines)


# ---- Main ------------------------------------------------------------------


def main():
    parser = argparse.ArgumentParser(description="Run the smoke request against built runners.")
    parser.add_argument(
        "-r", "--runner", action="append", dest="patterns", metavar="PATTERN",
        help="Only run variants whose slug or name matches (repeatable; OR semantics).",
    )
    args = parser.parse_args()

    # The table uses Unicode (✅/❌/—); force utf-8 so it prints on cp1252 consoles.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")

    variants = discover_variants()
    rows_data = []  # (slug, run_result_or_None, stats_or_None), in display order
    for variant in sorted(variants, key=lambda v: v["slug"]):
        if not variant["built"] or not matches_patterns(variant, args.patterns):
            rows_data.append((variant["slug"], None, None))
            continue
        print(f"running {variant['slug']} ...", file=sys.stderr)
        run_result = run_variant(variant)
        stats = mesh_stats(RESULT_DIR / variant["slug"] / "result.obj")
        rows_data.append((variant["slug"], run_result, stats))

    markdown = render(rows_data)
    print(markdown)

    RESULT_DIR.mkdir(parents=True, exist_ok=True)
    (RESULT_DIR / "smoke-results.md").write_text(markdown, encoding="utf-8")


if __name__ == "__main__":
    main()
