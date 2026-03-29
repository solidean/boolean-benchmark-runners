// ---------------------------------------------------------------------------
// main.cpp — nef_runner entry point
//
// Execution contract:
//   nef_runner --request <request.json> --result <result.json>
//              [--regularize | --no-regularize]
//
// The runner reads a request batch, executes all runs, and writes a result
// batch.  Failures inside individual runs are caught and reported as
// structured status fields; the runner always attempts to write the result
// file before exiting.
//
// This runner is binary-only: boolean ops with 3+ args return 'unsupported'.
// The meta-runner generates sequences of binary ops for iterated booleans.
//
// Timing conventions (per-op fields):
//   load-mesh:
//     total_ms   — start before file open, end when handle is in memory
//     io_ms      — file → in-memory OFF string
//                  (.off: raw bytes read; .obj/.stl: soup → OFF text)
//     import_ms  — OFF string → Nef_polyhedron_3 via OFF_to_nef_3
//                  (includes OFF text parsing, excludes file I/O)
//   boolean ops:
//     total_ms      — start when native operands are ready, end when result is ready
//     operation_ms  — boolean computation only (join/intersection/difference,
//                     plus optional regularization)
//     export_ms     — Nef_polyhedron_3 → IndexedMesh
//                     (convert_nef_polyhedron_to_polygon_soup + to_double;
//                     excludes disk write)
// ---------------------------------------------------------------------------

#include "mesh_io.hh"
#include "runner_types.hh"

#include <runner_utils/cli.hh>
#include <runner_utils/ops.hh>
#include <runner_utils/run_loop.hh>
#include <runner_utils/timer.hh>

#include <nlohmann/json.hpp>

#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

using json = nlohmann::json;
using runner_utils::Timer;
using runner_utils::print_progress_op;
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

static Nef apply_op(Nef lhs, Nef const& rhs, OpKind kind)
{
    switch (kind)
    {
    case OpKind::Union: return lhs.join(rhs);
    case OpKind::Intersection: return lhs.intersection(rhs);
    case OpKind::Difference: return lhs.difference(rhs);
    }
    // Unreachable, but suppress compiler warning.
    throw std::logic_error("unhandled OpKind");
}

// ---------------------------------------------------------------------------
// CLI parsing
// ---------------------------------------------------------------------------

struct NefConfig : runner_utils::Config
{
    bool regularize = false;
};

static NefConfig parse_args(int argc, char** argv)
{
    NefConfig cfg;
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if ((arg == "--request" || arg == "-r") && i + 1 < argc)
            cfg.request_path = argv[++i];
        else if (arg == "--result" && i + 1 < argc)
            cfg.result_path = argv[++i];
        else if (arg == "--regularize")
            cfg.regularize = true;
        else if (arg == "--no-regularize")
            cfg.regularize = false;
        else
            std::cerr << "[warn] Unknown argument: " << arg << "\n";
    }
    if (cfg.request_path.empty() || cfg.result_path.empty())
    {
        std::cerr << "Usage: nef_runner --request <req.json> --result <res.json>"
                     " [--regularize | --no-regularize]\n";
        std::exit(1);
    }
    return cfg;
}

// ---------------------------------------------------------------------------
// Per-run execution
// ---------------------------------------------------------------------------

static json execute_run(NefConfig const& cfg, json const& run_req)
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
        // Runners are encouraged to keep exact intermediate representations
        // alive across ops rather than re-loading from disk between steps.
        std::vector<Nef> ssa;
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

                    // io phase — file → OFF text string
                    Timer io_timer;
                    std::string off_str = load_raw_off_string(path);
                    double const io_ms = io_timer.elapsed_ms();

                    // import phase — OFF text string → Nef_polyhedron_3
                    Timer import_timer;
                    Nef loaded = import_nef(off_str, skipped);
                    double const import_ms = import_timer.elapsed_ms();

                    // disk write — not timed
                    std::string const file_path = out_dir + "/op_" + std::to_string(i) + ".obj";
                    export_nef(loaded, file_path);

                    if (skipped > 0)
                        throw std::runtime_error("import_nef skipped " + std::to_string(skipped) + " facets");

                    ssa.push_back(std::move(loaded));

                    op_res["status"] = "success";
                    op_res["total_ms"] = op_total_timer.elapsed_ms();
                    op_res["io_ms"] = io_ms;
                    op_res["import_ms"] = import_ms;
                    op_res["file"] = file_path;
                }
                else if (op_str == "boolean-union" || op_str == "boolean-intersection" || op_str == "boolean-difference")
                {
                    validate_op_boolean_binary(i, ops, "nef runner");

                    auto const args = op.at("args").get<std::vector<int>>();

                    OpKind const kind = (op_str == "boolean-union")        ? OpKind::Union
                                      : (op_str == "boolean-intersection") ? OpKind::Intersection
                                                                           : OpKind::Difference;

                    // Boolean phase — timed as operation_ms.
                    Timer op_timer;
                    Nef result = apply_op(ssa[args[0]], ssa[args[1]], kind);
                    if (cfg.regularize)
                        result = result.regularization();
                    double const operation_ms = op_timer.elapsed_ms();

                    // Export phase — timed as export_ms (conversion only).
                    Timer export_timer;
                    IndexedMesh im = nef_to_indexed(result);
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
                op_res["status"] = "crash";
                op_res["total_ms"] = op_total_timer.elapsed_ms();
                op_res["error"] = e.what();
                ops_result.push_back(op_res);
                failed = true;
            }
            catch (...)
            {
                op_res["status"] = "crash";
                op_res["total_ms"] = op_total_timer.elapsed_ms();
                op_res["error"] = "unknown exception";
                ssa.push_back(Nef());
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
    NefConfig const cfg = parse_args(argc, argv);
    return runner_utils::run_main_loop(cfg, execute_run, {{"regularize", cfg.regularize}});
}
