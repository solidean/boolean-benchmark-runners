// ---------------------------------------------------------------------------
// main.cpp — cork_runner entry point
//
// Execution contract:
//   cork_runner --request <request.json> --result <result.json>
//
// Uses cork's C API (cork.h) for union, intersection, and difference
// on triangle meshes.
//
// Requires closed solid inputs (validated via isSolid()).
//
// This runner is binary-only: boolean ops with 3+ args return 'unsupported'.
// The meta-runner generates sequences of binary ops for iterated booleans.
//
// Timing conventions (per-op fields):
//   load-mesh:
//     total_ms   — start before file open, end when TriMesh is in SSA
//     io_ms      — file → flat double verts + int indices
//                  (runner_mesh_helpers::loadFromFileIndexed)
//     import_ms  — double→float temp + isSolid() validation; excludes disk I/O
//   boolean ops:
//     total_ms      — start when native operands are ready, end when result is ready
//     operation_ms  — double→float conversion + computeUnion/computeIntersection/
//                     computeDifference + float→double copy + freeCorkTriMesh
//     export_ms     — disk-write preparation (trivial: data already in double/int)
//
// Ownership model:
//   SSA stores plain TriMesh structs (vector<double> verts, vector<int> tris).
//   For each boolean op, temporary float Cork meshes are built from the TriMesh
//   operands, the Cork op is called, the result is immediately copied into a new
//   TriMesh, and freeCorkTriMesh is called right there.  No Cork-allocated
//   buffers survive past apply_op.  No custom destructors or move semantics needed.
// ---------------------------------------------------------------------------

#include <cork.h>
#include <nlohmann/json.hpp>
#include <runner_mesh_helpers/io.hh>
#include <runner_utils/cli.hh>
#include <runner_utils/ops.hh>
#include <runner_utils/run_loop.hh>
#include <runner_utils/timer.hh>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <iostream>

using json = nlohmann::json;
using runner_utils::print_progress_op;
using runner_utils::Timer;
using runner_utils::validate_op_boolean_binary;

// ---------------------------------------------------------------------------
// Plain mesh storage — no special members, safe default copy/move
// ---------------------------------------------------------------------------

struct TriMesh
{
    std::vector<double> verts;  // flat x,y,z per vertex
    std::vector<int>    tris;   // flat i0,i1,i2 per triangle
};

// ---------------------------------------------------------------------------
// Operation helpers
// ---------------------------------------------------------------------------

enum class OpKind
{
    Union,
    Intersection,
    Difference
};

// Converts TriMesh operands to temporary Cork float meshes, calls the
// appropriate Cork boolean function, copies the result into a new TriMesh,
// and frees the Cork-allocated buffers immediately before returning.
static TriMesh apply_op(TriMesh const& lhs, TriMesh const& rhs, OpKind kind)
{
    // Downcast double→float for Cork input operands (temporary, stack-lifetime).
    std::vector<float>        lv(lhs.verts.begin(), lhs.verts.end());
    std::vector<unsigned int> lt(lhs.tris.begin(),  lhs.tris.end());
    std::vector<float>        rv(rhs.verts.begin(), rhs.verts.end());
    std::vector<unsigned int> rt(rhs.tris.begin(),  rhs.tris.end());

    CorkTriMesh cm_lhs{}, cm_rhs{}, cm_out{};
    cm_lhs.n_vertices  = static_cast<unsigned int>(lv.size() / 3);
    cm_lhs.n_triangles = static_cast<unsigned int>(lt.size() / 3);
    cm_lhs.vertices    = lv.data();
    cm_lhs.triangles   = lt.data();
    cm_rhs.n_vertices  = static_cast<unsigned int>(rv.size() / 3);
    cm_rhs.n_triangles = static_cast<unsigned int>(rt.size() / 3);
    cm_rhs.vertices    = rv.data();
    cm_rhs.triangles   = rt.data();

    std::cout << "BEFORE CORK OP" << std::endl;
    std::cout << "  cm_lhs.n_vertices = " << cm_lhs.n_vertices << std::endl;
    std::cout << "  cm_lhs.n_triangles = " << cm_lhs.n_triangles << std::endl;
    std::cout << "  cm_rhs.n_vertices = " << cm_rhs.n_vertices << std::endl;
    std::cout << "  cm_rhs.n_triangles = " << cm_rhs.n_triangles << std::endl;
    std::cout << "  isSolid(cm_lhs) = " << isSolid(cm_lhs) << std::endl;
    std::cout << "  isSolid(cm_rhs) = " << isSolid(cm_rhs) << std::endl;
    switch (kind)
    {
    case OpKind::Union:        computeUnion       (cm_lhs, cm_rhs, &cm_out); break;
    case OpKind::Intersection: computeIntersection(cm_lhs, cm_rhs, &cm_out); break;
    case OpKind::Difference:   computeDifference  (cm_lhs, cm_rhs, &cm_out); break;
    }
    std::cout << "AFTER CORK OP" << std::endl;

    // Copy Cork result into TriMesh (float→double upcast), then free immediately.
    TriMesh result;
    result.verts.assign(cm_out.vertices,  cm_out.vertices  + 3 * cm_out.n_vertices);
    result.tris.assign (cm_out.triangles, cm_out.triangles + 3 * cm_out.n_triangles);
    freeCorkTriMesh(&cm_out);

    return result;
}


// ---------------------------------------------------------------------------
// Per-run execution
// ---------------------------------------------------------------------------

static json execute_run(runner_utils::Config const& /*cfg*/, json const& run_req)
{
    json run_res;
    Timer total_timer;
    json ops_result = json::array();
    bool failed = false;

    try
    {
        auto const& ops = run_req.at("operations");
        std::string const out_dir = run_req.at("out_dir").get<std::string>();
        if (out_dir.empty())
            throw std::runtime_error("out_dir is required but was empty");

        // SSA storage: ssa[i] = result of op i.
        // Plain TriMesh structs — no custom destructors, no ownership complexity.
        std::vector<TriMesh> ssa;
        ssa.reserve(ops.size());

        for (std::size_t i = 0; i < ops.size(); ++i)
        {
            auto const& op = ops[i];
            std::string const op_str = op.at("op").get<std::string>();
            json op_res;
            Timer op_total_timer;

            print_progress_op(i, ops);

            try
            {
                if (op_str == "load-mesh")
                {
                    std::string const path = op.at("path").get<std::string>();

                    // io phase — file → flat double verts + int indices
                    Timer io_timer;
                    auto [verts, tris] = runner_mesh_helpers::loadFromFileIndexed(path);
                    double const io_ms = io_timer.elapsed_ms();

                    // import phase — build temp float Cork mesh for isSolid() check
                    Timer import_timer;

                    std::vector<float>        fv(verts.begin(), verts.end());
                    std::vector<unsigned int> ft(tris.begin(),  tris.end());
                    CorkTriMesh cm{};
                    cm.n_vertices  = static_cast<unsigned int>(fv.size() / 3);
                    cm.n_triangles = static_cast<unsigned int>(ft.size() / 3);
                    cm.vertices    = fv.data();
                    cm.triangles   = ft.data();

                    if (!isSolid(cm))
                        throw std::runtime_error("cork: isSolid() returned false — "
                                                 "mesh is not a closed solid");

                    TriMesh owned;
                    owned.verts = std::move(verts);
                    owned.tris  = std::move(tris);

                    double const import_ms = import_timer.elapsed_ms();

                    // Disk write — not timed.
                    std::string const file_path = out_dir + "/op_" + std::to_string(i) + ".obj";
                    runner_mesh_helpers::saveToFileIndexed(file_path, owned.verts, owned.tris);

                    ssa.push_back(std::move(owned));

                    op_res["status"]    = "success";
                    op_res["total_ms"]  = op_total_timer.elapsed_ms();
                    op_res["io_ms"]     = io_ms;
                    op_res["import_ms"] = import_ms;
                    op_res["file"]      = file_path;
                }
                else if (op_str == "boolean-union" || op_str == "boolean-intersection" || op_str == "boolean-difference")
                {
                    validate_op_boolean_binary(i, ops, "cork runner");

                    auto const args = op.at("args").get<std::vector<int>>();

                    OpKind const kind = (op_str == "boolean-union")        ? OpKind::Union
                                      : (op_str == "boolean-intersection") ? OpKind::Intersection
                                                                           : OpKind::Difference;

                    TriMesh const& lhs = ssa[static_cast<std::size_t>(args[0])];
                    TriMesh const& rhs = ssa[static_cast<std::size_t>(args[1])];

                    // Boolean phase — includes double→float conversion, Cork call,
                    // float→double copy, and eager freeCorkTriMesh.
                    Timer op_timer;
                    TriMesh result = apply_op(lhs, rhs, kind);
                    double const operation_ms = op_timer.elapsed_ms();

                    // Export phase — data already in double/int; trivial disk prep.
                    Timer export_timer;
                    std::string const file_path = out_dir + "/op_" + std::to_string(i) + ".obj";
                    double const export_ms = export_timer.elapsed_ms();

                    // Disk write — not timed.
                    runner_mesh_helpers::saveToFileIndexed(file_path, result.verts, result.tris);

                    ssa.push_back(std::move(result));

                    op_res["status"]       = "success";
                    op_res["total_ms"]     = op_total_timer.elapsed_ms();
                    op_res["operation_ms"] = operation_ms;
                    op_res["export_ms"]    = export_ms;
                    op_res["file"]         = file_path;
                }
                else
                {
                    op_res["status"]   = "unsupported";
                    op_res["total_ms"] = 0.0;
                    op_res["error"]    = "unknown op: " + op_str;
                    ssa.push_back(TriMesh{});
                    failed = true;
                }
            }
            catch (runner_utils::unsupported_op const& e)
            {
                op_res["status"]   = "unsupported";
                op_res["total_ms"] = 0.0;
                op_res["error"]    = e.what();
                failed = true;
            }
            catch (std::exception const& e)
            {
                op_res["status"]   = "crash";
                op_res["total_ms"] = op_total_timer.elapsed_ms();
                op_res["error"]    = e.what();
                failed = true;
            }
            catch (...)
            {
                op_res["status"]   = "crash";
                op_res["total_ms"] = op_total_timer.elapsed_ms();
                op_res["error"]    = "unknown exception";
                failed = true;
            }

            ops_result.push_back(op_res);

            if (failed)
                break; // fail-fast
        }

        run_res["status"] = failed ? "crash" : "success";
    }
    catch (std::exception const& e)
    {
        run_res["status"] = "crash";
        run_res["error"]  = e.what();
    }
    catch (...)
    {
        run_res["status"] = "crash";
        run_res["error"]  = "unknown exception";
    }

    run_res["duration_ms"] = total_timer.elapsed_ms();
    run_res["ops"]         = ops_result;
    return run_res;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char** argv)
{
    runner_utils::Config const cfg = runner_utils::parse_args(argc, argv, "cork_runner");
    return runner_utils::run_main_loop(cfg, execute_run);
}
