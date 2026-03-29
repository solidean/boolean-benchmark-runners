// ---------------------------------------------------------------------------
// main.cpp — corefine_runner entry point
//
// Execution contract:
//   corefine_runner --request <request.json> --result <result.json>
//
// Uses CGAL::Polygon_mesh_processing::corefine_and_compute_{union,intersection,
// difference} on Surface_mesh<EPEC> meshes.
//
// Requires closed, manifold, non-self-intersecting inputs.
// Invalid meshes are caught at load time (is_closed / is_valid_polygon_mesh).
// A false return from corefine_and_compute_* is reported as invalid_input.
//
// This runner is binary-only: boolean ops with 3+ args return 'unsupported'.
// The meta-runner generates sequences of binary ops for iterated booleans.
//
// Timing conventions (per-op fields):
//   load-mesh:
//     total_ms   — start before file open, end when handle is in memory
//     io_ms      — file → IndexedMesh (file I/O, format parse, fan triangulation)
//     import_ms  — IndexedMesh → Surface_mesh<EPEC> (EPEC promotion, repair,
//                  orient, polygon_soup_to_polygon_mesh, validation)
//   boolean ops:
//     total_ms      — start when native operands are ready, end when result is ready
//     operation_ms  — boolean computation only (corefine_and_compute_*)
//     export_ms     — Surface_mesh<EPEC> → IndexedMesh (EPEC→double, face
//                     traversal, fan triangulation; excludes disk write)
// ---------------------------------------------------------------------------

#include "mesh_io.hh"
#include "runner_types.hh"

#include <CGAL/Polygon_mesh_processing/corefinement.h>
#include <nlohmann/json.hpp>
#include <runner_utils/cli.hh>
#include <runner_utils/ops.hh>
#include <runner_utils/run_loop.hh>
#include <runner_utils/timer.hh>

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

// Both meshes are taken by value — corefine_and_compute_* modifies its inputs
// in place as a side effect of corefinement.  Passing by value ensures the
// SSA entries remain unmodified after each boolean op.
// Returns {result_mesh, success}.  success==false means the function returned
// false (inputs not geometrically valid for the requested operation).
static std::pair<Mesh, bool> apply_op(Mesh lhs, Mesh rhs, OpKind kind)
{
    namespace PMP = CGAL::Polygon_mesh_processing;
    Mesh out;
    bool ok = false;
    switch (kind)
    {
    case OpKind::Union: ok = PMP::corefine_and_compute_union(lhs, rhs, out); break;
    case OpKind::Intersection: ok = PMP::corefine_and_compute_intersection(lhs, rhs, out); break;
    case OpKind::Difference: ok = PMP::corefine_and_compute_difference(lhs, rhs, out); break;
    }
    return {std::move(out), ok};
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
        std::vector<Mesh> ssa;
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
                    std::string const name = op.value("name", "");

                    std::size_t skipped = 0;

                    // io phase — file → IndexedMesh
                    Timer io_timer;
                    IndexedMesh raw = load_raw(path, skipped);
                    double const io_ms = io_timer.elapsed_ms();

                    // import phase — IndexedMesh → Surface_mesh<EPEC>
                    Timer import_timer;
                    Mesh loaded = import_mesh(raw);
                    double const import_ms = import_timer.elapsed_ms();

                    // disk write — not timed
                    std::string const file_path = out_dir + "/op_" + std::to_string(i) + ".obj";
                    export_mesh(loaded, file_path);

                    if (skipped > 0)
                        throw std::runtime_error("load_raw skipped " + std::to_string(skipped) + " facets");

                    ssa.push_back(std::move(loaded));

                    op_res["status"] = "success";
                    op_res["total_ms"] = op_total_timer.elapsed_ms();
                    op_res["io_ms"] = io_ms;
                    op_res["import_ms"] = import_ms;
                    op_res["file"] = file_path;
                }
                else if (op_str == "boolean-union" || op_str == "boolean-intersection" || op_str == "boolean-difference")
                {
                    validate_op_boolean_binary(i, ops, "corefine runner");

                    auto const args = op.at("args").get<std::vector<int>>();

                    OpKind const kind = (op_str == "boolean-union")        ? OpKind::Union
                                      : (op_str == "boolean-intersection") ? OpKind::Intersection
                                                                           : OpKind::Difference;

                    // Copy both operands — corefinement mutates in place, so we
                    // must copy to keep SSA entries intact for potential later ops.
                    Mesh lhs_copy = ssa[args[0]];
                    Mesh rhs_copy = ssa[args[1]];

                    // Boolean phase — timed as operation_ms.
                    Timer op_timer;
                    auto [result, ok] = apply_op(std::move(lhs_copy), std::move(rhs_copy), kind);
                    double const operation_ms = op_timer.elapsed_ms();

                    if (!ok)
                    {
                        op_res["status"] = "invalid_input";
                        op_res["total_ms"] = op_total_timer.elapsed_ms();
                        op_res["operation_ms"] = operation_ms;
                        op_res["error"] = "corefine_and_compute returned false — inputs may be self-intersecting, "
                                          "non-manifold, or otherwise invalid";
                        ssa.push_back(Mesh());
                        ops_result.push_back(op_res);
                        failed = true;
                        break;
                    }

                    // Export phase — timed as export_ms (conversion only).
                    Timer export_timer;
                    IndexedMesh im = mesh_to_indexed(result);
                    double const export_ms = export_timer.elapsed_ms();

                    // Disk write — not timed.
                    std::string const file_path = out_dir + "/op_" + std::to_string(i) + ".obj";
                    write_indexed(im, file_path);

                    ssa.push_back(std::move(result));

                    op_res["status"] = "success";
                    op_res["total_ms"] = op_total_timer.elapsed_ms();
                    op_res["operation_ms"] = operation_ms;
                    op_res["export_ms"] = export_ms;
                    op_res["file"] = file_path;
                }
                else
                {
                    op_res["status"] = "unsupported";
                    op_res["total_ms"] = 0.0;
                    op_res["error"] = "unknown op: " + op_str;
                    ssa.push_back(Mesh());
                    failed = true;
                }
            }
            catch (runner_utils::unsupported_op const& e)
            {
                op_res["status"] = "unsupported";
                op_res["total_ms"] = 0.0;
                op_res["error"] = e.what();
                ops_result.push_back(op_res);
                failed = true;
            }
            catch (std::exception const& e)
            {
                // Topology validation failures from load_mesh → invalid_input.
                // Unexpected exceptions from boolean ops → crash.
                std::string const what = e.what();
                bool const is_validation = what.find("not closed") != std::string::npos
                                        || what.find("not a valid polygon mesh") != std::string::npos;
                op_res["status"] = (op_str == "load-mesh" && is_validation) ? "invalid_input" : "crash";
                op_res["total_ms"] = op_total_timer.elapsed_ms();
                op_res["error"] = what;
                failed = true;
            }
            catch (...)
            {
                op_res["status"] = "crash";
                op_res["total_ms"] = op_total_timer.elapsed_ms();
                op_res["error"] = "unknown exception";
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
        run_res["error"] = e.what();
    }
    catch (...)
    {
        run_res["status"] = "crash";
        run_res["error"] = "unknown exception";
    }

    run_res["duration_ms"] = total_timer.elapsed_ms();
    run_res["ops"] = ops_result;
    return run_res;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char** argv)
{
    runner_utils::Config const cfg = runner_utils::parse_args(argc, argv, "corefine_runner");
    return runner_utils::run_main_loop(cfg, execute_run);
}
