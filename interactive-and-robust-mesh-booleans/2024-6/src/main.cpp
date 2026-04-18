// ---------------------------------------------------------------------------
// main.cpp — iarmb_runner entry point
//
// Execution contract:
//   iarmb_runner --request <request.json> --result <result.json>
//
// Calls booleanPipeline() from code/booleans.h:
//   booleanPipeline(in_coords, in_tris, in_labels, op, out_coords, out_tris, out_labels)
//
// Input data model (binary boolean A op B):
//   in_coords = A_coords + B_coords   (flat double xyzxyz...)
//   in_tris   = A_tris + B_tris       (flat uint i0i1i2..., B indices offset by |A verts|)
//   in_labels = [0,...,0, 1,...,1]    (per-triangle: 0 = mesh A, 1 = mesh B)
//   op        = UNION / INTERSECTION / SUBTRACTION (A minus B)
//
// The library uses a "header-includes-cpp" unity build pattern, so including
// booleans.h pulls in all implementation code transitively.
//
// Timing conventions (per-op fields):
//   load-mesh:
//     total_ms   — start before file open, end when mesh is in SSA
//     io_ms      — file → flat double verts + int indices (runner_mesh_helpers)
//     import_ms  — int indices → uint (cast to match library's vector<uint>)
//   boolean ops:
//     total_ms      — start when native operands are ready, end when result is ready
//     operation_ms  — booleanPipeline() call only
//     export_ms     — uint result indices → int for saveToFileIndexed; excludes disk write
// ---------------------------------------------------------------------------

#include <booleans.h>
#include <nlohmann/json.hpp>
#include <runner_mesh_helpers/io.hh>
#include <runner_utils/cli.hh>
#include <runner_utils/ops.hh>
#include <runner_utils/run_loop.hh>
#include <runner_utils/timer.hh>

#include <bitset>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>


using json = nlohmann::json;
using runner_utils::print_progress_op;
using runner_utils::Timer;
using runner_utils::validate_op_boolean_binary;

// ---------------------------------------------------------------------------
// SSA entry — the library's native representation
// ---------------------------------------------------------------------------

struct IarmbMesh
{
    std::vector<double> coords; // flat xyzxyz...
    std::vector<uint> tris;     // flat i0i1i2...
};

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
        std::vector<IarmbMesh> ssa;
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
                    auto [verts, int_tris] = runner_mesh_helpers::loadFromFileIndexed(path);
                    double const io_ms = io_timer.elapsed_ms();

                    // import phase — int indices → uint (library expects vector<uint>)
                    Timer import_timer;
                    std::vector<uint> uint_tris(int_tris.size());
                    for (std::size_t k = 0; k < int_tris.size(); ++k)
                        uint_tris[k] = static_cast<uint>(int_tris[k]);
                    double const import_ms = import_timer.elapsed_ms();

                    // disk write — not timed
                    std::string const file_path = out_dir + "/op_" + std::to_string(i) + ".obj";
                    runner_mesh_helpers::saveToFileIndexed(file_path, verts, int_tris);

                    ssa.push_back({std::move(verts), std::move(uint_tris)});

                    op_res["status"] = "success";
                    op_res["total_ms"] = op_total_timer.elapsed_ms();
                    op_res["io_ms"] = io_ms;
                    op_res["import_ms"] = import_ms;
                    op_res["file"] = file_path;
                }
                else if (op_str == "boolean-union" || op_str == "boolean-intersection" || op_str == "boolean-difference")
                {
                    validate_op_boolean_binary(i, ops, "iarmb runner");

                    auto const args = op.at("args").get<std::vector<int>>();

                    BoolOp const kind = (op_str == "boolean-union")        ? UNION
                                      : (op_str == "boolean-intersection") ? INTERSECTION
                                                                           : SUBTRACTION;

                    IarmbMesh const& lhs = ssa[static_cast<std::size_t>(args[0])];
                    IarmbMesh const& rhs = ssa[static_cast<std::size_t>(args[1])];

                    // Build combined input (A then B).
                    uint const lhs_n_verts = static_cast<uint>(lhs.coords.size() / 3);
                    uint const lhs_n_tris = static_cast<uint>(lhs.tris.size() / 3);
                    uint const rhs_n_tris = static_cast<uint>(rhs.tris.size() / 3);

                    std::vector<double> in_coords;
                    in_coords.reserve(lhs.coords.size() + rhs.coords.size());
                    in_coords.insert(in_coords.end(), lhs.coords.begin(), lhs.coords.end());
                    in_coords.insert(in_coords.end(), rhs.coords.begin(), rhs.coords.end());

                    std::vector<uint> in_tris;
                    in_tris.reserve(lhs.tris.size() + rhs.tris.size());
                    in_tris.insert(in_tris.end(), lhs.tris.begin(), lhs.tris.end());
                    for (uint idx : rhs.tris)
                        in_tris.push_back(idx + lhs_n_verts);

                    std::vector<uint> in_labels;
                    in_labels.reserve(lhs_n_tris + rhs_n_tris);
                    in_labels.insert(in_labels.end(), lhs_n_tris, 0u);
                    in_labels.insert(in_labels.end(), rhs_n_tris, 1u);

                    // Boolean phase — timed as operation_ms.
                    std::vector<double> out_coords;
                    std::vector<uint> out_tris;
                    std::vector<std::bitset<NBIT>> out_labels; // unused by benchmark

                    Timer op_timer;
                    booleanPipeline(in_coords, in_tris, in_labels, kind, out_coords, out_tris, out_labels);
                    double const operation_ms = op_timer.elapsed_ms();

                    // Export phase — timed as export_ms (uint → int conversion only).
                    Timer export_timer;
                    std::vector<int> int_out_tris(out_tris.size());
                    for (std::size_t k = 0; k < out_tris.size(); ++k)
                        int_out_tris[k] = static_cast<int>(out_tris[k]);
                    double const export_ms = export_timer.elapsed_ms();

                    // Disk write — not timed.
                    std::string const file_path = out_dir + "/op_" + std::to_string(i) + ".obj";
                    runner_mesh_helpers::saveToFileIndexed(file_path, out_coords, int_out_tris);

                    ssa.push_back({std::move(out_coords), std::move(out_tris)});

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
                    ssa.push_back({});
                    failed = true;
                }
            }
            catch (runner_utils::unsupported_op const& e)
            {
                op_res["status"] = "unsupported";
                op_res["total_ms"] = 0.0;
                op_res["error"] = e.what();
                failed = true;
            }
            catch (std::exception const& e)
            {
                op_res["status"] = "crash";
                op_res["total_ms"] = op_total_timer.elapsed_ms();
                op_res["error"] = e.what();
                ssa.push_back({});
                failed = true;
            }
            catch (...)
            {
                op_res["status"] = "crash";
                op_res["total_ms"] = op_total_timer.elapsed_ms();
                op_res["error"] = "unknown exception";
                ssa.push_back({});
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
    runner_utils::Config const cfg = runner_utils::parse_args(argc, argv, "iarmb_runner");
    return runner_utils::run_main_loop(cfg, execute_run);
}
