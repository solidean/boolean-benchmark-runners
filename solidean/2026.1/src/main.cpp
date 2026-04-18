// ---------------------------------------------------------------------------
// main.cpp — solidean_runner entry point
//
// Execution contract:
//   solidean_runner --request <request.json> --result <result.json>
//
// Uses the Solidean Community SDK (2026.1) C++17 API for exact CSG booleans.
//
// A single solidean::Context and solidean::ExactArithmetic (extent = 1000)
// are created once at startup and shared across all runs.
//
// SSA stores float32 indexed meshes (solidean::pos3 / solidean::idxtri).
// Each boolean op executes a single ctx->execute() call that imports both
// operands, performs the operation, and exports the result.
//
// Timing conventions (per-op fields):
//   load-mesh:
//     total_ms   — start before file open, end when float32 SSA entry is ready
//     io_ms      — file → double indexed mesh (runner_mesh_helpers::loadFromFileIndexed)
//     import_ms  — double → float32 pos3/idxtri conversion
//   boolean ops:
//     total_ms      — start when operands are ready, end after extraction
//     operation_ms  — entire ctx->execute() call (import + boolean + export setup)
//     export_ms     — blob->getPositionsF32 + blob->getTrianglesIndexed extraction
// ---------------------------------------------------------------------------

#include <nlohmann/json.hpp>
#include <runner_mesh_helpers/io.hh>
#include <runner_utils/cli.hh>
#include <runner_utils/ops.hh>
#include <runner_utils/run_loop.hh>
#include <runner_utils/timer.hh>
#include <solidean.hh>

#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using json = nlohmann::json;
using runner_utils::print_progress_op;
using runner_utils::Timer;
using runner_utils::validate_op_boolean_binary;

// ---------------------------------------------------------------------------
// Solidean global state
// ---------------------------------------------------------------------------

static std::unique_ptr<solidean::Context> g_ctx;

// ---------------------------------------------------------------------------
// Operation helpers
// ---------------------------------------------------------------------------

enum class OpKind
{
    Union,
    Intersection,
    Difference
};

// Execute one boolean operation via a single ctx->execute() call.
// Imports both operands, performs the op, and returns the exported indexed result.
// Timing is captured via captured references (operation_ms = full execute duration).
static std::shared_ptr<solidean::Mesh> apply_bool_op(solidean::Mesh const& lhs,
                                                     solidean::Mesh const& rhs,
                                                     OpKind kind,
                                                     solidean::ExactArithmetic& arith,
                                                     double& out_operation_ms)
{
    Timer op_timer;
    auto mesh = g_ctx->execute( //
        arith,
        [&](solidean::Operation& op)
        {
            auto meshA = op.input(lhs);
            auto meshB = op.input(rhs);

            switch (kind)
            {
            case OpKind::Union: return op.output(op.union_(meshA, meshB));
            case OpKind::Intersection: return op.output(op.intersection(meshA, meshB));
            case OpKind::Difference: return op.output(op.difference(meshA, meshB));
            }

            std::cerr << "Unsupported operation type\n";
            std::exit(1);
        });
    out_operation_ms = op_timer.elapsed_ms();

    return mesh;
}

// ---------------------------------------------------------------------------
// Save helper — converts float32 SSA mesh to double indexed for OBJ output
// ---------------------------------------------------------------------------

static void save_ssa_mesh(solidean::Mesh const& mesh,
                          std::string const& path,
                          solidean::ExactArithmetic& arith,
                          double& out_export_ms)
{
    Timer op_timer;
    auto blob = g_ctx->execute( //
        arith, [&](solidean::Operation& op) { return op.exportToIndexedTrianglesF32(op.input(mesh)); });
    out_export_ms = op_timer.elapsed_ms();

    auto verts_f = blob->getPositionsF32();
    auto tris = blob->getTrianglesIndexed();

    std::vector<double> verts_d;
    verts_d.reserve(verts_f.size() * 3);
    for (auto const& v : verts_f)
    {
        verts_d.push_back(static_cast<double>(v.x));
        verts_d.push_back(static_cast<double>(v.y));
        verts_d.push_back(static_cast<double>(v.z));
    }

    std::vector<int> tris_i;
    tris_i.reserve(tris.size() * 3);
    for (auto const& t : tris)
    {
        tris_i.push_back(t.i0);
        tris_i.push_back(t.i1);
        tris_i.push_back(t.i2);
    }

    runner_mesh_helpers::saveToFileIndexed(path, verts_d, tris_i);
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

        // Derive arithmetic extent from the bounding box.
        auto const& bb = run_req.at("bounding_box");
        auto const& bb_min = bb.at("min");
        auto const& bb_max = bb.at("max");
        double max_abs = 0.0;
        for (int k = 0; k < 3; ++k)
        {
            max_abs = std::max(max_abs, std::abs(bb_min[k].get<double>()));
            max_abs = std::max(max_abs, std::abs(bb_max[k].get<double>()));
        }
        auto arith = g_ctx->createExactArithmetic(static_cast<float>(max_abs));

        // SSA storage: ssa[i] = result of op i.
        std::vector<std::shared_ptr<solidean::Mesh>> ssa;
        ssa.reserve(ops.size());

        for (std::size_t i = 0; i < ops.size() && !failed; ++i)
        {
            auto const& op = ops[i];
            std::string op_str = op.at("op").get<std::string>();
            json op_res;
            Timer op_total_timer;

            print_progress_op(i, ops);

            try
            {
                if (op_str == "load-mesh")
                {
                    std::string const path = op.at("path").get<std::string>();
                    std::string const name = op.value("name", "");

                    // io phase — file → float indexed mesh
                    Timer io_timer;
                    auto [verts_d, tris_i] = runner_mesh_helpers::loadFromFileIndexed(path);
                    auto verts_f = std::vector<float>(verts_d.size());
                    for (size_t i = 0; i < verts_d.size(); ++i)
                        verts_f[i] = verts_d[i];
                    double const io_ms = io_timer.elapsed_ms();

                    // import phase — double → float32 solidean types
                    Timer import_timer;
                    auto v_span = solidean::span<solidean::pos3>(          //
                        reinterpret_cast<solidean::pos3*>(verts_f.data()), //
                        verts_f.size() / 3);
                    auto t_span = solidean::span<solidean::idxtri>(         //
                        reinterpret_cast<solidean::idxtri*>(tris_i.data()), //
                        tris_i.size() / 3);
                    auto mesh
                        = g_ctx->createMeshFromIndexedTrianglesF32(v_span, t_span, *arith, solidean::MeshType::Solid);
                    double const import_ms = import_timer.elapsed_ms();

                    // Disk write — not timed
                    std::string const file_path = out_dir + "/op_" + std::to_string(i) + ".obj";
                    {
                        double discard_export_ms = 0.0;
                        save_ssa_mesh(*mesh, file_path, *arith, discard_export_ms);
                    }

                    ssa.push_back(std::move(mesh));

                    op_res["status"] = "success";
                    op_res["total_ms"] = op_total_timer.elapsed_ms();
                    op_res["io_ms"] = io_ms;
                    op_res["import_ms"] = import_ms;
                    op_res["file"] = file_path;
                }
                else if (op_str == "boolean-union" || op_str == "boolean-intersection" || op_str == "boolean-difference")
                {
                    validate_op_boolean_binary(i, ops, "solidean_runner");

                    auto const args = op.at("args").get<std::vector<int>>();

                    OpKind const kind = (op_str == "boolean-union")        ? OpKind::Union
                                      : (op_str == "boolean-intersection") ? OpKind::Intersection
                                                                           : OpKind::Difference;

                    solidean::Mesh const& lhs = *ssa[args[0]];
                    solidean::Mesh const& rhs = *ssa[args[1]];

                    double operation_ms = 0.0;
                    auto result = apply_bool_op(lhs, rhs, kind, *arith, operation_ms);

                    double export_ms = 0.0;
                    std::string const file_path = out_dir + "/op_" + std::to_string(i) + ".obj";
                    save_ssa_mesh(*result, file_path, *arith, export_ms);

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
                failed = true;
            }
            catch (solidean::exception const& e)
            {
                op_res["status"] = "crash";
                op_res["total_ms"] = op_total_timer.elapsed_ms();
                op_res["error"] = e.what();
                failed = true;
            }
            catch (std::exception const& e)
            {
                op_res["status"] = "crash";
                op_res["total_ms"] = op_total_timer.elapsed_ms();
                op_res["error"] = e.what();
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
    runner_utils::Config const cfg = runner_utils::parse_args(argc, argv, "solidean_runner");

    // -----------------------------------------------------------------------
    // Initialise Solidean global state
    // -----------------------------------------------------------------------
    try
    {
        g_ctx = solidean::Context::create();
    }
    catch (std::exception const& e)
    {
        std::cerr << "[fatal] Failed to initialise Solidean context: " << e.what() << "\n";
        return 1;
    }

    return runner_utils::run_main_loop(cfg, execute_run);
}
