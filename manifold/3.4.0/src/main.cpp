// ---------------------------------------------------------------------------
// main.cpp — manifold_runner entry point
//
// Execution contract:
//   manifold_runner --request <request.json> --result <result.json>
//
// Uses manifold::Manifold::Boolean for union, intersection, and difference
// on manifold meshes.
//
// Requires closed, manifold, non-self-intersecting inputs.
// Invalid meshes are caught at import time (Status() != NoError).
//
// This runner is binary-only: boolean ops with 3+ args return 'unsupported'.
// The meta-runner generates sequences of binary ops for iterated booleans.
//
// Timing conventions (per-op fields):
//   load-mesh:
//     total_ms   — start before file open, end when Manifold is in SSA
//     io_ms      — file → flat double verts + int indices
//                  (runner_mesh_helpers::loadFromFileIndexed)
//     import_ms  — flat f64 verts+indices → manifold::ManifoldGL64 → manifold::Manifold
//                  + Status() validation; excludes disk I/O
//   boolean ops:
//     total_ms      — start when native operands are ready, end when result is ready
//     operation_ms  — Boolean() call only
//     export_ms     — GetMeshGL64() (double vertex data extraction; excludes disk write)
// ---------------------------------------------------------------------------

#include <manifold/manifold.h>
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


using json = nlohmann::json;
using runner_utils::print_progress_op;
using runner_utils::Timer;
using runner_utils::validate_op_boolean_binary;

// ---------------------------------------------------------------------------
// Operation helpers
// ---------------------------------------------------------------------------

enum class OpKind
{
    Union,
    Intersection,
    Difference
};

// Boolean() is const and returns a new Manifold — pass operands by const ref.
// Returns {result_mesh, ok}.  ok==false means the result status is not NoError.
static std::pair<manifold::Manifold, bool> apply_op(manifold::Manifold const& lhs, manifold::Manifold const& rhs, OpKind kind)
{
    manifold::OpType op_type;
    switch (kind)
    {
    case OpKind::Union:        op_type = manifold::OpType::Add;       break;
    case OpKind::Intersection: op_type = manifold::OpType::Intersect; break;
    case OpKind::Difference:   op_type = manifold::OpType::Subtract;  break;
    }
    manifold::Manifold result = lhs.Boolean(rhs, op_type);
    bool ok = (result.Status() == manifold::Manifold::Error::NoError);
    return {std::move(result), ok};
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
        std::vector<manifold::Manifold> ssa;
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

                    // import phase — flat f64 → manifold::MeshGL64 → manifold::Manifold
                    Timer import_timer;

                    manifold::MeshGL64 meshgl;
                    meshgl.numProp = 3;
                    meshgl.vertProperties = std::move(verts);
                    meshgl.triVerts.reserve(tris.size());
                    for (int idx : tris)
                        meshgl.triVerts.push_back(static_cast<uint32_t>(idx));

                    manifold::Manifold loaded(meshgl);

                    if (loaded.Status() != manifold::Manifold::Error::NoError)
                        throw std::runtime_error("Manifold import failed: mesh is not manifold "
                                                 "(open boundary, self-intersection, or degenerate geometry)");

                    double const import_ms = import_timer.elapsed_ms();

                    // disk write — not timed
                    std::string const file_path = out_dir + "/op_" + std::to_string(i) + ".obj";
                    {
                        manifold::MeshGL64 out_mesh = loaded.GetMeshGL64();
                        std::vector<double> out_verts = std::move(out_mesh.vertProperties);
                        std::vector<int> out_tris;
                        out_tris.reserve(out_mesh.triVerts.size());
                        for (uint32_t idx : out_mesh.triVerts)
                            out_tris.push_back(static_cast<int>(idx));
                        runner_mesh_helpers::saveToFileIndexed(file_path, out_verts, out_tris);
                    }

                    ssa.push_back(std::move(loaded));

                    op_res["status"]    = "success";
                    op_res["total_ms"]  = op_total_timer.elapsed_ms();
                    op_res["io_ms"]     = io_ms;
                    op_res["import_ms"] = import_ms;
                    op_res["file"]      = file_path;
                }
                else if (op_str == "boolean-union" || op_str == "boolean-intersection" || op_str == "boolean-difference")
                {
                    validate_op_boolean_binary(i, ops, "manifold runner");

                    auto const args = op.at("args").get<std::vector<int>>();

                    OpKind const kind = (op_str == "boolean-union")        ? OpKind::Union
                                      : (op_str == "boolean-intersection") ? OpKind::Intersection
                                                                           : OpKind::Difference;

                    manifold::Manifold const& lhs = ssa[static_cast<std::size_t>(args[0])];
                    manifold::Manifold const& rhs = ssa[static_cast<std::size_t>(args[1])];

                    // Boolean phase — timed as operation_ms.
                    Timer op_timer;
                    auto [result, ok] = apply_op(lhs, rhs, kind);
                    double const operation_ms = op_timer.elapsed_ms();

                    if (!ok)
                    {
                        op_res["status"]       = "invalid_input";
                        op_res["total_ms"]     = op_total_timer.elapsed_ms();
                        op_res["operation_ms"] = operation_ms;
                        op_res["error"]        = "Manifold::Boolean returned non-NoError status — "
                                                 "inputs may be non-manifold or otherwise invalid";
                        ssa.push_back(manifold::Manifold());
                        ops_result.push_back(op_res);
                        failed = true;
                        break;
                    }

                    // Export phase — timed as export_ms (conversion only).
                    Timer export_timer;
                    manifold::MeshGL64 out_mesh = result.GetMeshGL64();
                    std::vector<double> out_verts = std::move(out_mesh.vertProperties);
                    std::vector<int> out_tris;
                    out_tris.reserve(out_mesh.triVerts.size());
                    for (uint32_t idx : out_mesh.triVerts)
                        out_tris.push_back(static_cast<int>(idx));
                    double const export_ms = export_timer.elapsed_ms();

                    // Disk write — not timed.
                    std::string const file_path = out_dir + "/op_" + std::to_string(i) + ".obj";
                    runner_mesh_helpers::saveToFileIndexed(file_path, out_verts, out_tris);

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
                    ssa.push_back(manifold::Manifold());
                    failed = true;
                }
            }
            catch (runner_utils::unsupported_op const& e)
            {
                op_res["status"]   = "unsupported";
                op_res["total_ms"] = 0.0;
                op_res["error"]    = e.what();
                ops_result.push_back(op_res);
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
    runner_utils::Config const cfg = runner_utils::parse_args(argc, argv, "manifold_runner");
    return runner_utils::run_main_loop(cfg, execute_run);
}
