"""
runner.py — Blender 5.1 Boolean Modifier runner (runs inside Blender's Python).

Execution contract:
    blender --background --factory-startup --python runner.py -- \\
        --request <request.json> --result <result.json> --solver <EXACT|FAST>

Reads a boolean-benchmark request JSON, executes all runs, writes result JSON.

Timing conventions:
  load-mesh:
    total_ms    — start before obj_import, end after view_layer.update()
    io_ms       — bpy.ops.wm.obj_import (file read + parse into scene)
    import_ms   — bpy.context.view_layer.update() (mesh geometry evaluation)
    export_ms   — triangulate modifier apply + obj_export (timed together)
  boolean ops:
    total_ms      — start when native operands are ready (lhs/rhs in SSA),
                    end when modifier_apply completes
    operation_ms  — bpy.ops.object.modifier_apply only (the boolean itself)
    export_ms     — triangulate modifier apply + obj_export (timed together)

SSA: each op appends one Blender object name (string) to the ssa list.
     Objects are retrieved from bpy.data.objects by name.
     lhs is duplicated before each boolean op to avoid mutating SSA entries.
"""

from __future__ import annotations

import json
import sys
import time
from pathlib import Path

import bpy  # type: ignore  # available only inside Blender


# ---------------------------------------------------------------------------
# CLI parsing
# ---------------------------------------------------------------------------

def parse_args() -> tuple[str, str, str]:
    """
    Parse --request, --result, --solver from sys.argv after the '--' separator.
    Returns (request_path, result_path, solver).
    """
    # Blender passes everything after '--' as a plain list in sys.argv.
    # sys.argv[0] is the script path; args start at index 1.
    args = sys.argv[1:]

    def _get(flag: str) -> str | None:
        try:
            idx = args.index(flag)
            return args[idx + 1]
        except (ValueError, IndexError):
            return None

    request_path = _get("--request")
    result_path = _get("--result")
    solver = _get("--solver") or "EXACT"

    if not request_path:
        print("[error] --request is required", file=sys.stderr)
        sys.exit(1)
    if not result_path:
        print("[error] --result is required", file=sys.stderr)
        sys.exit(1)
    if solver not in ("EXACT", "FLOAT", "MANIFOLD"):
        print(f"[error] --solver must be EXACT, FLOAT, or MANIFOLD, got {solver!r}", file=sys.stderr)
        sys.exit(1)

    return request_path, result_path, solver


# ---------------------------------------------------------------------------
# Timer
# ---------------------------------------------------------------------------

class Timer:
    def __init__(self) -> None:
        self._start = time.perf_counter()

    def elapsed_ms(self) -> float:
        return (time.perf_counter() - self._start) * 1000.0


# ---------------------------------------------------------------------------
# Blender helpers
# ---------------------------------------------------------------------------

def blender_op_to_modifier(op_str: str) -> str:
    return {
        "boolean-union":        "UNION",
        "boolean-intersection": "INTERSECT",
        "boolean-difference":   "DIFFERENCE",
    }[op_str]


def rename_object(obj: "bpy.types.Object", new_name: str) -> None:
    obj.name = new_name
    if obj.data:
        obj.data.name = new_name


def export_object_to_obj(obj: "bpy.types.Object", filepath: str) -> float:
    """Triangulate obj and export it as OBJ. Returns export_ms (triangulate + write)."""
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    Path(filepath).parent.mkdir(parents=True, exist_ok=True)

    export_timer = Timer()

    tri_mod = obj.modifiers.new(name="triangulate", type="TRIANGULATE")
    bpy.ops.object.modifier_apply(modifier=tri_mod.name)

    bpy.ops.wm.obj_export(
        filepath=filepath,
        export_selected_objects=True,
        export_materials=False,
        export_uv=False,
        export_normals=False,
        export_colors=False,
    )

    return export_timer.elapsed_ms()


# ---------------------------------------------------------------------------
# Per-run execution
# ---------------------------------------------------------------------------

def execute_run(run_req: dict, solver: str) -> dict:
    run_res: dict = {}
    total_timer = Timer()
    ops_result: list[dict] = []
    failed = False

    try:
        ops = run_req["operations"]
        out_dir: str = run_req.get("out_dir", "")
        if not out_dir:
            raise RuntimeError("out_dir is required but was absent or empty")

        # Reset Blender scene before every run.
        bpy.ops.wm.read_factory_settings(use_empty=True)

        # SSA: list of object names, indexed by op index.
        ssa: list[str] = []

        for i, op in enumerate(ops):
            op_str: str = op["op"]
            op_res: dict = {}
            op_total_timer = Timer()

            print(f"  op {i}/{len(ops)}: {op_str}", flush=True)

            try:
                if op_str == "load-mesh":
                    path: str = op["path"]
                    obj_name = f"ssa_op_{i}"

                    # io phase — file read + parse into Blender scene
                    io_timer = Timer()
                    bpy.ops.wm.obj_import(filepath=path)
                    obj = bpy.context.selected_objects[0]
                    io_ms = io_timer.elapsed_ms()

                    rename_object(obj, obj_name)

                    # import phase — mesh geometry evaluation
                    import_timer = Timer()
                    bpy.context.view_layer.update()
                    import_ms = import_timer.elapsed_ms()

                    total_ms = op_total_timer.elapsed_ms()

                    ssa.append(obj_name)

                    file_path = str(Path(out_dir) / f"op_{i}.obj")
                    export_ms = export_object_to_obj(obj, file_path)

                    op_res["status"]    = "success"
                    op_res["total_ms"]  = total_ms
                    op_res["io_ms"]     = io_ms
                    op_res["import_ms"] = import_ms
                    op_res["export_ms"] = export_ms
                    op_res["file"]      = file_path

                elif op_str in ("boolean-union", "boolean-intersection", "boolean-difference"):
                    args_list = op.get("args", [])
                    if len(args_list) != 2:
                        raise _UnsupportedOp(
                            f"{op_str} requires exactly 2 args, got {len(args_list)}"
                        )

                    lhs_name = ssa[args_list[0]]
                    rhs_name = ssa[args_list[1]]
                    lhs_obj = bpy.data.objects[lhs_name]
                    rhs_obj = bpy.data.objects[rhs_name]

                    result_name = f"ssa_op_{i}"

                    # Start total timer when operands are ready.
                    # Duplicate lhs to avoid mutating the SSA entry.
                    bpy.ops.object.select_all(action="DESELECT")
                    lhs_obj.select_set(True)
                    bpy.context.view_layer.objects.active = lhs_obj
                    bpy.ops.object.duplicate(linked=False)
                    result_obj = bpy.context.selected_objects[0]
                    rename_object(result_obj, result_name)

                    # Clear any inherited modifiers before adding the boolean modifier
                    result_obj.modifiers.clear()

                    # Add boolean modifier
                    mod = result_obj.modifiers.new(name="bool", type="BOOLEAN")
                    mod.operation = blender_op_to_modifier(op_str)
                    mod.solver = solver
                    mod.object = rhs_obj

                    # operation phase — modifier_apply only
                    bpy.context.view_layer.objects.active = result_obj
                    op_timer = Timer()
                    bpy.ops.object.modifier_apply(modifier=mod.name)
                    operation_ms = op_timer.elapsed_ms()

                    total_ms = op_total_timer.elapsed_ms()

                    ssa.append(result_name)

                    file_path = str(Path(out_dir) / f"op_{i}.obj")
                    export_ms = export_object_to_obj(result_obj, file_path)

                    op_res["status"]       = "success"
                    op_res["total_ms"]     = total_ms
                    op_res["operation_ms"] = operation_ms
                    op_res["export_ms"]    = export_ms
                    op_res["file"]         = file_path

                else:
                    op_res["status"]   = "unsupported"
                    op_res["total_ms"] = 0.0
                    op_res["error"]    = f"unknown op: {op_str}"
                    ssa.append(f"ssa_op_{i}_empty")
                    failed = True

            except _UnsupportedOp as e:
                op_res["status"]   = "unsupported"
                op_res["total_ms"] = 0.0
                op_res["error"]    = str(e)
                ops_result.append(op_res)
                failed = True

            except Exception as e:
                op_res["status"]   = "crash"
                op_res["total_ms"] = op_total_timer.elapsed_ms()
                op_res["error"]    = str(e)
                failed = True

            ops_result.append(op_res)

            if failed:
                break  # fail-fast

        run_res["status"] = "crash" if failed else "success"

    except Exception as e:
        run_res["status"] = "crash"
        run_res["error"]  = str(e)

    run_res["duration_ms"] = total_timer.elapsed_ms()
    run_res["ops"]         = ops_result
    return run_res


class _UnsupportedOp(Exception):
    pass


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    request_path, result_path, solver = parse_args()

    print(f"[blender runner] request={request_path} result={result_path} solver={solver}",
          flush=True)

    # Read request
    try:
        with open(request_path, encoding="utf-8") as f:
            request = json.load(f)
    except Exception as e:
        print(f"[error] Failed to read request file: {e}", file=sys.stderr)
        return 1

    runs_req: list = request.get("runs", [])
    runs_res: list = []

    for run_req in runs_req:
        case_id = run_req["case_id"]
        print(f"[run] {case_id}", flush=True)
        run_res = execute_run(run_req, solver)
        run_res["case_id"] = case_id
        runs_res.append(run_res)

    result = {
        "kind":    request.get("kind", "boolean-benchmark"),
        "version": request.get("version", 1),
        "id":      request.get("id", ""),
        "runs":    runs_res,
    }

    # Write result
    try:
        Path(result_path).parent.mkdir(parents=True, exist_ok=True)
        with open(result_path, "w", encoding="utf-8") as f:
            json.dump(result, f, indent=2)
    except Exception as e:
        print(f"[error] Failed to write result file: {e}", file=sys.stderr)
        return 1

    print(f"[blender runner] wrote result to {result_path}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
